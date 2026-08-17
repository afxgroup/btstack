/*
 * main/getattrsa.c — USBFDGetAttrsA (no attributes to report)
 */

#include <exec/exec.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <usb/system.h>
#include <proto/usbfd.h>
#include <stdarg.h>

extern struct UtilityIFace *IUtility;

void _USBFD_USBFDGetAttrsA(struct USBFDIFace *Self __attribute__((unused)),
                            struct TagItem *taglist __attribute__((unused)))
{
    /* No attributes */
}

void VARARGS68K _USBFD_USBFDGetAttrs(struct USBFDIFace *Self, ...)
{
    va_list ap;
    struct TagItem *tags;

    va_startlinear(ap, Self);
    tags = va_getlinearva(ap, struct TagItem *);

    Self->USBFDGetAttrsA(tags);
    va_end(ap);
}
