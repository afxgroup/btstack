/*
 * fdmain.c - bt.usbfd core
 *
 * Registers for USBCLASS_WIRELESS (0xE0) and reacts to a Bluetooth controller
 * being attached or removed.
 *
 * The driver deliberately does almost nothing: it makes sure the Bluetooth
 * service is running and tells it that a controller appeared. Everything else -
 * the stack, the devices, the profile handlers - lives in that service.
 *
 * Why not run the stack here: a function driver runs inside the USB stack's
 * context and its lifetime is tied to the device. A Bluetooth stack needs to own
 * a run loop, keep bonding keys, hold connections across dongle re-plugs and
 * talk to input.device; none of that belongs in a hotplug hook.
 *
 * The driver must NOT claim the interfaces of the controller: the service
 * reaches the dongle through libusb-1.library, which needs to claim interface 0
 * itself. We only ask for the attach/detach notification.
 */

#include <string.h>

#include <usb/usb.h>
#include <usb/devclasses.h>
#include <usb/system.h>

#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/usbsys.h>
#include <proto/usbresource.h>

#include "fdmain.h"
#include "bluetooth_service.h"

extern struct ExecIFace        *IExec;
extern struct UtilityIFace     *IUtility;
extern struct USBResourceIFace *IUSBResource;

/* Bluetooth controllers are Wireless / Radio Frequency / Bluetooth */
#define USBSUBCLASS_RF          0x01
#define USBPROTO_BLUETOOTH      0x01

/* started when a controller shows up and no service is running yet */
#define BLUETOOTH_SERVICE_COMMAND "C:BluetoothService"

static APTR MyFDKey;

/* -------------------------------------------------------------------------- */

void fdmain_init(void)
{
    /* Nothing to initialise */
}

int fdmain_register(void)
{
    int32 err;

    fdmain_init();

    /*
     * Interface driver, low priority: we only want to be told about the device,
     * never to own it. A driver that owns the interfaces would lock out
     * libusb-1.library, and with it the service.
     */
    MyFDKey = IUSBResource->USBResRegisterFD(
        USBA_FD_Name,            "bt.usbfd",
        USBA_FD_Title,           "Bluetooth hotplug helper",
        USBA_Priority,           -10,
        USBA_FD_InterfaceDriver, TRUE,
        USBA_Class,              USBCLASS_WIRELESS,
        USBA_Subclass,           USBSUBCLASS_RF,
        USBA_ErrorCode,          &err,
        TAG_END);
    /* there is no USBA_Protocol tag, so the Bluetooth protocol is checked in
     * _USBFD_USBFDRunInterface() instead */

    if ((MyFDKey != NULL) || ((MyFDKey == NULL) && (err == USBERR_ISPRESENT)))
    {
        IExec->DebugPrintF("[bt.usbfd] registered with the USB stack\n");
        return TRUE;
    }

    IExec->DebugPrintF("[bt.usbfd] registration failed, error %ld\n", (long)err);
    return FALSE;
}

void fdmain_unregister(void)
{
    if (MyFDKey)
    {
        IUSBResource->USBResUnregisterFD(MyFDKey);
        MyFDKey = NULL;
    }
}

/* -------------------------------------------------------------------------- */

/*
 * Send one command to the service and wait for the reply.
 * Returns FALSE when the service is not running.
 */
