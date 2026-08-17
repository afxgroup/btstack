/* bt_vectors.c — generated interface vector table */

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_EXEC_H
#include <exec/exec.h>
#endif
#ifndef EXEC_INTERFACES_H
#include <exec/interfaces.h>
#endif

extern ULONG _USBFD_Obtain(struct USBFDIFace *);
extern ULONG _USBFD_Release(struct USBFDIFace *);
extern void  _USBFD_USBFDGetAttrsA(struct USBFDIFace *, struct TagItem *taglist);
extern void  _USBFD_USBFDGetAttrs(struct USBFDIFace *, ...);
extern LONG  _USBFD_USBFDRunFunction(struct USBFDIFace *, struct USBFDStartupMsg *startmsg);
extern LONG  _USBFD_USBFDRunInterface(struct USBFDIFace *, struct USBFDStartupMsg *startmsg);

STATIC CONST APTR main_vectors[] =
{
    _USBFD_Obtain,
    _USBFD_Release,
    NULL,
    NULL,
    _USBFD_USBFDGetAttrsA,
    _USBFD_USBFDGetAttrs,
    _USBFD_USBFDRunFunction,
    _USBFD_USBFDRunInterface,
    (APTR)-1
};
