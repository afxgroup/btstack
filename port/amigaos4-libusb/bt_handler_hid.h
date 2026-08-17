/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title HID over GATT profile handler
 *
 * Feeds the Input Reports of a Bluetooth LE mouse to input.device.
 */

#ifndef BT_HANDLER_HID_H
#define BT_HANDLER_HID_H

#include "bt_profile_handler.h"

#if defined __cplusplus
extern "C" {
#endif

extern const bt_profile_handler_t bt_handler_hid;

/**
 * @brief Log every decoded report to the serial debug output
 */
void bt_handler_hid_set_verbose(bool enabled);

#if defined __cplusplus
}
#endif

#endif // BT_HANDLER_HID_H
