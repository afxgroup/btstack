/*
 * main/runinterface.c - USBFDRunInterface
 *
 * Called by the USB stack when an interface we registered for is attached.
 * Only the Bluetooth controller interface is of interest: Wireless class,
 * Radio Frequency subclass, Bluetooth protocol.
 */

#include <exec/exec.h>
#include <utility/tagitem.h>
#include <usb/usb.h>
#include <usb/system.h>
#include <usb/devclasses.h>

#include <proto/exec.h>
#include <proto/usbfd.h>

#include "fdmain.h"

extern struct ExecIFace *IExec;

#define USBSUBCLASS_RF     0x01
#define USBPROTO_BLUETOOTH 0x01

LONG _USBFD_USBFDRunInterface(struct USBFDIFace *Self __attribute__((unused)),
                              struct USBFDStartupMsg *startmsg)
{
    struct USBBusIntDsc *descriptor;

    if (!startmsg->Object)
        return USBERR_UNSUPPORTED;

    descriptor = (struct USBBusIntDsc *)startmsg->Descriptor;

    if (descriptor->id_Class != USBCLASS_WIRELESS)
        return USBERR_UNSUPPORTED;

    if ((descriptor->id_Subclass != USBSUBCLASS_RF) ||
        (descriptor->id_Protocol != USBPROTO_BLUETOOTH))
        return USBERR_UNSUPPORTED;

    /*
     * A Bluetooth controller exposes *two* interfaces with this very class:
     * interface 0 carries HCI events and ACL data, interface 1 the
     * isochronous endpoints for SCO audio. Without this check the driver
     * fires twice per dongle, and starts the service twice - the second one
     * finding the port already taken.
     */
    if (descriptor->id_InterfaceID != 0)
        return USBERR_UNSUPPORTED;

    return fdmain(startmsg);
}
