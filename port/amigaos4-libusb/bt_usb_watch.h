/**
 * @title Watching for the last Bluetooth controller leaving
 *
 * The USB stack tells whoever asks when a function is removed. This subscribes
 * to that and, each time one goes, asks whether any Bluetooth controller is
 * still plugged in - because two are perfectly possible and unplugging one is
 * not the same as having none.
 *
 * This lives here rather than in bt.usbfd, which is where it might be expected.
 * A function driver is only ever called to attach: there is no detach entry
 * point in the interface, and the one notification that reports it,
 * USBNM_TYPE_INTERFACEDETACH, goes solely to whoever claimed the interface -
 * which is this process, through libusb. The subscribable one,
 * USBNM_TYPE_FUNCTIONREMOVED, needs a MsgPort with somebody waiting on it, and
 * a driver is a library with no process of its own to wait with. The service
 * has a run loop already, and is the thing being stopped.
 */

#ifndef BT_USB_WATCH_H
#define BT_USB_WATCH_H

#include <stdbool.h>

#if defined __cplusplus
extern "C" {
#endif

/**
 * @brief Start watching. The callback runs when the last controller is gone.
 * @return false if the USB stack offers no notifications, in which case
 *         nothing is watched and the service simply keeps running.
 */
bool bt_usb_watch_open(void (*last_controller_gone)(void));

void bt_usb_watch_close(void);

#if defined __cplusplus
}
#endif

#endif // BT_USB_WATCH_H
