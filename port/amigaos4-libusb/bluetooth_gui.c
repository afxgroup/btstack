/*
 * Bluetooth device manager for AmigaOS 4.
 *
 * A ReAction front end for BluetoothService. It holds no Bluetooth state of its
 * own and links against none of BTstack: everything it shows comes from the
 * service over the public message port, and everything it does is a command
 * sent there. The service can be started, stopped or restarted underneath it
 * and this keeps working - it simply finds the port again next time it needs
 * it.
 *
 * Two lists, because they answer two different questions. What is nearby right
 * now, which only scanning can tell us and which changes constantly, and what
 * we know about, which persists and is what the user actually manages.
 */

#include <exec/types.h>
#include <exec/exec.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/listbrowser.h>
#include <gadgets/button.h>
#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/listbrowser.h>
#include <proto/button.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bluetooth_service.h"

/* ------------------------------------------------------------------------- */
/* libraries                                                                 */

/*
 * These are declared by the proto headers and defined by nobody: unlike exec,
 * none of them is opened for us, and the ReAction macros reach straight through
 * the interface pointers - ListBrowserObject is IIntuition->NewObject() with a
 * class from IListBrowser. So they are ours to fill in and ours to give back.
 */
struct Library *IntuitionBase, *WindowBase, *LayoutBase, *ListBrowserBase, *ButtonBase;
struct IntuitionIFace   *IIntuition;
struct WindowIFace      *IWindow;
struct LayoutIFace      *ILayout;
struct ListBrowserIFace *IListBrowser;
struct ButtonIFace      *IButton;

/*
 * The ReAction classes are opened by hand.
 *
 * Unlike intuition or dos these are not opened for us, and each one is a
 * library whose "main" interface has to be fetched before its macros work -
 * ListBrowserObject and the rest go through it.
 */
static struct Library * open_class(const char * name, uint32 version, APTR * interface){
    struct Library * base = OpenLibrary(name, version);
    if (base == NULL) return NULL;
    *interface = GetInterface(base, "main", 1, NULL);
    if (*interface == NULL){
        CloseLibrary(base);
        return NULL;
    }
    return base;
}

static void close_class(struct Library * base, APTR interface){
    if (interface != NULL) DropInterface((struct Interface *) interface);
    if (base != NULL)      CloseLibrary(base);
}

/* ------------------------------------------------------------------------- */
/* talking to the service                                                    */

#define MAX_DEVICES 32

static struct MsgPort * reply_port;
static struct MsgPort * event_port;
static bool             subscribed;

/*
 * Send one command and wait for the answer.
 *
 * The port is looked up every time rather than kept, so the service starting
 * later, or being restarted, needs no action here at all. Everything happens
 * inside Forbid() up to the PutMsg: between finding a port and using it, the
 * task owning it could be gone, and a message sent to a freed port is not an
 * error that can be recovered from afterwards.
 */
static bt_result_t bt_command(bt_command_t command, const uint8 * addr, uint8 addr_type,
                              BTDeviceInfo * devices, uint32 devices_max, uint32 * devices_count){

    BTServiceMsg msg;

    memset(&msg, 0, sizeof(msg));
    msg.bsm_Message.mn_Node.ln_Type = NT_MESSAGE;
    msg.bsm_Message.mn_Length       = sizeof(msg);
    msg.bsm_Message.mn_ReplyPort    = reply_port;
    msg.bsm_Version                 = BLUETOOTH_SERVICE_VERSION;
    msg.bsm_Command                 = command;
    msg.bsm_Devices                 = devices;
    msg.bsm_DevicesMax              = devices_max;
    msg.bsm_EventPort               = event_port;
    if (addr != NULL){
        memcpy(msg.bsm_Addr, addr, 6);
        msg.bsm_AddrType = addr_type;
    }

    Forbid();
    struct MsgPort * service = FindPort(BLUETOOTH_SERVICE_PORT_NAME);
    if (service != NULL){
        PutMsg(service, &msg.bsm_Message);
    }
    Permit();

    if (service == NULL) return BT_RESULT_NO_CONTROLLER;

    WaitPort(reply_port);
    while (GetMsg(reply_port) == NULL) {
        WaitPort(reply_port);
    }

    if (devices_count != NULL) *devices_count = msg.bsm_DevicesCount;
    return msg.bsm_Result;
}

