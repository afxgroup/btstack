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
#include "bt_hid_keymap.h"

static uint8_t hid_descriptor_storage[500];

static uint16_t            hids_cid;
static hid_protocol_mode_t protocol_mode = HID_PROTOCOL_MODE_REPORT;
static hci_con_handle_t    hid_con_handle = HCI_CON_HANDLE_INVALID;

/* set by bt_handler_hid_set_verbose() */
static bool verbose;

/*
 * Keyboard state.
 *
 * A HID keyboard reports the set of keys held right now, not presses and
 * releases, so both are derived by comparing with the previous report. Six keys
 * is what the boot protocol carries and what every keyboard reports at least;
 * beyond that a report says "rollover" and is ignored by the parser anyway.
 */
#define MAX_KEYS_DOWN 6
static uint8_t  keys_down[MAX_KEYS_DOWN];
static uint8_t  keys_down_count;
static uint8_t  modifiers_down;      /* bit per HID usage 0xE0..0xE7 */

static uint16_t keyboard_qualifier(void){
    uint16_t qualifier = 0;
    uint8_t i;
    for (i = 0; i < 8; i++){
        if (modifiers_down & (1 << i)){
            qualifier |= bt_hid_modifier_qualifier[i];
        }
    }
    return qualifier;
}

static void keyboard_send(uint8_t usage, bool pressed){
    if (usage >= BT_HID_KEYMAP_SIZE) return;

    uint16_t rawkey = bt_hid_keymap[usage];
    if (rawkey == BT_HID_KEY_NONE) return;

    uint16_t qualifier = keyboard_qualifier();
    if ((usage >= BT_HID_USAGE_KEYPAD_FIRST) && (usage <= BT_HID_USAGE_KEYPAD_LAST)){
        qualifier |= IEQUALIFIER_NUMERICPAD;
    }

    if (verbose){
        DebugPrintF("hid: key usage 0x%02x -> rawkey 0x%02x %s, qualifier 0x%04x\n",
                    usage, rawkey, pressed ? "down" : "up", qualifier);
    }

    amigaos4_input_key(rawkey, pressed, qualifier);
}

/* turn the set of keys in this report into presses and releases */
static void keyboard_handle_report(const uint8_t * new_keys, uint8_t new_count, uint8_t new_modifiers){

    uint8_t i, j;

    /* modifiers first: a shift has to be down before the key it applies to */
    uint8_t changed = modifiers_down ^ new_modifiers;
    if (changed != 0){
        for (i = 0; i < 8; i++){
            if ((changed & (1 << i)) == 0) continue;
            bool pressed = (new_modifiers & (1 << i)) != 0;
            /* update the state first, so the qualifier of this very event is right */
            if (pressed) modifiers_down |=  (1 << i);
            else         modifiers_down &= ~(1 << i);
            amigaos4_input_key(bt_hid_modifier_keymap[i], pressed, keyboard_qualifier());
        }
        modifiers_down = new_modifiers;
    }

    /* released: in the previous report, not in this one */
    for (i = 0; i < keys_down_count; i++){
        bool still_down = false;
        for (j = 0; j < new_count; j++){
            if (keys_down[i] == new_keys[j]){ still_down = true; break; }
        }
        if (!still_down) keyboard_send(keys_down[i], false);
    }

    /* pressed: in this report, not in the previous one */
    for (i = 0; i < new_count; i++){
        bool was_down = false;
        for (j = 0; j < keys_down_count; j++){
            if (new_keys[i] == keys_down[j]){ was_down = true; break; }
        }
        if (!was_down) keyboard_send(new_keys[i], true);
    }

    memcpy(keys_down, new_keys, new_count);
    keys_down_count = new_count;
}

/* let go of everything, so a disconnect cannot leave a key stuck down */
static void keyboard_release_all(void){
    uint8_t i;
    for (i = 0; i < keys_down_count; i++){
        keyboard_send(keys_down[i], false);
    }
    keys_down_count = 0;
    for (i = 0; i < 8; i++){
        if ((modifiers_down & (1 << i)) == 0) continue;
        modifiers_down &= ~(1 << i);
        amigaos4_input_key(bt_hid_modifier_keymap[i], false, keyboard_qualifier());
    }
    modifiers_down = 0;
}

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

    uint8_t new_keys[MAX_KEYS_DOWN];
    uint8_t new_key_count = 0;
    uint8_t new_modifiers = 0;
    bool    have_keys = false;

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
            case HID_USAGE_PAGE_KEYBOARD:
                have_keys = true;
                if (value == 0) break;
                if ((usage >= BT_HID_USAGE_MODIFIER_FIRST) && (usage <= BT_HID_USAGE_MODIFIER_LAST)){
                    new_modifiers |= 1 << (usage - BT_HID_USAGE_MODIFIER_FIRST);
                } else if ((usage != 0) && (new_key_count < MAX_KEYS_DOWN)){
                    new_keys[new_key_count++] = (uint8_t) usage;
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

    /* Only when the report really carried keyboard fields: a device with
     * several report IDs also sends reports without, and treating those as
     * "nothing pressed" would release a key the user is still holding. */
    if (have_keys){
        keyboard_handle_report(new_keys, new_key_count, new_modifiers);
    }
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
            /* do not leave a button or a key pressed behind */
            amigaos4_input_mouse_buttons(0);
            keyboard_release_all();
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
    amigaos4_input_mouse_buttons(0);
    keyboard_release_all();
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
