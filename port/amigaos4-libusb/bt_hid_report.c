/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bt_hid_report.c"

/*
 * HID report decoding and injection, shared by the LE and the Classic handler.
 *
 * Fields are looked up by HID usage, never by offset, so the report layout of
 * the device does not matter - and it varies a lot: 8 or 16 bit deltas, report
 * IDs, extra axes. That is also why this works for both transports without a
 * line of difference: by the time a report gets here, whether it arrived over
 * GATT from an LE mouse or over L2CAP from a Classic keyboard is irrelevant.
 *
 * Nothing here writes to the console. We inject input events, and Intuition can
 * be blocked waiting for them, so printing through the console handler would
 * deadlock the machine; diagnostics go to the serial debug output. See
 * amigaos4_input.c.
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
#include "bt_hid_keymap.h"
#include "bt_hid_report.h"

/* set by bt_hid_report_set_verbose() */
static bool verbose;

void bt_hid_report_set_verbose(bool enabled){
    verbose = enabled;
}

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
static bool     baseline_taken;      /* the first report only records state */

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

    /*
     * Take the first report after connecting as the baseline, without acting on
     * it.
     *
     * A keyboard reports what is held right now, and at the moment it connects
     * we have no idea what that is - nobody was watching. This one latches a
     * modifier when the link drops and hands it straight back as held on the
     * next connection, so every restart began with Ctrl down system wide until
     * the key was pressed and released to clear it.
     *
     * Injecting a modifier the user never pressed is worse than missing one
     * they are holding: the first costs every keystroke afterwards, the second
     * costs pressing the key again. So the state at connection time is recorded
     * and nothing is sent until it changes.
     */
    if (!baseline_taken){
        baseline_taken = true;
        modifiers_down = new_modifiers;
        if (new_modifiers != 0){
            log_info("hid: %02x held at connection, ignored until it changes", new_modifiers);
        }
    }

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
    baseline_taken = false;
}

/**
 * Decode an Input Report and feed it to input.device.
 *
 * - Generic Desktop / X, Y  -> relative pointer movement
 * - Generic Desktop / Wheel -> vertical wheel
 * - Consumer / AC Pan       -> horizontal wheel
 * - Button page 1..3        -> left, right, middle
 */
void bt_hid_report_process(const uint8_t * descriptor, uint16_t descriptor_len,
                           const uint8_t * report, uint16_t report_len){

    if (report_len < 1) return;

    /* No descriptor, no decoding: guessing the layout of a report is exactly
     * what this code refuses to do. A device without one of its own is in boot
     * protocol mode, so the boot descriptor is the right fallback. */
    if ((descriptor == NULL) || (descriptor_len == 0)){
        descriptor     = btstack_hid_get_boot_descriptor_data();
        descriptor_len = btstack_hid_get_boot_descriptor_len();
    }

    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, descriptor, descriptor_len,
                            HID_REPORT_TYPE_INPUT, report, report_len);

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


void bt_hid_report_release_all(void){
    amigaos4_input_mouse_buttons(0);
    keyboard_release_all();
}