static BOOL bt_service_command(bt_command_t command)
{
    struct MsgPort *service_port;
    struct MsgPort *reply_port;
    BTServiceMsg    msg;
    BOOL            ok = FALSE;

    reply_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (reply_port == NULL)
        return FALSE;

    memset(&msg, 0, sizeof(msg));
    msg.bsm_Message.mn_ReplyPort = reply_port;
    msg.bsm_Message.mn_Length    = sizeof(msg);
    msg.bsm_Version              = BLUETOOTH_SERVICE_VERSION;
    msg.bsm_Command              = command;

    /*
     * Forbid() only while looking the port up and putting the message on it:
     * the service could exit between FindPort() and PutMsg() otherwise, and we
     * would write into a freed port.
     */
    IExec->Forbid();
    service_port = IExec->FindPort(BLUETOOTH_SERVICE_PORT_NAME);
    if (service_port != NULL)
    {
        IExec->PutMsg(service_port, &msg.bsm_Message);
        ok = TRUE;
    }
    IExec->Permit();

    if (ok)
    {
        /* the service replies from its run loop, so this is bounded */
        IExec->WaitPort(reply_port);
        IExec->GetMsg(reply_port);
        ok = (msg.bsm_Result == BT_RESULT_OK);
    }

    IExec->FreeSysObject(ASOT_PORT, reply_port);
    return ok;
}

/*
 * Trace to the serial debug output.
 *
 * The console does not exist yet at the point this driver runs, and neither
 * does anything else that could record what happened: at boot the only way to
 * see how far we got is the serial log, next to the [DOS] and [_impl_
 * InitResident] lines the kernel prints there.
 */
#define BTFD_LOG(...) IExec->DebugPrintF("[bt.usbfd] " __VA_ARGS__)

/*
 * Has the USB stack finished booting?
 *
 * Before that it is "prebooted": running on preloaded drivers, deliberately as
 * if dos.library were not around - because it may not be. A dongle already
 * plugged in at power on gets us called right there, and touching DOS at that
 * point does not fail cleanly, it hangs the boot.
 *
 * The already-open usbsys.device from the startup message is used rather than
 * opening it here: we are called from inside the USB stack, which may well hold
 * a lock on itself while doing so, and opening its device from that context is
 * asking for a deadlock. The message documents USBReq as being there for
 * exactly this, and says not to close it.
 */
static BOOL usb_stack_fullbooted(struct USBFDStartupMsg *startmsg)
{
    struct USBSysIFace *IUSBSys;
    uint32              fullbooted = FALSE;

    if ((startmsg == NULL) || (startmsg->USBReq == NULL))
    {
        BTFD_LOG("no USBReq in the startup message\n");
        return FALSE;
    }

    IUSBSys = (struct USBSysIFace *)IExec->GetInterface(
        (struct Library *)startmsg->USBReq->io_Device, "main", 1, NULL);
    if (IUSBSys == NULL)
    {
        BTFD_LOG("cannot get the usbsys interface\n");
        return FALSE;
    }

    IUSBSys->USBGetStackAttrs(USBA_Stack_Fullbooted, &fullbooted, TAG_END);
    IExec->DropInterface((struct Interface *)IUSBSys);

    return fullbooted ? TRUE : FALSE;
}

/*
 * Is there a filesystem to load the service from yet?
 *
 * The USB stack fullbooting means dos.library is available, which is not the
 * same as the boot volume being mounted and C: pointing at it. SystemTags()
 * would then block waiting for something that is not there, and it would block
 * us inside the USB stack. Requesters are turned off for the check: asking the
 * user to insert a volume, on a machine still booting, with no Workbench to
 * show the requester on, is the last thing this should do.
 */
static BOOL bt_service_available(void)
{
    struct Library  *DOSBase;
    struct DOSIFace *IDOS;
    BOOL             available = FALSE;

    DOSBase = IExec->OpenLibrary("dos.library", 50);
    if (DOSBase == NULL)
        return FALSE;

    IDOS = (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL);
    if (IDOS != NULL)
    {
        APTR old_window = IDOS->SetProcWindow((APTR)-1);
        BPTR lock = IDOS->Lock(BLUETOOTH_SERVICE_COMMAND, SHARED_LOCK);
        if (lock != ZERO)
        {
            IDOS->UnLock(lock);
            available = TRUE;
        }
        IDOS->SetProcWindow(old_window);
        IExec->DropInterface((struct Interface *)IDOS);
    }

    IExec->CloseLibrary(DOSBase);
    return available;
}

