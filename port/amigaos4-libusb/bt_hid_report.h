/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title HID report decoding and injection
 *
 * Turns a HID Input Report into AmigaOS input events, for mice and keyboards.
 *
 * Shared by both HID handlers on purpose: a report from a Classic keyboard over
 * L2CAP and one from an LE mouse over GATT are the same thing by the time they
 * get here, and nothing below this line needs to know which transport carried
 * it. Only the report descriptor and the report itself matter.
 */

#ifndef BT_HID_REPORT_H
#define BT_HID_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#if defined __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one Input Report and feed the resulting events to input.device.
 *
 * @param descriptor      the device's HID report descriptor
 * @param descriptor_len
 * @param report          the Input Report as received
 * @param report_len
 */
void bt_hid_report_process(const uint8_t * descriptor, uint16_t descriptor_len,
                           const uint8_t * report, uint16_t report_len);

/**
 * @brief Release every held button and key.
 *
 * Call when a device disconnects: without it a key or a mouse button that was
 * down at that moment stays down for the whole system, and nothing else can
 * put it back up.
 */
void bt_hid_report_release_all(void);

/**
 * @brief Log every decoded report to the serial debug output
 */
void bt_hid_report_set_verbose(bool enabled);

#if defined __cplusplus
}
#endif

#endif // BT_HID_REPORT_H
