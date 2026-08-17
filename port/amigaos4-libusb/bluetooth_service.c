/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bluetooth_service.c"

/*
 * BluetoothService - the Bluetooth daemon of this port.
 *
 * Owns the controller and the connections, keeps the list of known devices, and
 * hands each connected device to the profile handler that claims it. Clients -
 * bt.usbfd, the GUI, applications - talk to it through the public MsgPort
 * described in bluetooth_service.h.
 *
 * Started by bt.usbfd when a Bluetooth dongle is plugged in, or by hand.
 *
 * Two rules this program lives by:
 *
 * - it never writes to a console. It injects input events through its handlers,
 *   and Intuition can be blocked waiting for exactly those events while it
 *   drags or sizes a window; the console handler needs Intuition too, so a
 *   printf() from here deadlocks the machine. Diagnostics go to the serial
 *   debug output. The only exception is the startup banner, before any device
 *   is connected.
 * - it never blocks. Everything happens in the run loop: USB, the public port
 *   and the handlers are all data sources.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include <btstack_tlv.h>

#include "btstack_config.h"
#include "btstack.h"

#include "amigaos4_input.h"
#include "bt_handler_hid.h"
#include "bt_profile_handler.h"
#include "bt_service_port.h"
#include "btstack_run_loop_amigaos.h"

#define MAX_DEVICES 32

/* how long to wait for an outgoing connection before giving up */
#define CONNECTION_TIMEOUT_MS 10000

typedef struct {
    BTDeviceInfo                  info;
    hci_con_handle_t              con_handle;
    const bt_profile_handler_t  * handler;
    bool                          in_use;      /* entry allocated */
    bool                          autoconnect; /* reconnect when seen again */
} bt_device_t;

static bt_device_t devices[MAX_DEVICES];

static bool     controller_ready;
static bool     scanning;
static bool     shutdown_requested;

/* device we are currently connecting to, NULL when idle */
static bt_device_t * pending_device;
static btstack_timer_source_t connection_timer;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

/* polled every run loop wake-up: recycles the input.device requests */
static btstack_data_source_t input_data_source;

static bool verbose;

/* -------------------------------------------------------------------------- */
/* device table                                                               */

static bt_device_t * device_for_addr(const bd_addr_t addr){
    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        if (!devices[i].in_use) continue;
        if (memcmp(devices[i].info.bd_addr, addr, 6) == 0) return &devices[i];
    }
    return NULL;
}

static bt_device_t * device_for_handle(hci_con_handle_t con_handle){
    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        if (!devices[i].in_use) continue;
        if (devices[i].con_handle == con_handle) return &devices[i];
    }
    return NULL;
}

static bt_device_t * device_add(const bd_addr_t addr, uint8_t addr_type){
    bt_device_t * device = device_for_addr(addr);
    if (device != NULL) return device;

    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        if (devices[i].in_use) continue;
        memset(&devices[i], 0, sizeof(bt_device_t));
        devices[i].in_use = true;
        devices[i].con_handle = HCI_CON_HANDLE_INVALID;
        memcpy(devices[i].info.bd_addr, addr, 6);
        devices[i].info.addr_type = addr_type;
        devices[i].info.state = BT_DEVICE_STATE_FOUND;
        return &devices[i];
    }
    /* table full: the oldest entries are not evicted on purpose - a device the
     * user is using must never be dropped to make room for one just seen */
    return NULL;
}

static void device_set_state(bt_device_t * device, bt_device_state_t state){
    if (device->info.state == state) return;
    device->info.state = state;
    bt_service_port_notify(BTEVENT_DEVICE_UPDATED, &device->info, 0);
}

/* -------------------------------------------------------------------------- */
/* connecting                                                                 */

static void connection_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (pending_device == NULL) return;
    DebugPrintF("service: connection to %s timed out\n", bd_addr_to_str(pending_device->info.bd_addr));
    gap_connect_cancel();
    device_set_state(pending_device, BT_DEVICE_STATE_BONDED);
    pending_device = NULL;
}

