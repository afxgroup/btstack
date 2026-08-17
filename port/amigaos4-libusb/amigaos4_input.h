/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title AmigaOS 4 input event injection
 *
 * Feeds mouse events into input.device with IND_WRITEEVENT, so they appear to
 * the whole system as coming from a real mouse.
 */

#ifndef AMIGAOS4_INPUT_H
#define AMIGAOS4_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#if defined __cplusplus
extern "C" {
#endif

/* button bits used by amigaos4_input_mouse_buttons() - same order as HID */
#define AMIGAOS4_INPUT_BUTTON_LEFT   0x01
#define AMIGAOS4_INPUT_BUTTON_RIGHT  0x02
#define AMIGAOS4_INPUT_BUTTON_MIDDLE 0x04

/**
 * @brief Open input.device
 * @return true on success
 */
bool amigaos4_input_open(void);

/**
 * @brief Release input.device. Releases held buttons first.
 */
void amigaos4_input_close(void);

/**
 * @brief Collect the replies from input.device and send whatever is queued.
 *
 * Events are written asynchronously, never blocking the caller: input.device
 * takes the event through the whole input handler chain, and waiting for that to
 * finish would stall the caller for as long as Intuition is busy. Call this
 * regularly from the main loop, so requests are recycled and the queue drains.
 */
void amigaos4_input_poll(void);

/**
 * @brief Switch the conventions used when building an event, to bisect what
 *        Intuition actually expects. All default to true (except log_buttons).
 *
 * @param timestamps        stamp every event with GetSysTime()
 * @param button_qualifiers carry IEQUALIFIER_LEFTBUTTON/RBUTTON/MIDBUTTON
 * @param relative_flag     set IEQUALIFIER_RELATIVEMOUSE
 * @param log_buttons       print every injected button event
 */
void amigaos4_input_set_options(bool timestamps, bool button_qualifiers, bool relative_flag,
                                bool log_buttons);

/**
 * @brief Print counters: events sent, movement coalesced, events dropped,
 *        longest queue seen, and whether a request is still in flight.
 *        "in flight 1" together with a grown queue means input.device is not
 *        completing our requests.
 */
void amigaos4_input_dump_stats(void);

/**
 * @brief Report relative pointer movement
 */
void amigaos4_input_mouse_move(int16_t dx, int16_t dy);

/**
 * @brief Report the current button state, see AMIGAOS4_INPUT_BUTTON_*.
 *        Only changes against the previous call are sent.
 */
void amigaos4_input_mouse_buttons(uint8_t buttons);

/**
 * @brief Report wheel movement
 */
void amigaos4_input_mouse_wheel(int16_t horizontal, int16_t vertical);

#if defined __cplusplus
}
#endif

#endif // AMIGAOS4_INPUT_H
