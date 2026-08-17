/*
 * main/release.c — USBFDIFace Release
 */

#include <exec/exec.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <usb/system.h>
#include <proto/usbfd.h>

extern struct ExecIFace *IExec;

ULONG _USBFD_Release(struct USBFDIFace *Self)
{
    return --Self->Data.RefCount;
}
