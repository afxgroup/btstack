/*
 * Desktop notifications for BluetoothService.
 *
 * The service has no window and normally nothing to say: the GUI need not be
 * running, and the serial log is not somewhere anyone watches. So the events
 * that a person would want to know about - a device paired, connected, gone
 * away, a file arrived - are announced through application.library, which puts
 * them where the user is already looking.
 *
 * Everything here is optional. If application.library is missing or refuses to
 * register the application, the calls become silent and the service runs
 * exactly as before.
 */

#ifndef BT_NOTIFY_H
#define BT_NOTIFY_H

#if defined __cplusplus
extern "C" {
#endif

/** @brief Open application.library and register the service. Failure is not fatal. */
void bt_notify_open(void);

/** @brief Unregister and close. Safe to call when the open failed. */
void bt_notify_close(void);

/**
 * The four events worth interrupting someone for.
 *
 * @param name  the device's name, or NULL when it has none yet - the text then
 *              falls back to saying "a device", which is still more use than an
 *              empty line where a name should be.
 */
void bt_notify_paired(const char * name);
void bt_notify_connected(const char * name);
void bt_notify_disconnected(const char * name);

/** @param filename  what was received, without its path. */
void bt_notify_file_received(const char * filename);

#if defined __cplusplus
}
#endif

#endif /* BT_NOTIFY_H */
