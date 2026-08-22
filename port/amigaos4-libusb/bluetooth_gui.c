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
#include <gadgets/string.h>
#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/locale.h>
#include <devices/timer.h>
#include <dos/dostags.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/listbrowser.h>
#include <proto/button.h>
#include <proto/string.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bluetooth_service.h"
#include "bluetooth_gui_cat.h"

/* ------------------------------------------------------------------------- */
/* libraries                                                                 */

/*
 * These are declared by the proto headers and defined by nobody: unlike exec,
 * none of them is opened for us, and the ReAction macros reach straight through
 * the interface pointers - ListBrowserObject is IIntuition->NewObject() with a
 * class from IListBrowser. So they are ours to fill in and ours to give back.
 */
struct Library *IntuitionBase, *WindowBase, *LayoutBase, *ListBrowserBase, *ButtonBase, *StringBase;
struct Library *LocaleBase;
struct LocaleIFace *ILocale;
struct IntuitionIFace   *IIntuition;
struct WindowIFace      *IWindow;
struct LayoutIFace      *ILayout;
struct ListBrowserIFace *IListBrowser;
struct ButtonIFace      *IButton;
struct StringIFace      *IString;

static struct Catalog * catalog;

/*
 * The text for a string ID, translated when there is a translation.
 *
 * The built-in English is what GetCatalogStr() falls back to, so every string
 * has an answer whether or not a catalog is installed, whether or not it covers
 * this ID, and whether or not locale.library opened at all. Nothing here has to
 * check.
 */
static CONST_STRPTR GetString(LONG id){
    CONST_STRPTR text = (CONST_STRPTR) "";
    uint32 i;

    for (i = 0; i < CATCOMP_ARRAY_SIZE; i++){
        if (CatCompArray[i].cca_ID == id){
            text = (CONST_STRPTR) CatCompArray[i].cca_Str;
            break;
        }
    }
    if (ILocale == NULL) return text;
    return (CONST_STRPTR) GetCatalogStr(catalog, id, (CONST_STRPTR) text);
}

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

static struct Window * window;

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
/*
 * Say so in the title bar when something goes wrong.
 *
 * Every command result used to be discarded, so a service that refused
 * everything looked exactly like one with nothing to report: the window came up
 * saying the service was running and then stayed empty, with no way to tell
 * which. A version mismatch is the likely reason and worth naming, since the
 * fix is to update the other half.
 */
static void status_show(LONG message_id){
    if (window == NULL) return;
    SetWindowTitles(window, (CONST_STRPTR) GetString(message_id), (CONST_STRPTR) -1);
}

/*
 * Send a prepared message and wait for the answer.
 *
 * The port is looked up every time rather than kept, so the service starting
 * later, or being restarted, needs no action here at all. Everything up to the
 * PutMsg happens inside Forbid(): between finding a port and using it the task
 * owning it could be gone, and a message sent to a freed port is not a mistake
 * that can be noticed afterwards.
 */
static bt_result_t bt_send(BTServiceMsg * msg){

    msg->bsm_Message.mn_Node.ln_Type = NT_MESSAGE;
    msg->bsm_Message.mn_Length       = sizeof(*msg);
    msg->bsm_Message.mn_ReplyPort    = reply_port;
    msg->bsm_Version                 = BLUETOOTH_SERVICE_VERSION;
    msg->bsm_EventPort               = event_port;

    Forbid();
    struct MsgPort * service = FindPort(BLUETOOTH_SERVICE_PORT_NAME);
    if (service != NULL){
        PutMsg(service, &msg->bsm_Message);
    }
    Permit();

    if (service == NULL) return BT_RESULT_NO_CONTROLLER;

    WaitPort(reply_port);
    while (GetMsg(reply_port) == NULL) {
        WaitPort(reply_port);
    }

    if (msg->bsm_Result == BT_RESULT_UNSUPPORTED){
        status_show(MSG_VERSION_MISMATCH);
    } else if (msg->bsm_Result != BT_RESULT_OK){
        status_show(MSG_COMMAND_FAILED);
    } else {
        status_show(MSG_WINDOW_TITLE);
    }

    return msg->bsm_Result;
}

