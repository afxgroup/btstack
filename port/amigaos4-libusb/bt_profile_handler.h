/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title Bluetooth profile handlers
 *
 * A profile handler is what actually makes a connected device useful: the HID
 * handler turns reports into input.device events, an audio handler would feed
 * AHI, and so on. The service owns the controller and the connections and hands
 * each device to the first handler that claims it.
 *
 * Keeping this behind a small vtable is what allows a new profile to be added
 * without touching the service.
 */

#ifndef BT_PROFILE_HANDLER_H
#define BT_PROFILE_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"
#include "bluetooth_service.h"

#if defined __cplusplus
extern "C" {
#endif

typedef struct {
    /** short name, reported to clients in BTDeviceInfo.handler */
    const char * name;

    /** what this handler makes a device look like to the user */
    bt_device_kind_t kind;

    /** called once at startup */
    void (*init)(void);

    /**
     * @brief Does this handler want the device?
     * @param ad_data advertising data of the device
     * @param ad_len
     * @return true if the handler claims it
     */
    bool (*probe)(const uint8_t * ad_data, uint8_t ad_len);

    /**
     * @brief Start using a connected and encrypted device.
     * @return ERROR_CODE_SUCCESS or an error, in which case the service
     *         disconnects the device again
     */
    uint8_t (*connect)(hci_con_handle_t con_handle);

    /** @brief Stop using the device; also called when it just disappeared */
    void (*disconnect)(hci_con_handle_t con_handle);
} bt_profile_handler_t;

/**
 * @brief The handlers built into the service, NULL terminated.
 */
const bt_profile_handler_t ** bt_profile_handlers(void);

/**
 * @brief First handler claiming a device from its advertisement, NULL if none.
 */
const bt_profile_handler_t * bt_profile_handler_probe(const uint8_t * ad_data, uint8_t ad_len);

#if defined __cplusplus
}
#endif

#endif // BT_PROFILE_HANDLER_H
