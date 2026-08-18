/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bthid.c"

/*
 * bthid - use a Bluetooth LE mouse as a system mouse on AmigaOS 4.
 *
 * Scans for LE HID devices, connects and pairs with the first one found,
 * subscribes to its Input Reports and feeds them into input.device, so the
 * pointer moves for the whole system (see amigaos4_input.c).
 *
 * The connection state machine follows example/hog_host_demo.c; the reports are
 * not dumped but decoded with BTstack's HID parser and translated into Amiga
 * input events. Decoding goes through the report descriptor of the device, so
 * any mouse works regardless of how it lays out its reports - which matters,
 * as report layouts differ wildly (8 or 16 bit deltas, report IDs, extra axes).
 *
 * Once bonded the device is stored in the TLV, so the next start reconnects
 * without scanning. Quit with CTRL-C.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)); BTstack needs the
 * (void) form, and without this every UNUSED(x) below breaks the parse */
#undef UNUSED

#include <btstack_tlv.h>

#include "btstack_config.h"
#include "btstack.h"

#include "amigaos4_input.h"
#include "bt_handler_hid.h"
#include "bt_profile_handler.h"
#include "btstack_run_loop_amigaos.h"


#define TLV_TAG_HOGD ((((uint32_t) 'H') << 24 ) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'D')

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

static enum {
    W4_WORKING,
    W4_HID_DEVICE_FOUND,
    W4_CONNECTED,
    W4_ENCRYPTED,
    W4_HID_CLIENT_CONNECTED,
    READY,
    W4_TIMEOUT_THEN_SCAN,
    W4_TIMEOUT_THEN_RECONNECT,
} app_state;

static le_device_addr_t    remote_device;
static hci_con_handle_t    connection_handle;

static btstack_timer_source_t connection_timer;

/* polled on every run loop wake-up: recycles the input.device requests and
 * drains events that had to be queued while the device was busy */
static btstack_data_source_t input_data_source;

/*
 * Progress messages go to the console while connecting, and to the serial debug
 * output once we are the system mouse.
 *
 * The shell window is an Intuition window, so printf() goes through the console
 * handler, which needs Intuition - and Intuition can be busy dragging or sizing
 * a window, waiting for the button release that only we can deliver. Printing
 * then blocks us, we stop reading USB, the release never goes out, and the
 * machine is stuck until a real mouse event breaks the cycle. Before READY no
 * event is being injected, so the console is safe and the user gets to watch
 * the connection come up.
 */
static void bthid_log(const char * format, ...){
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (app_state == READY){
        DebugPrintF("%s", buffer);
    } else {
        printf("%s", buffer);
        fflush(stdout);
    }
}

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static const btstack_tlv_t * btstack_tlv_singleton_impl;
static void *                btstack_tlv_singleton_context;

// report movement and button changes, so mouse activity can be watched
static bool verbose;

/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */

/* the handler drives the device asynchronously, so this is where we learn
 * whether it actually took it over */
static void handler_status(hci_con_handle_t con_handle, bool in_use, uint8_t status){
    UNUSED(con_handle);
    if (in_use){
        app_state = READY;
        bthid_log("Ready - the mouse now controls the system pointer.\n");
    } else if (status != ERROR_CODE_SUCCESS){
        bthid_log("HID handler failed, status 0x%02x\n", status);
        gap_disconnect(connection_handle);
    }
}

static void input_poll_ds(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(ds); UNUSED(type);
    amigaos4_input_poll();
}