static bt_result_t bt_command(bt_command_t command, const uint8 * addr, uint8 addr_type,
                              BTDeviceInfo * devices, uint32 devices_max, uint32 * devices_count){

    BTServiceMsg msg;

    memset(&msg, 0, sizeof(msg));
    msg.bsm_Command    = command;
    msg.bsm_Devices    = devices;
    msg.bsm_DevicesMax = devices_max;
    if (addr != NULL){
        memcpy(msg.bsm_Addr, addr, 6);
        msg.bsm_AddrType = addr_type;
    }

    bt_result_t result = bt_send(&msg);
    if (devices_count != NULL) *devices_count = msg.bsm_DevicesCount;
    return result;
}

/* the name travels in both directions, so it gets its own way in */
static bt_result_t bt_command_name(bt_command_t command, char * name, uint32 name_size){

    BTServiceMsg msg;

    memset(&msg, 0, sizeof(msg));
    msg.bsm_Command = command;
    if (command == BTCMD_SET_NAME){
        /* the buffer is zeroed above, so a short copy is already terminated */
        memcpy(msg.bsm_Name, name, strnlen(name, sizeof(msg.bsm_Name) - 1));
    }

    bt_result_t result = bt_send(&msg);

    if ((result == BT_RESULT_OK) && (command == BTCMD_GET_NAME)){
        msg.bsm_Name[sizeof(msg.bsm_Name) - 1] = 0;
        uint32 len = strnlen(msg.bsm_Name, name_size - 1);
        memcpy(name, msg.bsm_Name, len);
        name[len] = 0;
    }
    return result;
}

/*
 * Whether the service is running, checked on a timer.
 *
 * It is not ours to own: bt.usbfd starts it when a controller is plugged in, it
 * can be stopped from a Shell, and it can exit on its own. Asking every couple
 * of seconds keeps the window honest without being a poll loop in any
 * meaningful sense - and there is nothing to be notified by, since the thing
 * that would send the notification is the thing that has gone.
 */
#define SERVICE_CHECK_SECONDS 2

static struct MsgPort     * timer_port;
static struct TimeRequest * timer_req;
static bool                 timer_pending;
static bool                 service_running;

static void timer_arm(uint32 seconds){
    if (timer_req == NULL) return;
    timer_req->Request.io_Command = TR_ADDREQUEST;
    timer_req->Time.Seconds       = seconds;
    timer_req->Time.Microseconds  = 0;
    SendIO((struct IORequest *) timer_req);
    timer_pending = true;
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
        case BT_DEVICE_KIND_MOUSE:    return (const char *) GetString(MSG_KIND_MOUSE);
        case BT_DEVICE_KIND_KEYBOARD: return (const char *) GetString(MSG_KIND_KEYBOARD);
        case BT_DEVICE_KIND_GAMEPAD:  return (const char *) GetString(MSG_KIND_GAMEPAD);
        case BT_DEVICE_KIND_AUDIO:    return (const char *) GetString(MSG_KIND_AUDIO);
        case BT_DEVICE_KIND_SERIAL:   return (const char *) GetString(MSG_KIND_SERIAL);
        default:                      return "";
    }
}

static const char * state_name(bt_device_state_t state){
    switch (state){
        case BT_DEVICE_STATE_FOUND:      return (const char *) GetString(MSG_STATE_FOUND);
        case BT_DEVICE_STATE_BONDED:     return (const char *) GetString(MSG_STATE_PAIRED);
        case BT_DEVICE_STATE_CONNECTING: return (const char *) GetString(MSG_STATE_CONNECTING);
        case BT_DEVICE_STATE_CONNECTED:  return (const char *) GetString(MSG_STATE_CONNECTED);
        case BT_DEVICE_STATE_IN_USE:     return (const char *) GetString(MSG_STATE_IN_USE);
        default:                         return "";
    }
}

