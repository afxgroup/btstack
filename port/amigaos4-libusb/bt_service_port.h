/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title Public MsgPort of the Bluetooth service
 *
 * Handles the plumbing - the public port, the run loop integration and the
 * subscriber list - so the service itself only has to implement the commands.
 */

#ifndef BT_SERVICE_PORT_H
#define BT_SERVICE_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include <exec/ports.h>

#include "bluetooth_service.h"

#if defined __cplusplus
extern "C" {
#endif

/**
 * @brief Called from the run loop for each command, must fill in the result.
 */
typedef bt_result_t (*bt_service_command_handler_t)(BTServiceMsg * msg);

/**
 * @brief Create the public port and hook it into the run loop.
 * @return false if the port cannot be created, or a service is already running
 */
bool bt_service_port_open(bt_service_command_handler_t handler);

/**
 * @brief Remove the port; pending commands are replied with BT_RESULT_FAILED.
 */
void bt_service_port_close(void);

/**
 * @brief Add a client port to the list receiving BTServiceEvent messages.
 * @return false if there is no room left
 */
bool bt_service_port_subscribe(struct MsgPort * port);

void bt_service_port_unsubscribe(struct MsgPort * port);

/**
 * @brief Send an event to every subscriber. Never blocks.
 */
void bt_service_port_notify(bt_event_t event, const BTDeviceInfo * device, uint32_t passkey);

/**
 * @brief The public port, e.g. to use as a reply port
 */
struct MsgPort * bt_service_port_get(void);

#if defined __cplusplus
}
#endif

#endif // BT_SERVICE_PORT_H
