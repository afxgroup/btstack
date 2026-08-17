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
static uint16_t            hids_cid;
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;

static uint8_t hid_descriptor_storage[500];

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

/* -t: measure where the time per report goes. Splits the two things we control
 * (decoding the report, writing to input.device) from everything else, so a CPU
 * problem can be attributed instead of guessed. */
static bool     timing;
#define TIMING_REPORT_INTERVAL 100
static uint32_t timing_reports;
static uint32_t timing_parse_us;
static uint32_t timing_inject_us;
static uint32_t timing_window_start_us;

/* -------------------------------------------------------------------------- */

/**
 * Decode an Input Report and feed it to input.device.
 *
 * Fields are looked up by HID usage instead of by offset:
 * - Generic Desktop / X, Y      -> relative pointer movement
 * - Generic Desktop / Wheel     -> vertical wheel
 * - Consumer / AC Pan           -> horizontal wheel
 * - Button page, buttons 1..3   -> left, right, middle
 */
static void hid_handle_input_report(uint8_t service_index, const uint8_t * report, uint16_t report_len){

    if (report_len < 1) return;

    uint32_t t_start = timing ? btstack_run_loop_amigaos_get_time_us() : 0;

    btstack_hid_parser_t parser;
    switch (protocol_mode){
        case HID_PROTOCOL_MODE_BOOT:
            btstack_hid_parser_init(&parser,
                                    btstack_hid_get_boot_descriptor_data(),
                                    btstack_hid_get_boot_descriptor_len(),
                                    HID_REPORT_TYPE_INPUT, report, report_len);
            break;
        default:
            btstack_hid_parser_init(&parser,
                                    hids_host_descriptor_storage_get_descriptor_data(hids_cid, service_index),
                                    hids_host_descriptor_storage_get_descriptor_len(hids_cid, service_index),
                                    HID_REPORT_TYPE_INPUT, report, report_len);
            break;
    }

    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel = 0;
    int32_t pan = 0;
    uint8_t buttons = 0;
    bool    have_buttons = false;

    while (btstack_hid_parser_has_more(&parser)){
        uint16_t usage_page;
        uint16_t usage;
        int32_t  value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);

        switch (usage_page){
            case HID_USAGE_PAGE_DESKTOP:
                switch (usage){
                    case 0x30:  // X
                        dx = value;
                        break;
                    case 0x31:  // Y
                        dy = value;
                        break;
                    case 0x38:  // Wheel
                        wheel = value;
                        break;
                    default:
                        break;
                }
                break;
            case HID_USAGE_PAGE_CONSUMER:
                if (usage == 0x0238){   // AC Pan
                    pan = value;
                }
                break;
            case HID_USAGE_PAGE_BUTTON:
                /* Buttons are numbered from 1. A mouse with more buttons also
                 * reports them here; only the first three have an Amiga
                 * equivalent, the rest is ignored. */
                have_buttons = true;
                if (value != 0){
                    switch (usage){
                        case 1:
                            buttons |= AMIGAOS4_INPUT_BUTTON_LEFT;
                            break;
                        case 2:
                            buttons |= AMIGAOS4_INPUT_BUTTON_RIGHT;
                            break;
                        case 3:
                            buttons |= AMIGAOS4_INPUT_BUTTON_MIDDLE;
                            break;
                        default:
                            break;
                    }
                }
                break;
            default:
                break;
        }
    }

    /* ie_x/ie_y are int16 - clamp instead of letting a high resolution mouse
     * wrap around and send the pointer in the opposite direction */
    if (dx >  32767) dx =  32767;
    if (dx < -32768) dx = -32768;
    if (dy >  32767) dy =  32767;
    if (dy < -32768) dy = -32768;

    /* Serial, never the console: printing to the shell window from here blocks
     * as soon as Intuition is dragging or sizing a window, and that deadlocks
     * the machine - we would stop reading USB and never deliver the button
     * release Intuition is waiting for. See amigaos4_input.c. */
    if (verbose && ((dx != 0) || (dy != 0) || (wheel != 0) || (pan != 0) || have_buttons)){
        DebugPrintF("mouse: dx %5d dy %5d wheel %3d pan %3d buttons %c%c%c\n",
               (int) dx, (int) dy, (int) wheel, (int) pan,
               (buttons & AMIGAOS4_INPUT_BUTTON_LEFT)   ? 'L' : '-',
               (buttons & AMIGAOS4_INPUT_BUTTON_MIDDLE) ? 'M' : '-',
               (buttons & AMIGAOS4_INPUT_BUTTON_RIGHT)  ? 'R' : '-');
    }

    uint32_t t_parsed = timing ? btstack_run_loop_amigaos_get_time_us() : 0;

    amigaos4_input_mouse_move((int16_t) dx, (int16_t) dy);

    /* Only touch the button state when the report actually carried buttons:
     * a device with several report IDs may send reports without them, and
     * treating those as "all released" would drop a held button. */
    if (have_buttons){
        amigaos4_input_mouse_buttons(buttons);
    }

    /* HID reports the wheel with positive values away from the user; Amiga
     * expects a positive Y for a downwards scroll, hence the negated wheel. */
    amigaos4_input_mouse_wheel((int16_t) pan, (int16_t) -wheel);

    if (timing == false) return;

    uint32_t t_end = btstack_run_loop_amigaos_get_time_us();
    timing_parse_us  += t_parsed - t_start;
    timing_inject_us += t_end - t_parsed;
    timing_reports++;

    if (timing_reports >= TIMING_REPORT_INTERVAL){
        uint32_t window_us = t_end - timing_window_start_us;
        /* reports/s over the window, and the share of wall time we spend in the
         * two stages - the rest is BTstack plus the USB transport */
        DebugPrintF("timing: %lu reports in %lu ms (%lu/s) - decode %lu us avg, input.device %lu us avg, %lu%% of wall time\n",
               (unsigned long) timing_reports,
               (unsigned long) (window_us / 1000),
               (unsigned long) (window_us ? (timing_reports * 1000000UL) / window_us : 0),
               (unsigned long) (timing_parse_us / timing_reports),
               (unsigned long) (timing_inject_us / timing_reports),
               (unsigned long) (window_us ? ((timing_parse_us + timing_inject_us) * 100UL) / window_us : 0));
        amigaos4_input_dump_stats();
        timing_reports = 0;
        timing_parse_us = 0;
        timing_inject_us = 0;
        timing_window_start_us = t_end;
    }
}

