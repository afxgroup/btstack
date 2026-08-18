/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title Bluetooth Classic HID profile handler
 *
 * Keyboards and mice speaking HID over L2CAP. Such a device never shows up in
 * an LE scan: it is found by inquiry and matched on its Class of Device.
 */

#ifndef BT_HANDLER_HID_CLASSIC_H
#define BT_HANDLER_HID_CLASSIC_H

#include "bt_profile_handler.h"

#if defined __cplusplus
extern "C" {
#endif

extern const bt_profile_handler_t bt_handler_hid_classic;

#if defined __cplusplus
}
#endif

#endif // BT_HANDLER_HID_CLASSIC_H