static bool service_present(void){
    Forbid();
    bool present = FindPort(BLUETOOTH_SERVICE_PORT_NAME) != NULL;
    Permit();
    return present;
}

/* ------------------------------------------------------------------------- */
/* what we know                                                              */

/*
 * Devices seen while scanning are kept here rather than asked for.
 *
 * The service does not keep a table entry for a device no handler can drive -
 * with LE privacy addresses rotating every few minutes the supply is endless
 * and it would fill up in seconds - but it does announce every sighting. So a
 * list of what is nearby is built from those announcements, which is also what
 * makes it live: an entry appears the moment its device is heard from.
 */
static BTDeviceInfo nearby[MAX_DEVICES];
static uint32       nearby_count;

static BTDeviceInfo known[MAX_DEVICES];
static uint32       known_count;

static void nearby_update(const BTDeviceInfo * device){
    uint32 i;
    for (i = 0; i < nearby_count; i++){
        if (memcmp(nearby[i].bd_addr, device->bd_addr, 6) == 0){
            nearby[i] = *device;
            return;
        }
    }
    if (nearby_count >= MAX_DEVICES) return;
    nearby[nearby_count++] = *device;
}

static void nearby_remove(const BTDeviceInfo * device){
    uint32 i;
    for (i = 0; i < nearby_count; i++){
        if (memcmp(nearby[i].bd_addr, device->bd_addr, 6) != 0) continue;
        nearby[i] = nearby[nearby_count - 1];
        nearby_count--;
        return;
    }
}

/*
 * The service's table holds more than the devices we know: a supported one that
 * has only been seen is in there too, waiting to be paired. Those belong in the
 * nearby list, so this keeps only what has actually been bonded - otherwise
 * "Known devices" would list things the user has never agreed to.
 */
static void known_refresh(void){
    BTDeviceInfo all[MAX_DEVICES];
    uint32       all_count = 0;
    uint32       i;

    known_count = 0;
    if (bt_command(BTCMD_LIST_DEVICES, NULL, 0, all, MAX_DEVICES, &all_count) != BT_RESULT_OK){
        return;
    }

    for (i = 0; i < all_count; i++){
        if (all[i].state < BT_DEVICE_STATE_BONDED) continue;
        known[known_count++] = all[i];
    }
}

/* ------------------------------------------------------------------------- */
/* presenting it                                                             */

static const char * kind_name(bt_device_kind_t kind){
    switch (kind){
        case BT_DEVICE_KIND_MOUSE:    return "Mouse";
        case BT_DEVICE_KIND_KEYBOARD: return "Keyboard";
        case BT_DEVICE_KIND_GAMEPAD:  return "Gamepad";
        case BT_DEVICE_KIND_AUDIO:    return "Audio";
        case BT_DEVICE_KIND_SERIAL:   return "Serial";
        default:                      return "";
    }
}

static const char * state_name(bt_device_state_t state){
    switch (state){
        case BT_DEVICE_STATE_FOUND:      return "Found";
        case BT_DEVICE_STATE_BONDED:     return "Paired";
        case BT_DEVICE_STATE_CONNECTING: return "Connecting";
        case BT_DEVICE_STATE_CONNECTED:  return "Connected";
        case BT_DEVICE_STATE_IN_USE:     return "In use";
        default:                         return "";
    }
}