static void bt_service_start(void)
{
    struct Library  *DOSBase;
    struct DOSIFace *IDOS;

    IExec->Forbid();
    if (IExec->FindPort(BLUETOOTH_SERVICE_PORT_NAME) != NULL)
    {
        IExec->Permit();
        return;
    }
    IExec->Permit();

    /* the driver is built without library bases of its own, so open dos here */
    DOSBase = IExec->OpenLibrary("dos.library", 50);
    if (DOSBase == NULL)
        return;

    IDOS = (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL);
    if (IDOS != NULL)
    {
        BPTR nil_in  = IDOS->Open("NIL:", MODE_OLDFILE);
        BPTR nil_out = IDOS->Open("NIL:", MODE_NEWFILE);

        BTFD_LOG("launching %s\n", BLUETOOTH_SERVICE_COMMAND);

        IDOS->SystemTags(BLUETOOTH_SERVICE_COMMAND,
                         SYS_Input,   nil_in,
                         SYS_Output,  nil_out,
                         SYS_Error,   ZERO,
                         SYS_Asynch,  TRUE,
                         NP_Name,     "Bluetooth Service",
                         NP_Priority, 1,
                         TAG_END);
        /* with SYS_Asynch the file handles belong to the new process */

        BTFD_LOG("launched\n");
        IExec->DropInterface((struct Interface *)IDOS);
    }

    IExec->CloseLibrary(DOSBase);
}

/* -------------------------------------------------------------------------- */

/*
 * Called by the USB stack in its own task context when a Bluetooth controller
 * interface is attached, and again on removal.
 */
int fdmain(struct USBFDStartupMsg *startmsg)
{
    struct USBBusIntDsc *descriptor = (struct USBBusIntDsc *)startmsg->Descriptor;

    (void)descriptor;

    BTFD_LOG("attach: interface %u, class %02x/%02x/%02x\n",
             descriptor ? descriptor->id_InterfaceID : 0xff,
             descriptor ? descriptor->id_Class : 0,
             descriptor ? descriptor->id_Subclass : 0,
             descriptor ? descriptor->id_Protocol : 0);

    /*
     * Do nothing at all until the USB stack has fullbooted. With a dongle
     * plugged in at power on we are called during the prebooted phase, where
     * dos.library may not exist yet - and starting a process there hangs the
     * boot before Workbench ever appears. The stack calls us again for the same
     * device once it has fullbooted, which is when there is a system to run the
     * service on.
     */
    if (usb_stack_fullbooted(startmsg) == FALSE)
    {
        BTFD_LOG("stack not fullbooted yet, doing nothing\n");
        return USBERR_NOERROR;
    }
    BTFD_LOG("stack is fullbooted\n");

    if (bt_service_available() == FALSE)
    {
        BTFD_LOG("%s not reachable yet, leaving it to WBStartup\n", BLUETOOTH_SERVICE_COMMAND);
        return USBERR_NOERROR;
    }
    BTFD_LOG("%s is reachable\n", BLUETOOTH_SERVICE_COMMAND);

    /*
     * Attach: make sure the service is up, then tell it a controller is there.
     * The notification is best effort - a service that has just been started is
     * not listening yet, and finds the controller by scanning USB itself.
     */
    bt_service_start();
    BTFD_LOG("notifying the service\n");
    bt_service_command(BTCMD_CONTROLLER_ATTACHED);
    BTFD_LOG("done\n");

    /*
     * TODO: to also get the removal notification, the interface has to be
     * claimed with a notify port and waited on here, like usbaudio.usbfd does.
     * That has to be done without preventing libusb-1.library from claiming it
     * (it uses USBA_SeeClaimed), which needs to be verified on the real stack
     * before it goes in - claiming the wrong interface would lock the service
     * out of the dongle. Until then the service notices the controller is gone
     * from its own USB errors.
     */

    return USBERR_NOERROR;
}
