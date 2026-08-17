#ifndef BT_USBFD__FDMAIN_H
#define BT_USBFD__FDMAIN_H

#include <usb/system.h>

struct USBFDStartupMsg;

void fdmain_init(void);
int  fdmain_register(void);
void fdmain_unregister(void);
int  fdmain(struct USBFDStartupMsg *startmsg);

#endif
