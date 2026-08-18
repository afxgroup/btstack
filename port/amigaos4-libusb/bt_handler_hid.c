/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bt_handler_hid.c"

/*
 * HID over GATT profile handler: turns a Bluetooth LE mouse into the system
 * mouse by feeding its Input Reports to input.device.
 *
 * Reports are decoded with BTstack's HID parser and fields are looked up by HID
 * usage, never by offset, so the report layout of the device does not matter -
 * and it varies a lot: 8 or 16 bit deltas, report IDs, extra axes.
 *
 * Nothing here writes to the console. The service injects input events and
 * Intuition can be blocked waiting for them, so a print to a console window
 * would deadlock the machine; diagnostics go to the serial debug output.
 * See amigaos4_input.c.
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include "btstack.h"

#include "amigaos4_input.h"
#include "bt_handler_hid.h"

static uint8_t hid_descriptor_storage[500];

static uint16_t            hids_cid;
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;
static hci_con_handle_t    hid_con_handle = HCI_CON_HANDLE_INVALID;

/* set by bt_handler_hid_set_verbose() */
static bool verbose;

void bt_handler_hid_set_verbose(bool enabled){
    verbose = enabled;
}

/* -------------------------------------------------------------------------- */

/**
 * Decode an Input Report and feed it to input.device.
 *
 * - Generic Desktop / X, Y  -> relative pointer movement
 * - Generic Desktop / Wheel -> vertical wheel
 * - Consumer / AC Pan       -> horizontal wheel
 * - Button page 1..3        -> left, right, middle
 */
static void hid_handle_input_report(uint8_t service_index, const uint8_t * report, uint16_t report_len){

    if (report_len < 1) return;

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
                /* Only the first three buttons have an Amiga equivalent; a
                 * mouse with more reports them here too and they are ignored. */
                have_buttons = true;
                if (value != 0){
                    switch (usage){
                        case 1: buttons |= AMIGAOS4_INPUT_BUTTON_LEFT;   break;
                        case 2: buttons |= AMIGAOS4_INPUT_BUTTON_RIGHT;  break;
                        case 3: buttons |= AMIGAOS4_INPUT_BUTTON_MIDDLE; break;
                        default: break;
                    }
                }
                break;
            default:
                break;
        }
    }

    /* ie_x/ie_y are int16 - clamp, or a high resolution mouse wraps around and
     * throws the pointer in the opposite direction */
    if (dx >  32767) dx =  32767;
    if (dx < -32768) dx = -32768;
    if (dy >  32767) dy =  32767;
    if (dy < -32768) dy = -32768;

    if (verbose && ((dx != 0) || (dy != 0) || (wheel != 0) || (pan != 0) || have_buttons)){
        DebugPrintF("hid: dx %5d dy %5d wheel %3d pan %3d buttons %c%c%c\n",
                    (int) dx, (int) dy, (int) wheel, (int) pan,
                    (buttons & AMIGAOS4_INPUT_BUTTON_LEFT)   ? 'L' : '-',
                    (buttons & AMIGAOS4_INPUT_BUTTON_MIDDLE) ? 'M' : '-',
                    (buttons & AMIGAOS4_INPUT_BUTTON_RIGHT)  ? 'R' : '-');
    }

    amigaos4_input_mouse_move((int16_t) dx, (int16_t) dy);

    /* Only touch the buttons when the report actually carried them: a device
     * with several report IDs also sends reports without, and reading those as
     * "all released" would drop a held button. */
    if (have_buttons){
        amigaos4_input_mouse_buttons(buttons);
    }

    /* HID counts the wheel positive away from the user, Amiga positive
     * downwards, hence the negated wheel */
    amigaos4_input_mouse_wheel((int16_t) pan, (int16_t) -wheel);
}

/* -------------------------------------------------------------------------- */

static void hid_gatt_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)){
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            {
                uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
                if (status == ERROR_CODE_SUCCESS){
                    DebugPrintF("hid: service connected, %d instances\n",
                                gattservice_subevent_hid_service_connected_get_num_instances(packet));
                } else {
                    DebugPrintF("hid: service connection failed, status 0x%02x\n", status);
                    hid_con_handle = HCI_CON_HANDLE_INVALID;
                }
                bt_profile_handler_report_status(hid_con_handle, status == ERROR_CODE_SUCCESS, status);
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            DebugPrintF("hid: service disconnected\n");
            /* do not leave a button pressed behind */
            amigaos4_input_mouse_buttons(0);
            {
                hci_con_handle_t gone = hid_con_handle;
                hid_con_handle = HCI_CON_HANDLE_INVALID;
                bt_profile_handler_report_status(gone, false, ERROR_CODE_SUCCESS);
            }
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

/* -------------------------------------------------------------------------- */

static void hid_handler_init(void){
    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
}

static bool hid_handler_probe(const uint8_t * ad_data, uint8_t ad_len){
    return ad_data_contains_uuid16(ad_len, (uint8_t *) ad_data,
                                   ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static uint8_t hid_handler_connect(hci_con_handle_t con_handle){
    /* one device at a time for now: input.device has a single pointer anyway,
     * and a second mouse would just fight the first one */
    if (hid_con_handle != HCI_CON_HANDLE_INVALID){
        return ERROR_CODE_COMMAND_DISALLOWED;
    }
    hid_con_handle = con_handle;
    return hids_host_connect(con_handle, hid_gatt_event_handler, protocol_mode, &hids_cid);
}

static void hid_handler_disconnect(hci_con_handle_t con_handle){
    if (con_handle != hid_con_handle) return;
    /* never leave the system with a stuck mouse button */
    amigaos4_input_mouse_buttons(0);
    hid_con_handle = HCI_CON_HANDLE_INVALID;
}

const bt_profile_handler_t bt_handler_hid = {
    "hid",
    BT_DEVICE_KIND_MOUSE,
    &hid_handler_init,
    &hid_handler_probe,
    &hid_handler_connect,
    &hid_handler_disconnect,
};
