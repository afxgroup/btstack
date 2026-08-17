/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title Run Loop AmigaOS 4
 *
 * Native run loop for AmigaOS 4: waits on timer.device, the MsgPorts registered
 * by the transport and SIGBREAKF_CTRL_C. Used instead of the posix run loop,
 * whose select()/pipe() based wait does not work on AmigaOS 4.
 */

#ifndef BTSTACK_RUN_LOOP_AMIGAOS_H
#define BTSTACK_RUN_LOOP_AMIGAOS_H

#include <exec/types.h>

#include "btstack_run_loop.h"

#if defined __cplusplus
extern "C" {
#endif

/**
 * @brief Get the AmigaOS run loop instance, to be passed to btstack_run_loop_init()
 */
const btstack_run_loop_t * btstack_run_loop_amigaos_get_instance(void);

/**
 * @brief Make the run loop wake up on the signals of a MsgPort, e.g. the port
 *        used for asynchronous USB transfers: 1UL << port->mp_SigBit
 */
void btstack_run_loop_amigaos_add_signal_mask(uint32 mask);

/**
 * @brief Stop waiting on the given signals, e.g. before freeing the MsgPort
 */
void btstack_run_loop_amigaos_remove_signal_mask(uint32 mask);

/**
 * @brief Set handler called on the first CTRL-C, e.g. to power off Bluetooth.
 *        The run loop keeps running so the shutdown can complete; a second
 *        CTRL-C leaves the run loop unconditionally.
 */
void btstack_run_loop_amigaos_set_break_handler(void (*handler)(void));

/**
 * @brief Microsecond clock, same time base as the run loop's get_time_ms().
 *        Wraps after ~71 minutes, which is fine for measuring durations.
 */
uint32_t btstack_run_loop_amigaos_get_time_us(void);

/**
 * @brief Report a CTRL-C that did not arrive as SIGBREAKF_CTRL_C, e.g. the
 *        console in RAW mode delivering it as character 0x03. Handled exactly
 *        like the signal; a key press arriving through both channels counts once.
 */
void btstack_run_loop_amigaos_trigger_break(void);

/**
 * @brief Release timer.device and the run loop resources
 */
void btstack_run_loop_amigaos_deinit(void);

#if defined __cplusplus
}
#endif

#endif // BTSTACK_RUN_LOOP_AMIGAOS_H