/* most LE devices advertise no name at all, and an empty row looks broken */
static const char * name_or_unknown(const char * name){
    return (name[0] != 0) ? name : (const char *) GetString(MSG_NO_NAME);
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
    GID_SERVICE,
    GID_NAME,
    GID_SET_NAME,
};

static struct List nearby_labels;
static struct List known_labels;

static Object * win_obj;
static Object * gad_nearby;
static Object * gad_known;
static Object * gad_scan;
static Object * gad_pair;
static Object * gad_connect;
static Object * gad_disconnect;
static Object * gad_forget;
static Object * gad_service;
static Object * gad_name;
static bool     scan_running;
static bool     scan_shown;   /* what the button currently says */

/* the listbrowser must not be looking at a list while it is being rebuilt */
static int32 selected_row(Object * gadget);

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

        /*
         * One flat taglist, ended once.
         *
         * There is no TAG_END between the columns: putting one there ends the
         * list at the first column, and every column after it silently keeps
         * whatever it had - which is why this list came up with rows in it and
         * nothing written in them, while the other showed names and no
         * addresses.
         */
        struct Node * node = AllocListBrowserNode(4,
            LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, name_or_unknown(nearby[i].name),
            LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, address,
            LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, rssi,
            LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, kind_name(nearby[i].kind),
            TAG_DONE);
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
            LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, name_or_unknown(known[i].name),
            LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, address,
            LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, kind_name(known[i].kind),
            LBNA_Column, 3, LBNCA_CopyText, TRUE, LBNCA_Text, state_name(known[i].state),
            TAG_DONE);
        if (node != NULL) AddTail(&known_labels, node);
    }

    list_attach(gad_known, &known_labels);
}

/*
 * Follow the service appearing or disappearing.
 *
 * Everything shown belongs to the service, so when it goes the lists go with
 * it - leaving a device listed as connected by a service that is not running
 * would be worse than showing nothing. When it comes back we subscribe again,
 * since the subscription died with it, and ask what it knows.
 */
/* the name the service is using, so the field shows what is true now */
static void name_refresh(void){
    char name[32];
    if (bt_command_name(BTCMD_GET_NAME, name, sizeof(name)) != BT_RESULT_OK) return;
    if ((window == NULL) || (gad_name == NULL)) return;
    SetGadgetAttrs((struct Gadget *) gad_name, window, NULL,
                   STRINGA_TextVal, name, TAG_DONE);
}

static void service_state_changed(bool running){

    service_running = running;

    if (running){
        subscribed = bt_command(BTCMD_SUBSCRIBE_EVENTS, NULL, 0, NULL, 0, NULL) == BT_RESULT_OK;
        name_refresh();
        known_refresh();
    } else {
        subscribed   = false;
        nearby_count = 0;
        known_count  = 0;
    }

    nearby_show();
    known_show();

    if (window != NULL){
        SetGadgetAttrs((struct Gadget *) gad_service, window, NULL,
                       GA_Text,     running ? GetString(MSG_BUTTON_STOP_SERVICE)
                                            : GetString(MSG_BUTTON_START_SERVICE),
                       GA_HintInfo, running ? GetString(MSG_HINT_STOP_SERVICE)
                                            : GetString(MSG_HINT_START_SERVICE),
                       TAG_DONE);
    }
}

/*
 * Start the service as its own process.
 *
 * Detached, with its handles on NIL:, so it outlives this window - it is a
 * service, and closing the thing that manages it is no reason for it to stop.
 */
static void service_start(void){
    BPTR nil_in  = Open("NIL:", MODE_OLDFILE);
    BPTR nil_out = Open("NIL:", MODE_NEWFILE);

    SystemTags("C:BluetoothService",
               SYS_Input,  nil_in,
               SYS_Output, nil_out,
               SYS_Error,  ZERO,
               SYS_Asynch, TRUE,
               NP_Name,    "BluetoothService",
               TAG_DONE);
    /* with SYS_Asynch the handles belong to the new process */
}