static void addr_to_str(const uint8 * addr, char * out){
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

enum {
    GID_NEARBY = 1,
    GID_KNOWN,
    GID_SCAN,
    GID_PAIR,
    GID_CONNECT,
    GID_DISCONNECT,
    GID_FORGET,
};

static struct List nearby_labels;
static struct List known_labels;

static Object * win_obj;
static Object * gad_nearby;
static Object * gad_known;
static Object * gad_scan;
static struct Window * window;

/* the listbrowser must not be looking at a list while it is being rebuilt */
static void list_detach(Object * gadget, struct List * list){
    SetGadgetAttrs((struct Gadget *) gadget, window, NULL,
                               LISTBROWSER_Labels, NULL, TAG_END);
    FreeListBrowserList(list);
    NewList(list);
}

static void list_attach(Object * gadget, struct List * list){
    SetGadgetAttrs((struct Gadget *) gadget, window, NULL,
                               LISTBROWSER_Labels, list, TAG_END);
}

static void nearby_show(void){
    uint32 i;

    list_detach(gad_nearby, &nearby_labels);

    for (i = 0; i < nearby_count; i++){
        char address[18], rssi[16];
        addr_to_str(nearby[i].bd_addr, address);
        sprintf(rssi, "%d dBm", nearby[i].rssi);

        struct Node * node = AllocListBrowserNode(4,
            LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, nearby[i].name, TAG_END,
            LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, address, TAG_END,
            LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, rssi, TAG_END,
            LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, kind_name(nearby[i].kind), TAG_END,
            TAG_END);
        if (node != NULL) AddTail(&nearby_labels, node);
    }

    list_attach(gad_nearby, &nearby_labels);
}

static void known_show(void){
    uint32 i;

    list_detach(gad_known, &known_labels);

    for (i = 0; i < known_count; i++){
        char address[18];
        addr_to_str(known[i].bd_addr, address);

        struct Node * node = AllocListBrowserNode(4,
            LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, known[i].name, TAG_END,
            LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, address, TAG_END,
            LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, kind_name(known[i].kind), TAG_END,
            LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, state_name(known[i].state), TAG_END,
            TAG_END);
        if (node != NULL) AddTail(&known_labels, node);
    }

    list_attach(gad_known, &known_labels);
}

/* which row is selected, or -1 */
static int32 selected_row(Object * gadget){
    uint32 row = 0;
    GetAttr(LISTBROWSER_Selected, gadget, &row);
    return (row == (uint32) ~0) ? -1 : (int32) row;
}

/* ------------------------------------------------------------------------- */

static struct ColumnInfo * make_columns(const char * a, const char * b,
                                        const char * c, const char * d){
    struct ColumnInfo * ci = AllocLBColumnInfo(4,
        LBCIA_Column, 0, LBCIA_Title, a, LBCIA_Weight, 34, TAG_END,
        LBCIA_Column, 1, LBCIA_Title, b, LBCIA_Weight, 33, TAG_END,
        LBCIA_Column, 2, LBCIA_Title, c, LBCIA_Weight, 16, TAG_END,
        LBCIA_Column, 3, LBCIA_Title, d, LBCIA_Weight, 17, TAG_END,
        TAG_END);
    return ci;
}

int main(void){

    IntuitionBase   = open_class("intuition.library", 51,     (APTR *) &IIntuition);
    WindowBase      = open_class("window.class", 53,          (APTR *) &IWindow);
    LayoutBase      = open_class("gadgets/layout.gadget", 53,     (APTR *) &ILayout);
    ListBrowserBase = open_class("gadgets/listbrowser.gadget", 53,(APTR *) &IListBrowser);
    ButtonBase      = open_class("gadgets/button.gadget", 53,     (APTR *) &IButton);

    if ((IntuitionBase == NULL) || (WindowBase == NULL) || (LayoutBase == NULL) ||
        (ListBrowserBase == NULL) || (ButtonBase == NULL)){
        printf("Cannot open the ReAction classes this needs.\n");
        return RETURN_FAIL;
    }

    reply_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    event_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if ((reply_port == NULL) || (event_port == NULL)){
        printf("Cannot create the message ports.\n");
        return RETURN_FAIL;
    }

    NewList(&nearby_labels);
    NewList(&known_labels);

    if (!service_present()){
        printf("BluetoothService is not running - start it first.\n");
    }

    gad_nearby = ListBrowserObject,
        GA_ID,                   GID_NEARBY,
        GA_RelVerify,            TRUE,
        LISTBROWSER_ColumnInfo,  make_columns("Name", "Address", "Signal", "Kind"),
        LISTBROWSER_ColumnTitles, TRUE,
        LISTBROWSER_Labels,      &nearby_labels,
        LISTBROWSER_ShowSelected, TRUE,
        LISTBROWSER_AutoFit,     TRUE,
    End;

    gad_known = ListBrowserObject,
        GA_ID,                   GID_KNOWN,
        GA_RelVerify,            TRUE,
        LISTBROWSER_ColumnInfo,  make_columns("Name", "Address", "Kind", "State"),
        LISTBROWSER_ColumnTitles, TRUE,
        LISTBROWSER_Labels,      &known_labels,
        LISTBROWSER_ShowSelected, TRUE,
        LISTBROWSER_AutoFit,     TRUE,
    End;

    gad_scan = ButtonObject, GA_ID, GID_SCAN, GA_RelVerify, TRUE,
                             GA_Text, "_Scan", End;

    win_obj = WindowObject,
        WA_Title,          "Bluetooth Devices",
        WA_Width,          660,
        WA_Height,         360,
        WA_CloseGadget,    TRUE,
        WA_DepthGadget,    TRUE,
        WA_DragBar,        TRUE,
        WA_SizeGadget,     TRUE,
        WA_Activate,       TRUE,
        WINDOW_Position,   WPOS_CENTERSCREEN,
        WINDOW_IconifyGadget, TRUE,
        WINDOW_Layout, VLayoutObject,
            LAYOUT_SpaceOuter, TRUE,
            LAYOUT_DeferLayout, TRUE,

            LAYOUT_AddChild, VLayoutObject,
                LAYOUT_Label,    "Nearby",
                LAYOUT_BevelStyle, BVS_GROUP,
                LAYOUT_AddChild, gad_nearby,
                LAYOUT_AddChild, HLayoutObject,
                    LAYOUT_AddChild, gad_scan,
                    LAYOUT_AddChild, ButtonObject, GA_ID, GID_PAIR, GA_RelVerify, TRUE,
                                                   GA_Text, "_Pair", End,
                    LAYOUT_AddChild, HLayoutObject, End,   /* pushes the buttons left */
                End,
                CHILD_WeightedHeight, 0,
            End,

            LAYOUT_AddChild, VLayoutObject,
                LAYOUT_Label,    "Known devices",
                LAYOUT_BevelStyle, BVS_GROUP,
                LAYOUT_AddChild, gad_known,
                LAYOUT_AddChild, HLayoutObject,
                    LAYOUT_AddChild, ButtonObject, GA_ID, GID_CONNECT, GA_RelVerify, TRUE,
                                                   GA_Text, "_Connect", End,
                    LAYOUT_AddChild, ButtonObject, GA_ID, GID_DISCONNECT, GA_RelVerify, TRUE,
                                                   GA_Text, "_Disconnect", End,
                    LAYOUT_AddChild, ButtonObject, GA_ID, GID_FORGET, GA_RelVerify, TRUE,
                                                   GA_Text, "_Forget", End,
                    LAYOUT_AddChild, HLayoutObject, End,
                End,
                CHILD_WeightedHeight, 0,
            End,
        End,
    End;

    if (win_obj == NULL){
        printf("Cannot create the window.\n");
        return RETURN_FAIL;
    }

    window = (struct Window *) IDoMethod(win_obj, WM_OPEN, NULL);
    if (window == NULL){
        printf("Cannot open the window.\n");
        DisposeObject(win_obj);
        return RETURN_FAIL;
    }

    /* live updates rather than polling: the service tells us what changed */
    subscribed = bt_command(BTCMD_SUBSCRIBE_EVENTS, NULL, 0, NULL, 0, NULL) == BT_RESULT_OK;
    known_refresh();
    known_show();

    uint32 window_sig = 0;
    GetAttr(WINDOW_SigMask, win_obj, &window_sig);
    uint32 event_sig = 1UL << event_port->mp_SigBit;

    bool done = false;
    while (!done){

        uint32 signals = Wait(window_sig | event_sig | SIGBREAKF_CTRL_C);

        if (signals & SIGBREAKF_CTRL_C) done = true;

        if (signals & event_sig){
            BTServiceEvent * event;
            bool refresh_known  = false;
            bool refresh_nearby = false;

            while ((event = (BTServiceEvent *) GetMsg(event_port)) != NULL){
                switch (event->bse_Event){
                    case BTEVENT_DEVICE_FOUND:
                    case BTEVENT_DEVICE_UPDATED:
                        nearby_update(&event->bse_Device);
                        refresh_nearby = true;
                        refresh_known  = true;
                        break;
                    case BTEVENT_DEVICE_REMOVED:
                        nearby_remove(&event->bse_Device);
                        refresh_nearby = true;
                        refresh_known  = true;
                        break;
                    case BTEVENT_PAIRING_REQUEST:
                        /* accepted by the service for now; showing the number
                         * lets the user check it against the device */
                        printf("Pairing with %s, passkey %06lu\n",
                               event->bse_Device.name, (unsigned long) event->bse_Passkey);
                        break;
                    default:
                        refresh_known = true;
                        break;
                }
                ReplyMsg(&event->bse_Message);
            }

            if (refresh_nearby) nearby_show();
            if (refresh_known){
                known_refresh();
                known_show();
            }
        }

        if (signals & window_sig){
            uint32 result;
            uint16 code = 0;

            while ((result = IDoMethod(win_obj, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG){
                switch (result & WMHI_CLASSMASK){

                    case WMHI_CLOSEWINDOW:
                        done = true;
                        break;

                    case WMHI_ICONIFY:
                        IDoMethod(win_obj, WM_ICONIFY, NULL);
                        window = NULL;
                        break;

                    case WMHI_UNICONIFY:
                        window = (struct Window *) IDoMethod(win_obj, WM_OPEN, NULL);
                        break;

                    case WMHI_GADGETUP:
                        switch (result & WMHI_GADGETMASK){

                            case GID_SCAN:
                                /* asking to scan also makes the service look for
                                 * Classic devices again, which it stops doing on
                                 * its own once everything it knows is connected */
                                bt_command(BTCMD_SCAN_START, NULL, 0, NULL, 0, NULL);
                                break;

                            case GID_PAIR: {
                                int32 row = selected_row(gad_nearby);
                                if ((row >= 0) && ((uint32) row < nearby_count)){
                                    bt_command(BTCMD_PAIR, nearby[row].bd_addr,
                                               nearby[row].addr_type, NULL, 0, NULL);
                                }
                                break;
                            }

                            case GID_CONNECT: {
                                int32 row = selected_row(gad_known);
                                if ((row >= 0) && ((uint32) row < known_count)){
                                    bt_command(BTCMD_CONNECT, known[row].bd_addr,
                                               known[row].addr_type, NULL, 0, NULL);
                                }
                                break;
                            }

                            case GID_DISCONNECT: {
                                int32 row = selected_row(gad_known);
                                if ((row >= 0) && ((uint32) row < known_count)){
                                    bt_command(BTCMD_DISCONNECT, known[row].bd_addr,
                                               known[row].addr_type, NULL, 0, NULL);
                                }
                                break;
                            }

                            case GID_FORGET: {
                                int32 row = selected_row(gad_known);
                                if ((row >= 0) && ((uint32) row < known_count)){
                                    bt_command(BTCMD_UNPAIR, known[row].bd_addr,
                                               known[row].addr_type, NULL, 0, NULL);
                                    known_refresh();
                                    known_show();
                                }
                                break;
                            }

                            default:
                                break;
                        }
                        break;

                    default:
                        break;
                }
            }
        }
    }

    if (subscribed){
        bt_command(BTCMD_UNSUBSCRIBE_EVENTS, NULL, 0, NULL, 0, NULL);
    }

    /*
     * Reply anything that arrived between unsubscribing and now. The service
     * drops a subscriber that stops replying, but a message still owned by us
     * when this exits is one it can never free.
     */
    {
        struct Message * message;
        while ((message = GetMsg(event_port)) != NULL){
            ReplyMsg(message);
        }
    }

    DisposeObject(win_obj);
    FreeListBrowserList(&nearby_labels);
    FreeListBrowserList(&known_labels);

    FreeSysObject(ASOT_PORT, event_port);
    FreeSysObject(ASOT_PORT, reply_port);

    close_class(ButtonBase,      IButton);
    close_class(ListBrowserBase, IListBrowser);
    close_class(LayoutBase,      ILayout);
    close_class(WindowBase,      IWindow);
    close_class(IntuitionBase,   IIntuition);

    return RETURN_OK;
}
