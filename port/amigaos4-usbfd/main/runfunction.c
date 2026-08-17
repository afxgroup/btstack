/*
 * main/runfunction.c — USBFDRunFunction (not used, we're interface-level)
 */

#include <exec/exec.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <usb/system.h>
#include <proto/usbfd.h>

LONG _USBFD_USBFDRunFunction(struct USBFDIFace *Self __attribute__((unused)),
                              struct USBFDStartupMsg *startmsg __attribute__((unused)))
{
    return USBERR_UNSUPPORTED;
}