/*
 * A button that acts on a selection is off while there is none.
 *
 * Every one of these needs a device, so with nothing selected there is nothing
 * for them to do and pressing them could only be a mistake. Scan is the
 * exception and stays live: it acts on nothing in particular.
 *
 * Called after anything that can change either the selection or the lists,
 * since rebuilding a list drops the selection with it.
 */
static void buttons_update(void){
    if (window == NULL) return;

    /* with no service there is nothing any of these could act on */
    BOOL nearby_off = (!service_running || (selected_row(gad_nearby) < 0)) ? TRUE : FALSE;
    BOOL known_off  = (!service_running || (selected_row(gad_known)  < 0)) ? TRUE : FALSE;

    /*
     * Scan is a switch, and is only written to when it changes.
     *
     * Setting GA_Text redraws the button, and this runs on every batch of
     * events - which during a scan means constantly - so writing it
     * unconditionally made it flicker. Nothing here is written unless the state
     * it shows has actually moved.
     */
    if (scan_running != scan_shown){
        scan_shown = scan_running;
        SetGadgetAttrs((struct Gadget *) gad_scan, window, NULL,
                       GA_Text, scan_running ? GetString(MSG_BUTTON_STOP_SCAN)
                                             : GetString(MSG_BUTTON_SCAN),
                       TAG_DONE);
    }
    SetGadgetAttrs((struct Gadget *) gad_scan, window, NULL,
                   GA_Disabled, service_running ? FALSE : TRUE, TAG_DONE);
    SetGadgetAttrs((struct Gadget *) gad_pair,       window, NULL, GA_Disabled, nearby_off, TAG_DONE);
    SetGadgetAttrs((struct Gadget *) gad_connect,    window, NULL, GA_Disabled, known_off,  TAG_DONE);
    SetGadgetAttrs((struct Gadget *) gad_disconnect, window, NULL, GA_Disabled, known_off,  TAG_DONE);
    SetGadgetAttrs((struct Gadget *) gad_forget,     window, NULL, GA_Disabled, known_off,  TAG_DONE);
}

/* which row is selected, or -1 */
static int32 selected_row(Object * gadget){
    uint32 row = 0;
    GetAttr(LISTBROWSER_Selected, gadget, &row);
    return (row == (uint32) ~0) ? -1 : (int32) row;
}

/* ------------------------------------------------------------------------- */

/*
 * Columns as a plain array, which is what listbrowser.gadget wants.
 *
 * The first attempt built these with AllocLBColumnInfo() and weights, and only
 * the first column ever appeared: everything else ended up in one nameless
 * space with the names cut off at five characters. A static array terminated by
 * { -1, (STRPTR)~0, -1 } is the form that works, and pixel widths are what it
 * takes - the titles are filled in at startup because they are translated and a
 * static initialiser cannot call anything.
 */
static struct ColumnInfo nearby_columns[] = {
    { 160, NULL, CIF_DRAGGABLE },
    { 150, NULL, CIF_DRAGGABLE },
    {  80, NULL, CIF_DRAGGABLE },
    {  90, NULL, CIF_DRAGGABLE },
    {  -1, (STRPTR) ~0, -1 }
};

static struct ColumnInfo known_columns[] = {
    { 160, NULL, CIF_DRAGGABLE },
    { 150, NULL, CIF_DRAGGABLE },
    {  90, NULL, CIF_DRAGGABLE },
    { 100, NULL, CIF_DRAGGABLE },
    {  -1, (STRPTR) ~0, -1 }
};