static bt_result_t device_connect(bt_device_t * device){

    if (controller_ready == false) return BT_RESULT_NO_CONTROLLER;
    if (pending_device != NULL)     return BT_RESULT_BUSY;
    if (device->info.state >= BT_DEVICE_STATE_CONNECTED) return BT_RESULT_OK;

    /* scanning and connecting at the same time is not worth the trouble: the
     * controller has to interleave them and both get slower */
    if (scanning){
        gap_stop_scan();
        scanning = false;
        bt_service_port_notify(BTEVENT_SCAN_STOPPED, NULL, 0);
    }

    pending_device = device;
    device_set_state(device, BT_DEVICE_STATE_CONNECTING);

    btstack_run_loop_set_timer_handler(&connection_timer, &connection_timeout_handler);
    btstack_run_loop_set_timer(&connection_timer, CONNECTION_TIMEOUT_MS);
    btstack_run_loop_add_timer(&connection_timer);

    gap_connect(device->info.bd_addr, (bd_addr_type_t) device->info.addr_type);
    return BT_RESULT_OK;
}

/* hand the device to the handler that claimed it */
static void device_attach_handler(bt_device_t * device){

    if (device->handler == NULL) return;

    uint8_t status = device->handler->connect(device->con_handle);
    if (status != ERROR_CODE_SUCCESS){
        DebugPrintF("service: handler '%s' refused %s, status 0x%02x\n",
                    device->handler->name, bd_addr_to_str(device->info.bd_addr), status);
        device->handler = NULL;
        device->info.handler[0] = 0;
        return;
    }

    btstack_strcpy(device->info.handler, sizeof(device->info.handler), device->handler->name);
    device->info.kind = device->handler->kind;
    device_set_state(device, BT_DEVICE_STATE_IN_USE);
    DebugPrintF("service: %s in use by handler '%s'\n",
                bd_addr_to_str(device->info.bd_addr), device->handler->name);
}

