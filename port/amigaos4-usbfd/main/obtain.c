/*
 * main/obtain.c — USBFDIFace Obtain
 */

#include <exec/exec.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <usb/system.h>
#include <proto/usbfd.h>

extern struct ExecIFace *IExec;

ULONG _USBFD_Obtain(struct USBFDIFace *Self)
{
    return ++Self->Data.RefCount;
}