static void columns_translate(void){
    nearby_columns[0].ci_Title = (STRPTR) GetString(MSG_CI_NAME);
    nearby_columns[1].ci_Title = (STRPTR) GetString(MSG_CI_ADDRESS);
    nearby_columns[2].ci_Title = (STRPTR) GetString(MSG_CI_SIGNAL);
    nearby_columns[3].ci_Title = (STRPTR) GetString(MSG_CI_KIND);

    known_columns[0].ci_Title  = (STRPTR) GetString(MSG_CI_NAME);
    known_columns[1].ci_Title  = (STRPTR) GetString(MSG_CI_ADDRESS);
    known_columns[2].ci_Title  = (STRPTR) GetString(MSG_CI_KIND);
    known_columns[3].ci_Title  = (STRPTR) GetString(MSG_CI_STATE);
}

int main(void){

    /* a missing catalog is not a failure: GetString() falls back to English */
    LocaleBase = open_class("locale.library", 52, (APTR *) &ILocale);
    if (ILocale != NULL){
        catalog = OpenCatalogA(NULL, "BluetoothGUI.catalog", NULL);
    }
    columns_translate();

    IntuitionBase   = open_class("intuition.library", 51,     (APTR *) &IIntuition);
    WindowBase      = open_class("window.class", 53,          (APTR *) &IWindow);
    LayoutBase      = open_class("gadgets/layout.gadget", 53,     (APTR *) &ILayout);
    ListBrowserBase = open_class("gadgets/listbrowser.gadget", 53,(APTR *) &IListBrowser);
    ButtonBase      = open_class("gadgets/button.gadget", 53,     (APTR *) &IButton);
    StringBase      = open_class("gadgets/string.gadget", 53,     (APTR *) &IString);

    if ((IntuitionBase == NULL) || (WindowBase == NULL) || (LayoutBase == NULL) ||
        (ListBrowserBase == NULL) || (ButtonBase == NULL) || (StringBase == NULL)){
        printf("%s\n", GetString(MSG_NO_CLASSES));
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

    timer_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (timer_port != NULL){
        timer_req = AllocSysObjectTags(ASOT_IOREQUEST,
                                       ASOIOR_Size,      sizeof(struct TimeRequest),
                                       ASOIOR_ReplyPort, timer_port,
                                       TAG_END);
    }
    if ((timer_req == NULL) ||
        (OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *) timer_req, 0) != 0)){
        /* without it the state is only refreshed when something else happens,
         * which is worth carrying on for rather than refusing to start */
        if (timer_req != NULL){
            FreeSysObject(ASOT_IOREQUEST, timer_req);
            timer_req = NULL;
        }
    }


    gad_nearby = ListBrowserObject,
        GA_ID,                      GID_NEARBY,
        GA_RelVerify,               TRUE,
        LISTBROWSER_ColumnInfo,     &nearby_columns,
        LISTBROWSER_ColumnTitles,   TRUE,
        LISTBROWSER_Labels,         &nearby_labels,
        LISTBROWSER_ShowSelected,   TRUE,
        LISTBROWSER_HorizontalProp, TRUE,
        LISTBROWSER_Separators,     TRUE,
    End;

    gad_known = ListBrowserObject,
        GA_ID,                      GID_KNOWN,
        GA_RelVerify,               TRUE,
        LISTBROWSER_ColumnInfo,     &known_columns,
        LISTBROWSER_ColumnTitles,   TRUE,
        LISTBROWSER_Labels,         &known_labels,
        LISTBROWSER_ShowSelected,   TRUE,
        LISTBROWSER_HorizontalProp, TRUE,
        LISTBROWSER_Separators,     TRUE,
    End;

    gad_scan = ButtonObject, GA_ID, GID_SCAN, GA_RelVerify, TRUE,
                             GA_Text,     GetString(MSG_BUTTON_SCAN),
                             GA_HintInfo, GetString(MSG_HINT_SCAN), End;

    win_obj = WindowObject,
        WA_Title,          GetString(MSG_WINDOW_TITLE),
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
            LAYOUT_SpaceOuter,  TRUE,
            LAYOUT_DeferLayout, TRUE,

            /*
             * The list takes the room and the buttons take none.
             *
             * Weighting the whole group zero instead squashed both lists to a
             * few pixels while the window kept its size, which is what the
             * first version did.
             */
            LAYOUT_AddChild, HLayoutObject,
                LAYOUT_Label,    GetString(MSG_LOCAL_NAME),
                LAYOUT_AddChild, gad_name = StringObject,
                    GA_ID,          GID_NAME,
                    GA_RelVerify,   TRUE,
                    GA_HintInfo,    GetString(MSG_HINT_LOCAL_NAME),
                    STRINGA_MaxChars, 31,
                End,
                LAYOUT_AddChild, ButtonObject,
                    GA_ID,        GID_SET_NAME,
                    GA_RelVerify, TRUE,
                    GA_Text,      GetString(MSG_BUTTON_SET_NAME),
                    GA_HintInfo,  GetString(MSG_HINT_LOCAL_NAME),
                End,
                CHILD_WeightedWidth, 0,
            End,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, VLayoutObject,
                LAYOUT_BevelStyle, BVS_GROUP,
                LAYOUT_Label,      GetString(MSG_NEARBY),
                LAYOUT_SpaceInner, TRUE,

                LAYOUT_AddChild, gad_nearby,

                LAYOUT_AddChild, HLayoutObject,
                    LAYOUT_EvenSize, TRUE,
                    LAYOUT_AddChild, gad_service = ButtonObject,
                        GA_ID,        GID_SERVICE,
                        GA_RelVerify, TRUE,
                        GA_Text,      GetString(MSG_BUTTON_START_SERVICE),
                        GA_HintInfo,  GetString(MSG_HINT_START_SERVICE),
                    End,
                    LAYOUT_AddChild, gad_scan,
                    LAYOUT_AddChild, gad_pair = ButtonObject,
                        GA_ID,        GID_PAIR,
                        GA_RelVerify, TRUE,
                        GA_Text,      GetString(MSG_BUTTON_PAIR),
                        GA_HintInfo,  GetString(MSG_HINT_PAIR),
                    End,
                End,
                CHILD_WeightedHeight, 0,
            End,

            LAYOUT_AddChild, VLayoutObject,
                LAYOUT_BevelStyle, BVS_GROUP,
                LAYOUT_Label,      GetString(MSG_KNOWN),
                LAYOUT_SpaceInner, TRUE,

                LAYOUT_AddChild, gad_known,

                LAYOUT_AddChild, HLayoutObject,
                    LAYOUT_EvenSize, TRUE,
                    LAYOUT_AddChild, gad_connect = ButtonObject,
                        GA_ID,        GID_CONNECT,
                        GA_RelVerify, TRUE,
                        GA_Text,      GetString(MSG_BUTTON_CONNECT),
                        GA_HintInfo,  GetString(MSG_HINT_CONNECT),
                    End,
                    LAYOUT_AddChild, gad_disconnect = ButtonObject,
                        GA_ID,        GID_DISCONNECT,
                        GA_RelVerify, TRUE,
                        GA_Text,      GetString(MSG_BUTTON_DISCONNECT),
                        GA_HintInfo,  GetString(MSG_HINT_DISCONNECT),
                    End,
                    LAYOUT_AddChild, gad_forget = ButtonObject,
                        GA_ID,        GID_FORGET,
                        GA_RelVerify, TRUE,
                        GA_Text,      GetString(MSG_BUTTON_FORGET),
                        GA_HintInfo,  GetString(MSG_HINT_FORGET),
                    End,
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
        printf("%s\n", GetString(MSG_NO_WINDOW));
        DisposeObject(win_obj);
        return RETURN_FAIL;
    }

    /* live updates for what the service knows; the timer is only for whether it
     * is there at all, which it cannot very well tell us itself */
    printf("BluetoothGUI (protocol %u, built %s %s)\n",
           (unsigned) BLUETOOTH_SERVICE_VERSION, __DATE__, __TIME__);

    service_state_changed(service_present());
    buttons_update();
    timer_arm(SERVICE_CHECK_SECONDS);

    uint32 window_sig = 0;
    GetAttr(WINDOW_SigMask, win_obj, &window_sig);
    uint32 event_sig = 1UL << event_port->mp_SigBit;
    uint32 timer_sig = (timer_req != NULL) ? (1UL << timer_port->mp_SigBit) : 0;

    bool done = false;
    while (!done){

        uint32 signals = Wait(window_sig | event_sig | timer_sig | SIGBREAKF_CTRL_C);

        if (signals & SIGBREAKF_CTRL_C) done = true;

        if (signals & timer_sig){
            while (GetMsg(timer_port) != NULL) { /* drain */ }
            timer_pending = false;

            bool running = service_present();
            if (running != service_running){
                service_state_changed(running);
                buttons_update();
            }
            timer_arm(SERVICE_CHECK_SECONDS);
        }

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
                    case BTEVENT_SCAN_STARTED:
                        scan_running  = true;
                        refresh_known = true;
                        break;

                    case BTEVENT_SCAN_STOPPED:
                        scan_running  = false;
                        refresh_known = true;
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
                FreeSysObject(ASOT_MESSAGE, event);
            }

            if (refresh_nearby) nearby_show();
            if (refresh_known){
                known_refresh();
                known_show();
            }
            if (refresh_nearby || refresh_known) buttons_update();
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

                            case GID_NEARBY:
                            case GID_KNOWN:
                                buttons_update();
                                break;

                            case GID_SET_NAME:
                            case GID_NAME: {
                                /* Enter in the field means the same as the
                                 * button: the name as typed is what is wanted */
                                STRPTR typed = NULL;
                                GetAttr(STRINGA_TextVal, gad_name, (uint32 *) &typed);
                                if ((typed != NULL) && (typed[0] != 0)){
                                    bt_command_name(BTCMD_SET_NAME, (char *) typed, 32);
                                }
                                break;
                            }

                            case GID_SERVICE:
                                if (service_running){
                                    bt_command(BTCMD_SHUTDOWN, NULL, 0, NULL, 0, NULL);
                                } else {
                                    service_start();
                                }
                                /* look again shortly rather than assume it
                                 * worked: starting is a process being created
                                 * and stopping is one winding itself down, and
                                 * neither has finished by the time we get here */
                                timer_arm(1);
                                break;

                            case GID_SCAN:
                                /* asking to scan makes the service look for
                                 * Classic devices again, which it stops doing on
                                 * its own once everything it knows is connected */
                                bt_command(scan_running ? BTCMD_SCAN_STOP : BTCMD_SCAN_START,
                                           NULL, 0, NULL, 0, NULL);
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
                                buttons_update();
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
     * Anything that arrived between unsubscribing and now is ours to free -
     * events are one way and the service has already forgotten them.
     */
    {
        struct Message * message;
        while ((message = GetMsg(event_port)) != NULL){
            FreeSysObject(ASOT_MESSAGE, message);
        }
    }

    DisposeObject(win_obj);
    FreeListBrowserList(&nearby_labels);
    FreeListBrowserList(&known_labels);

    if (timer_req != NULL){
        if (timer_pending){
            AbortIO((struct IORequest *) timer_req);
            WaitIO((struct IORequest *) timer_req);
        }
        CloseDevice((struct IORequest *) timer_req);
        FreeSysObject(ASOT_IOREQUEST, timer_req);
    }
    if (timer_port != NULL) FreeSysObject(ASOT_PORT, timer_port);

    FreeSysObject(ASOT_PORT, event_port);
    FreeSysObject(ASOT_PORT, reply_port);

    close_class(StringBase,      IString);
    close_class(ButtonBase,      IButton);
    close_class(ListBrowserBase, IListBrowser);
    close_class(LayoutBase,      ILayout);
    close_class(WindowBase,      IWindow);
    close_class(IntuitionBase,   IIntuition);
    if (catalog != NULL) CloseCatalog(catalog);
    close_class(LocaleBase,      ILocale);

    return RETURN_OK;
}