static bool adv_event_contains_hid_service(const uint8_t * packet){
    const uint8_t * ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void hog_start_scan(void){
    bthid_log("Scanning for LE HID devices...\n");
    app_state = W4_HID_DEVICE_FOUND;
    // Passive scanning, 100% (scan interval = scan window)
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void hog_connection_timeout(btstack_timer_source_t * ts){
    UNUSED(ts);
    bthid_log("Timeout - abort connection\n");
    gap_connect_cancel();
    hog_start_scan();
}

static void hog_connect(void){
    btstack_run_loop_set_timer(&connection_timer, 10000);
    btstack_run_loop_set_timer_handler(&connection_timer, &hog_connection_timeout);
    btstack_run_loop_add_timer(&connection_timer);
    app_state = W4_CONNECTED;
    gap_connect(remote_device.addr, remote_device.addr_type);
}

static void hog_reconnect_timeout(btstack_timer_source_t * ts){
    UNUSED(ts);
    switch (app_state){
        case W4_TIMEOUT_THEN_RECONNECT:
            hog_connect();
            break;
        case W4_TIMEOUT_THEN_SCAN:
            hog_start_scan();
            break;
        default:
            break;
    }
}

static void hog_start_connect(void){
    // reconnect to the bonded device if there is one
    btstack_tlv_get_instance(&btstack_tlv_singleton_impl, &btstack_tlv_singleton_context);
    if (btstack_tlv_singleton_impl){
        int len = btstack_tlv_singleton_impl->get_tag(btstack_tlv_singleton_context, TLV_TAG_HOGD,
                                                     (uint8_t *) &remote_device, sizeof(remote_device));
        if (len == sizeof(remote_device)){
            bthid_log("Bonded, connect to device with %s address %s ...\n",
                   remote_device.addr_type == 0 ? "public" : "random", bd_addr_to_str(remote_device.addr));
            hog_connect();
            return;
        }
    }
    hog_start_scan();
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t event;

    if (packet_type != HCI_EVENT_PACKET) return;

    event = hci_event_packet_get_type(packet);
    switch (event){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            btstack_assert(app_state == W4_WORKING);
            hog_start_connect();
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            if (app_state != W4_HID_DEVICE_FOUND) break;
            if (adv_event_contains_hid_service(packet) == false) break;
            gap_stop_scan();
            gap_event_advertising_report_get_address(packet, remote_device.addr);
            remote_device.addr_type = gap_event_advertising_report_get_address_type(packet);
            bthid_log("Found, connect to device with %s address %s ...\n",
                   remote_device.addr_type == 0 ? "public" : "random", bd_addr_to_str(remote_device.addr));
            hog_connect();
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (app_state != READY) break;
            connection_handle = HCI_CON_HANDLE_INVALID;
            // release held buttons, the mouse is gone
            amigaos4_input_mouse_buttons(0);
            bthid_log("\nDisconnected, try to reconnect...\n");
            app_state = W4_TIMEOUT_THEN_RECONNECT;
            btstack_run_loop_set_timer(&connection_timer, 100);
            btstack_run_loop_set_timer_handler(&connection_timer, &hog_reconnect_timeout);
            btstack_run_loop_add_timer(&connection_timer);
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            if (app_state != W4_CONNECTED) return;
            btstack_run_loop_remove_timer(&connection_timer);
            connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            app_state = W4_ENCRYPTED;
            sm_request_pairing(connection_handle);
            break;
        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    bool connect_to_service = false;

    switch (hci_event_packet_get_type(packet)){
        case SM_EVENT_JUST_WORKS_REQUEST:
            bthid_log("Just works requested\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            bthid_log("Confirming numeric comparison: %"PRIu32"\n", sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_passkey_display_number_get_handle(packet));
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            bthid_log("Display Passkey: %"PRIu32"\n", sm_event_passkey_display_number_get_passkey(packet));
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)){
                case ERROR_CODE_SUCCESS:
                    bthid_log("Pairing complete, success\n");
                    connect_to_service = true;
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    bthid_log("Pairing failed, timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    bthid_log("Pairing failed, disconnected\n");
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    bthid_log("Pairing failed, reason = %u\n", sm_event_pairing_complete_get_reason(packet));
                    bthid_log("Hint: a device without LE Secure Connections needs the -p option\n");
                    break;
                default:
                    break;
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            bthid_log("Re-encryption complete, success\n");
            connect_to_service = true;
            break;
        default:
            break;
    }

    if (connect_to_service){
        bthid_log("Search for HID service.\n");
        app_state = W4_HID_CLIENT_CONNECTED;
        bt_handler_hid.connect(connection_handle);
    }
}

/* -------------------------------------------------------------------------- */

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    /* event convention switches, to find out what Intuition wants - see
     * amigaos4_input_set_options() */
    bool opt_timestamps        = true;
    bool opt_button_qualifiers = true;
    bool opt_relative_flag     = true;
    bool opt_log_buttons       = false;

    int i;
    for (i = 1; i < argc; i++){
        if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--verbose") == 0)){
            verbose = true;
        }
        if (strcmp(argv[i], "-s") == 0) opt_timestamps = false;         // no time stamps
        if (strcmp(argv[i], "-q") == 0) opt_button_qualifiers = false;  // no button qualifiers
        if (strcmp(argv[i], "-R") == 0) opt_relative_flag = false;      // no RELATIVEMOUSE
        if (strcmp(argv[i], "-b") == 0) opt_log_buttons = true;         // log button events
    }

    if (amigaos4_input_open() == false){
        printf("ERROR: cannot open input.device - mouse events cannot be injected\n");
        return -1;
    }

    amigaos4_input_set_options(opt_timestamps, opt_button_qualifiers, opt_relative_flag,
                               opt_log_buttons);

    /* events are written asynchronously, so the replies have to be collected */
    btstack_run_loop_set_data_source_handler(&input_data_source, &input_poll_ds);
    btstack_run_loop_enable_data_source_callbacks(&input_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&input_data_source);

    l2cap_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    gatt_client_init();

    /*
     * No ATT server.
     *
     * hog_host_demo starts one "in case the LE Peripheral does ATT queries on
     * its own", but a mouse does not, and it is the path that crashed here:
     * l2cap_request_can_send_fix_channel_now_event() calls
     * l2cap_notify_channel_can_send() synchronously, which emits CAN_SEND_NOW to
     * att_server, whose handler re-requests can-send-now at the end - straight
     * back into l2cap_notify_channel_can_send(). While HCI keeps answering "you
     * may send", that recursion never unwinds and blows the stack.
     *
     * We are a HID host: without the server the loop cannot be entered at all.
     */

    bt_profile_handler_set_status_callback(&handler_status);
    bt_handler_hid.init();

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    app_state = W4_WORKING;

    hci_power_control(HCI_POWER_ON);
    return 0;
}