/* -------------------------------------------------------------------------- */

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

static void handle_outgoing_connection_error(void){
    bthid_log("Error occurred, disconnect and start over\n");
    gap_disconnect(connection_handle);
    hog_start_scan();
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t status;

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)){
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            switch (status){
                case ERROR_CODE_SUCCESS:
                    bthid_log("HID service client connected, found %d services\n",
                           gattservice_subevent_hid_service_connected_get_num_instances(packet));
                    // store device as bonded
                    if (btstack_tlv_singleton_impl){
                        btstack_tlv_singleton_impl->store_tag(btstack_tlv_singleton_context, TLV_TAG_HOGD,
                                                              (const uint8_t *) &remote_device, sizeof(remote_device));
                    }
                    bthid_log("Ready - the mouse now controls the system pointer.\n");
                    app_state = READY;
                    break;
                default:
                    bthid_log("HID service client connection failed, status 0x%02x.\n", status);
                    handle_outgoing_connection_error();
                    break;
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            bthid_log("HID service client disconnected\n");
            // do not leave a button pressed behind
            amigaos4_input_mouse_buttons(0);
            hog_start_connect();
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            hid_handle_input_report(gattservice_subevent_hid_report_get_service_index(packet),
                                    gattservice_subevent_hid_report_get_report(packet),
                                    gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            break;
    }
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
        hids_host_connect(connection_handle, handle_gatt_client_event, protocol_mode, &hids_cid);
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
        if ((strcmp(argv[i], "-t") == 0) || (strcmp(argv[i], "--timing") == 0)){
            timing = true;
            timing_window_start_us = btstack_run_loop_amigaos_get_time_us();
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

    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    app_state = W4_WORKING;

    hci_power_control(HCI_POWER_ON);
    return 0;
}