/* -------------------------------------------------------------------------- */
/* BTstack events                                                             */

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    bt_device_t * device;

    switch (hci_event_packet_get_type(packet)){

        case BTSTACK_EVENT_STATE:
            switch (btstack_event_state_get_state(packet)){
                case HCI_STATE_WORKING:
                    controller_ready = true;
                    bt_service_port_notify(BTEVENT_CONTROLLER_READY, NULL, 0);
                    DebugPrintF("service: controller ready\n");
                    break;
                case HCI_STATE_OFF:
                    controller_ready = false;
                    bt_service_port_notify(BTEVENT_CONTROLLER_GONE, NULL, 0);
                    if (shutdown_requested){
                        btstack_run_loop_trigger_exit();
                    }
                    break;
                default:
                    break;
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (!scanning) break;

            gap_event_advertising_report_get_address(packet, addr);
            const uint8_t * ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t         ad_len  = gap_event_advertising_report_get_data_length(packet);

            bool is_new = device_for_addr(addr) == NULL;
            device = device_add(addr, gap_event_advertising_report_get_address_type(packet));
            if (device == NULL) break;

            device->info.rssi = (int8_t) gap_event_advertising_report_get_rssi(packet);

            /* remember which handler wants it, so connecting is a decision the
             * user makes and not a guess made later */
            const bt_profile_handler_t * handler = bt_profile_handler_probe(ad_data, ad_len);
            if (handler != NULL){
                device->handler   = handler;
                device->info.kind = handler->kind;
            }

            /* pick up the name when the device advertises one */
            ad_context_t context;
            for (ad_iterator_init(&context, ad_len, ad_data);
                 ad_iterator_has_more(&context);
                 ad_iterator_next(&context)){

                uint8_t data_type = ad_iterator_get_data_type(&context);
                if ((data_type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME) &&
                    (data_type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME)) continue;

                uint8_t name_len = ad_iterator_get_data_len(&context);
                if (name_len >= sizeof(device->info.name)) name_len = sizeof(device->info.name) - 1;
                memcpy(device->info.name, ad_iterator_get_data(&context), name_len);
                device->info.name[name_len] = 0;
                break;
            }

            if (is_new){
                DebugPrintF("service: found %s '%s'%s\n", bd_addr_to_str(addr), device->info.name,
                            handler ? " (supported)" : "");
                bt_service_port_notify(BTEVENT_DEVICE_FOUND, &device->info, 0);
            }
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            btstack_run_loop_remove_timer(&connection_timer);
            if (pending_device == NULL) break;

            pending_device->con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            device_set_state(pending_device, BT_DEVICE_STATE_CONNECTED);
            DebugPrintF("service: connected to %s\n", bd_addr_to_str(pending_device->info.bd_addr));

            /* encrypt before using the device: a HID device will not deliver
             * reports otherwise, and the keys are what makes it reconnect */
            sm_request_pairing(pending_device->con_handle);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            device = device_for_handle(con_handle);
            if (device == NULL) break;

            if (device->handler != NULL){
                device->handler->disconnect(con_handle);
            }
            device->con_handle = HCI_CON_HANDLE_INVALID;
            device->info.handler[0] = 0;
            device_set_state(device, BT_DEVICE_STATE_BONDED);
            DebugPrintF("service: %s disconnected\n", bd_addr_to_str(device->info.bd_addr));

            /* a device we are meant to use comes back on its own */
            if (device->autoconnect && !shutdown_requested){
                device_connect(device);
            }
            break;
        }

        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    bool encrypted = false;
    hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;

    switch (hci_event_packet_get_type(packet)){
        case SM_EVENT_JUST_WORKS_REQUEST:
            /* Just Works has nothing to confirm; a device asking for a passkey
             * is reported to the subscribers instead, so the GUI can ask */
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
            bt_device_t * device = device_for_handle(sm_event_numeric_comparison_request_get_handle(packet));
            bt_service_port_notify(BTEVENT_PAIRING_REQUEST, device ? &device->info : NULL,
                                   sm_event_numeric_comparison_request_get_passkey(packet));
            /* TODO: wait for the GUI to confirm instead of accepting. Until the
             * GUI exists, refusing would make every such device unusable. */
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;
        }

        case SM_EVENT_PAIRING_COMPLETE:
            con_handle = sm_event_pairing_complete_get_handle(packet);
            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS){
                encrypted = true;
            } else {
                DebugPrintF("service: pairing failed, status 0x%02x reason 0x%02x\n",
                            sm_event_pairing_complete_get_status(packet),
                            sm_event_pairing_complete_get_reason(packet));
                gap_disconnect(con_handle);
            }
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE:
            con_handle = sm_event_reencryption_complete_get_handle(packet);
            encrypted = true;
            break;

        default:
            break;
    }

    if (encrypted){
        bt_device_t * device = device_for_handle(con_handle);
        if (device == NULL) return;
        device->autoconnect = true;
        device_attach_handler(device);
        if (pending_device == device){
            pending_device = NULL;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* commands                                                                   */

static bt_result_t handle_command(BTServiceMsg * msg){

    bt_device_t * device;

    switch (msg->bsm_Command){

        case BTCMD_PING:
            return BT_RESULT_OK;

        case BTCMD_SHUTDOWN:
            shutdown_requested = true;
            hci_power_control(HCI_POWER_OFF);
            return BT_RESULT_OK;

        case BTCMD_CONTROLLER_ATTACHED:
            /* bt.usbfd telling us a dongle appeared. When we are already up
             * this is a no-op: the stack found it by itself at startup. */
            return controller_ready ? BT_RESULT_OK : BT_RESULT_NO_CONTROLLER;

        case BTCMD_CONTROLLER_DETACHED:
            controller_ready = false;
            return BT_RESULT_OK;

        case BTCMD_SCAN_START:
            if (controller_ready == false) return BT_RESULT_NO_CONTROLLER;
            if (pending_device != NULL)     return BT_RESULT_BUSY;
            if (!scanning){
                gap_set_scan_parameters(0, 48, 48);
                gap_start_scan();
                scanning = true;
                bt_service_port_notify(BTEVENT_SCAN_STARTED, NULL, 0);
            }
            return BT_RESULT_OK;

        case BTCMD_SCAN_STOP:
            if (scanning){
                gap_stop_scan();
                scanning = false;
                bt_service_port_notify(BTEVENT_SCAN_STOPPED, NULL, 0);
            }
            return BT_RESULT_OK;

        case BTCMD_LIST_DEVICES: {
            if (msg->bsm_Devices == NULL) return BT_RESULT_TOO_SMALL;
            uint32_t count = 0;
            uint8_t i;
            for (i = 0; i < MAX_DEVICES; i++){
                if (!devices[i].in_use) continue;
                if (count >= msg->bsm_DevicesMax){
                    msg->bsm_DevicesCount = count;
                    return BT_RESULT_TOO_SMALL;
                }
                msg->bsm_Devices[count++] = devices[i].info;
            }
            msg->bsm_DevicesCount = count;
            return BT_RESULT_OK;
        }

        case BTCMD_PAIR:
        case BTCMD_CONNECT:
            device = device_for_addr(msg->bsm_Addr);
            if (device == NULL){
                /* connecting to a device we never saw is legitimate: the client
                 * may know its address from a previous run */
                device = device_add(msg->bsm_Addr, msg->bsm_AddrType);
                if (device == NULL) return BT_RESULT_FAILED;
            }
            return device_connect(device);

        case BTCMD_DISCONNECT:
            device = device_for_addr(msg->bsm_Addr);
            if (device == NULL) return BT_RESULT_UNKNOWN_DEVICE;
            device->autoconnect = false;
            if (device->con_handle == HCI_CON_HANDLE_INVALID) return BT_RESULT_OK;
            gap_disconnect(device->con_handle);
            return BT_RESULT_OK;

        case BTCMD_UNPAIR:
            device = device_for_addr(msg->bsm_Addr);
            if (device == NULL) return BT_RESULT_UNKNOWN_DEVICE;
            device->autoconnect = false;
            if (device->con_handle != HCI_CON_HANDLE_INVALID){
                gap_disconnect(device->con_handle);
            }
            gap_delete_bonding((bd_addr_type_t) device->info.addr_type, device->info.bd_addr);
            device->info.state = BT_DEVICE_STATE_FOUND;
            bt_service_port_notify(BTEVENT_DEVICE_UPDATED, &device->info, 0);
            return BT_RESULT_OK;

        case BTCMD_SUBSCRIBE_EVENTS:
            return bt_service_port_subscribe(msg->bsm_EventPort) ? BT_RESULT_OK : BT_RESULT_BUSY;

        case BTCMD_UNSUBSCRIBE_EVENTS:
            bt_service_port_unsubscribe(msg->bsm_EventPort);
            return BT_RESULT_OK;

        default:
            return BT_RESULT_UNSUPPORTED;
    }
}

/* -------------------------------------------------------------------------- */

static void input_poll_ds(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(ds); UNUSED(type);
    amigaos4_input_poll();
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    int i;
    for (i = 1; i < argc; i++){
        if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--verbose") == 0)){
            verbose = true;
        }
    }
    bt_handler_hid_set_verbose(verbose);

    /* startup banner - the console is still safe here, nothing is injected yet */
    printf("BluetoothService starting, port '%s'\n", BLUETOOTH_SERVICE_PORT_NAME);

    if (amigaos4_input_open() == false){
        printf("ERROR: cannot open input.device\n");
        return -1;
    }
    btstack_run_loop_set_data_source_handler(&input_data_source, &input_poll_ds);
    btstack_run_loop_enable_data_source_callbacks(&input_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&input_data_source);

    if (bt_service_port_open(&handle_command) == false){
        printf("ERROR: cannot create the service port - is a service already running?\n");
        return -1;
    }

    l2cap_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    gatt_client_init();

    /* no ATT server: we are a central, and running one opens a re-entrancy in
     * att_server that recurses until the stack overflows. See bthid.c. */

    const bt_profile_handler_t ** handlers = bt_profile_handlers();
    for (i = 0; handlers[i] != NULL; i++){
        handlers[i]->init();
    }

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    hci_power_control(HCI_POWER_ON);
    return 0;
}
