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
#include <devices/inputevent.h>
#include <libraries/keymap.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include "btstack.h"

#include "amigaos4_input.h"
#include "bt_handler_hid.h"
#include "bt_hid_report.h"

static uint8_t hid_descriptor_storage[500];

static uint16_t            hids_cid;
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;
static hci_con_handle_t    hid_con_handle = HCI_CON_HANDLE_INVALID;

void bt_handler_hid_set_verbose(bool enabled){
    bt_hid_report_set_verbose(enabled);
}

/* -------------------------------------------------------------------------- */

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
            /* do not leave a button or a key pressed behind */
            bt_hid_report_release_all();
            {
                hci_con_handle_t gone = hid_con_handle;
                hid_con_handle = HCI_CON_HANDLE_INVALID;
                bt_profile_handler_report_status(gone, false, ERROR_CODE_SUCCESS);
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            {
                uint8_t service_index = gattservice_subevent_hid_report_get_service_index(packet);
                bt_hid_report_process(
                    hids_host_descriptor_storage_get_descriptor_data(hids_cid, service_index),
                    hids_host_descriptor_storage_get_descriptor_len(hids_cid, service_index),
                    gattservice_subevent_hid_report_get_report(packet),
                    gattservice_subevent_hid_report_get_report_len(packet));
            }
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

    if (ad_data_contains_uuid16(ad_len, (uint8_t *) ad_data,
                                ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)){
        return true;
    }

    /*
     * Not every HID device lists the HID service UUID in its advertisement -
     * plenty of keyboards only carry the Appearance field. Category 0x00F
     * (values 0x03C0..0x03FF) is HID: keyboard 0x03C1, mouse 0x03C2, and so on.
     */
    ad_context_t context;
    for (ad_iterator_init(&context, ad_len, (uint8_t *) ad_data);
         ad_iterator_has_more(&context);
         ad_iterator_next(&context)){
        if (ad_iterator_get_data_type(&context) != BLUETOOTH_DATA_TYPE_APPEARANCE) continue;
        if (ad_iterator_get_data_len(&context) < 2) continue;
        uint16_t appearance = little_endian_read_16(ad_iterator_get_data(&context), 0);
        if ((appearance >> 6) == 0x00F) return true;
    }

    return false;
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
    /* never leave the system with a stuck button or key */
    bt_hid_report_release_all();
    hid_con_handle = HCI_CON_HANDLE_INVALID;
}

const bt_profile_handler_t bt_handler_hid = {
    "hid",
    BT_DEVICE_KIND_MOUSE,
    &hid_handler_init,
    &hid_handler_probe,
    &hid_handler_connect,
    &hid_handler_disconnect,
    NULL,   /* LE only: a Classic device is never ours */
    NULL,
};
