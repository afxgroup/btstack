/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bt_service_port.c"

/*
 * Public MsgPort of the Bluetooth service.
 *
 * Clients - bt.usbfd, the GUI, applications - send a BTServiceMsg here and get
 * it replied. The port is added to the run loop's signal mask, so serving
 * clients costs nothing while idle and needs no thread: the run loop wakes up
 * on the port's signal exactly like it does for a USB transfer.
 *
 * Messages are handled from a data source, that is from the run loop, never
 * from an interrupt or another task, so the command handlers can call BTstack
 * directly without any locking.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/exectags.h>
#include <exec/ports.h>
#include <proto/exec.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_amigaos.h"

#include "bt_service_port.h"

#define MAX_SUBSCRIBERS 8

static struct MsgPort      * service_port;
static btstack_data_source_t service_data_source;
static bt_service_command_handler_t command_handler;

/* clients that asked to be notified of changes */
static struct MsgPort * subscribers[MAX_SUBSCRIBERS];

/* -------------------------------------------------------------------------- */

static void bt_service_port_process(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(ds);
    UNUSED(type);

    if (service_port == NULL) return;

    struct Message * message;
    while ((message = GetMsg(service_port)) != NULL){

        /*
         * Events are one way now and never come back, so anything marked
         * NT_REPLYMSG is from a client built against the old protocol. Freeing
         * it is all that can be done with it.
         */
        if (message->mn_Node.ln_Type == NT_REPLYMSG){
            FreeSysObject(ASOT_MESSAGE, message);
            continue;
        }

        BTServiceMsg * msg = (BTServiceMsg *) message;

        /*
         * Never trust a client blindly: a message that is too short or from a
         * future protocol version is rejected instead of interpreted.
         */
        if ((msg->bsm_Message.mn_Length < sizeof(BTServiceMsg)) ||
            (msg->bsm_Version != BLUETOOTH_SERVICE_VERSION)){
            msg->bsm_Result = BT_RESULT_UNSUPPORTED;
        } else if (command_handler != NULL){
            msg->bsm_Result = command_handler(msg);
        } else {
            msg->bsm_Result = BT_RESULT_FAILED;
        }

        ReplyMsg(message);
    }
}

/* -------------------------------------------------------------------------- */

bool bt_service_port_open(bt_service_command_handler_t handler){

    if (service_port != NULL) return true;

    /*
     * Fail if a service is already running instead of creating a second public
     * port with the same name: two Bluetooth stacks fighting over one
     * controller is not something to find out about later.
     */
    Forbid();
    bool already_running = FindPort(BLUETOOTH_SERVICE_PORT_NAME) != NULL;
    Permit();
    if (already_running){
        log_error("a Bluetooth service is already running");
        return false;
    }

    service_port = AllocSysObjectTags(ASOT_PORT,
                                      ASOPORT_Name,   BLUETOOTH_SERVICE_PORT_NAME,
                                      ASOPORT_Public, TRUE,
                                      TAG_END);
    if (service_port == NULL){
        log_error("cannot create the public service port");
        return false;
    }

    command_handler = handler;

    /* wake the run loop up when a client sends something */
    btstack_run_loop_amigaos_add_signal_mask(1UL << service_port->mp_SigBit);

    btstack_run_loop_set_data_source_handler(&service_data_source, &bt_service_port_process);
    btstack_run_loop_enable_data_source_callbacks(&service_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&service_data_source);

    return true;
}

void bt_service_port_close(void){

    if (service_port == NULL) return;

    btstack_run_loop_remove_data_source(&service_data_source);
    btstack_run_loop_amigaos_remove_signal_mask(1UL << service_port->mp_SigBit);

    /*
     * Remove the port from the public list before draining it, so no client can
     * still find it and post a message we would never reply to.
     */
    Forbid();
    RemPort(service_port);
    struct Message * message;
    while ((message = GetMsg(service_port)) != NULL){
        if (message->mn_Node.ln_Type == NT_REPLYMSG){
            FreeSysObject(ASOT_MESSAGE, message);
            continue;
        }
        BTServiceMsg * msg = (BTServiceMsg *) message;
        msg->bsm_Result = BT_RESULT_FAILED;
        ReplyMsg(message);
    }
    Permit();

    /*
     * An event message still held by a subscriber is leaked, which is what the
     * one way protocol trades for being safe: waiting for it would hang
     * shutdown on a client that is not answering, and freeing the port with
     * replies outstanding is what used to crash them.
     */

    SetSignal(0, 1UL << service_port->mp_SigBit);
    FreeSysObject(ASOT_PORT, service_port);
    service_port = NULL;
    command_handler = NULL;

    memset(subscribers, 0, sizeof(subscribers));
}

/* -------------------------------------------------------------------------- */

bool bt_service_port_subscribe(struct MsgPort * port){
    if (port == NULL) return false;
    uint8_t i;
    for (i = 0; i < MAX_SUBSCRIBERS; i++){
        if (subscribers[i] == port) return true;   // already subscribed
    }
    for (i = 0; i < MAX_SUBSCRIBERS; i++){
        if (subscribers[i] == NULL){
            subscribers[i] = port;
            return true;
        }
    }
    return false;
}

void bt_service_port_unsubscribe(struct MsgPort * port){
    uint8_t i;
    for (i = 0; i < MAX_SUBSCRIBERS; i++){
        if (subscribers[i] == port){
            subscribers[i] = NULL;
        }
    }
}

void bt_service_port_notify(bt_event_t event, const BTDeviceInfo * device, uint32_t passkey){

    uint8_t i;
    for (i = 0; i < MAX_SUBSCRIBERS; i++){
        if (subscribers[i] == NULL) continue;

        /*
         * One message per subscriber, allocated by us and freed by them.
         *
         * No reply port, deliberately. These used to come back to our own
         * public port, which we free when the service stops - so a subscriber
         * still holding one replied into memory that was gone and took itself
         * down with it. Nothing it could have checked would have saved it.
         */
        BTServiceEvent * ev = AllocSysObjectTags(ASOT_MESSAGE,
                                                 ASOMSG_Size, sizeof(BTServiceEvent),
                                                 TAG_END);
        if (ev == NULL) return;

        ev->bse_Event   = event;
        ev->bse_Passkey = passkey;
        if (device != NULL){
            ev->bse_Device = *device;
        } else {
            memset(&ev->bse_Device, 0, sizeof(ev->bse_Device));
        }

        PutMsg(subscribers[i], &ev->bse_Message);
    }
}

struct MsgPort * bt_service_port_get(void){
    return service_port;
}
