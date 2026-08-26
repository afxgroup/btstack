/*
 * One switch for the service's serial output.
 *
 * Everything here used to print unconditionally, which on a working system is
 * a steady stream of noise down the serial line - and serial is the only place
 * a running service can be watched, so drowning it costs the very thing it is
 * for. Now nothing is printed unless -v was given, and with -v everything comes
 * back exactly as before.
 *
 * The few messages that must always be seen - the startup banner, and failures
 * that stop the service - call DebugPrintF directly and say so where they do.
 */

#ifndef BT_LOG_H
#define BT_LOG_H

#include <stdbool.h>
#include <proto/exec.h>

#if defined __cplusplus
extern "C" {
#endif

extern bool bt_log_enabled;

#define BT_LOG(...) do { if (bt_log_enabled) DebugPrintF(__VA_ARGS__); } while (0)

#if defined __cplusplus
}
#endif

#endif /* BT_LOG_H */
