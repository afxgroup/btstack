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

typedef struct bt_profile_handler {
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

    /*
     * Bluetooth Classic. A Classic device does not advertise, so probe() can
     * never see it: it is found by inquiry and identified by its Class of
     * Device instead. Both may be NULL in a handler that only does LE.
     */

    /**
     * @brief Does this handler want the device, judging by its Class of Device?
     */
    bool (*probe_classic)(uint32_t class_of_device);

    /**
     * @brief Connect to a Classic device by address.
     *
     * Unlike connect(), this starts from the address: a Classic profile brings
     * up its own L2CAP channels, so there is no ACL connection to hand over
     * beforehand.
     */
    uint8_t (*connect_addr)(const bd_addr_t addr);
} bt_profile_handler_t;

/**
 * @brief Called when a handler starts or stops driving a device.
 *
 * A handler takes the device over asynchronously - the HID one has to discover
 * services first - so whoever owns the connection only learns from here whether
 * it worked. Without it an application has no way of knowing, which is how
 * bthid ended up sitting at "Search for HID service" forever.
 *
 * The address identifies the device, not the connection handle: a Classic
 * device that reconnects does so by connecting to us, and the service has no
 * handle for it until this very call.
 *
 * The handler is passed too, because it is the authority on what is driving
 * the device: a device that connected to us on its own was never probed, so
 * there is nothing else to deduce it from.
 *
 * @param handler    the handler reporting
 * @param addr       the device
 * @param con_handle its connection
 * @param in_use     true when the handler is now driving it
 * @param status     ERROR_CODE_SUCCESS, or why it failed
 */
typedef void (*bt_profile_status_callback_t)(const struct bt_profile_handler * handler,
                                             const bd_addr_t addr, hci_con_handle_t con_handle,
                                             bool in_use, uint8_t status);

void bt_profile_handler_set_status_callback(bt_profile_status_callback_t callback);

/** @brief Called by the handlers themselves */
void bt_profile_handler_report_status(const struct bt_profile_handler * handler,
                                      const bd_addr_t addr, hci_con_handle_t con_handle,
                                      bool in_use, uint8_t status);

/**
 * @brief The handlers built into the service, NULL terminated.
 */
const bt_profile_handler_t ** bt_profile_handlers(void);

/* the handler able to drive a Classic device of this kind, for devices
 * restored from storage that were never probed */
const bt_profile_handler_t * bt_profile_handler_classic_for_kind(bt_device_kind_t kind);

/**
 * @brief First handler claiming a device from its advertisement, NULL if none.
 */
const bt_profile_handler_t * bt_profile_handler_probe(const uint8_t * ad_data, uint8_t ad_len);

/**
 * @brief First handler claiming a Classic device by its Class of Device.
 */
const bt_profile_handler_t * bt_profile_handler_probe_classic(uint32_t class_of_device);

#if defined __cplusplus
}
#endif

#endif // BT_PROFILE_HANDLER_H
