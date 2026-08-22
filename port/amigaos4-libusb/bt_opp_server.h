/**
 * @title OBEX Object Push - receiving files
 *
 * Accepts files pushed from another device: a phone, or a computer sending
 * from its Bluetooth file transfer dialog. Only receiving is implemented, which
 * is the half that has to exist for anything to arrive at all.
 *
 * The name a file arrives with is the sender's, and is taken with suspicion:
 * it is turned into something that can only name a file in the chosen drawer,
 * never a path out of it.
 */

#ifndef BT_OPP_SERVER_H
#define BT_OPP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#if defined __cplusplus
extern "C" {
#endif

/**
 * @brief Start accepting pushed files.
 * @param service_name  what the sender sees the service called
 */
void bt_opp_server_init(const char * service_name);

/** @brief Where received files are written. Defaults to RAM:. */
void bt_opp_server_set_folder(const char * folder);
const char * bt_opp_server_get_folder(void);

/**
 * @brief Is a transfer in progress?
 *
 * True from the moment a connection opens until it closes. The service asks so
 * that it does not page a sleeping device while a file is arriving: a page
 * costs the whole page timeout with the radio elsewhere, and doing it every few
 * seconds through a transfer is what turned fifty kilobytes a second into
 * fifteen, and then into a failure.
 */
bool bt_opp_server_is_busy(void);

/** @brief Log each transfer to the serial debug output */
void bt_opp_server_set_verbose(bool enabled);

#if defined __cplusplus
}
#endif

#endif // BT_OPP_SERVER_H
