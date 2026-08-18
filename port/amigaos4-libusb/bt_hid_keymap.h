/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

/**
 * @title USB HID keyboard usage to Amiga raw key
 *
 * The mapping is positional, not by character: HID usage 0x14 is "the key where
 * Q is on a US keyboard", and RAWKEY 0x10 is the same physical key. What the key
 * produces is then decided by the keymap the user has chosen, so an Italian or
 * German layout comes out right without anything here knowing about it.
 *
 * Codes are from libraries/keymap.h. 0xFFFF means "no Amiga equivalent".
 */

#ifndef BT_HID_KEYMAP_H
#define BT_HID_KEYMAP_H

#include <stdint.h>

#include <devices/inputevent.h>
#include <devices/rawkeycodes.h>
#include <libraries/keymap.h>

#define BT_HID_KEY_NONE 0xFFFF

/* HID Keyboard/Keypad page (0x07), usages 0x00..0x67 */
static const uint16_t bt_hid_keymap[] = {
    /* 0x00 */ BT_HID_KEY_NONE, BT_HID_KEY_NONE, BT_HID_KEY_NONE, BT_HID_KEY_NONE,
    /* 0x04 a-f */ 0x20, 0x35, 0x33, 0x22, 0x12, 0x23,
    /* 0x0a g-l */ 0x24, 0x25, 0x17, 0x26, 0x27, 0x28,
    /* 0x10 m-r */ 0x37, 0x36, 0x18, 0x19, 0x10, 0x13,
    /* 0x16 s-x */ 0x21, 0x14, 0x16, 0x34, 0x11, 0x32,
    /* 0x1c y-z */ 0x15, 0x31,
    /* 0x1e 1-9 */ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    /* 0x27 0   */ 0x0A,
    /* 0x28 */ RAWKEY_RETURN,      /* Enter */
    /* 0x29 */ RAWKEY_ESC,
    /* 0x2a */ RAWKEY_BACKSPACE,
    /* 0x2b */ RAWKEY_TAB,
    /* 0x2c */ RAWKEY_SPACE,
    /* 0x2d */ 0x0B,               /* - */
    /* 0x2e */ 0x0C,               /* = */
    /* 0x2f */ 0x1A,               /* [ */
    /* 0x30 */ 0x1B,               /* ] */
    /* 0x31 */ 0x0D,               /* backslash */
    /* 0x32 */ 0x2B,               /* non-US # */
    /* 0x33 */ 0x29,               /* ; */
    /* 0x34 */ 0x2A,               /* ' */
    /* 0x35 */ 0x00,               /* ` */
    /* 0x36 */ 0x38,               /* , */
    /* 0x37 */ 0x39,               /* . */
    /* 0x38 */ 0x3A,               /* / */
    /* 0x39 */ RAWKEY_CAPSLOCK,
    /* 0x3a F1-F10 */ RAWKEY_F1, RAWKEY_F2, RAWKEY_F3, RAWKEY_F4, RAWKEY_F5,
                      RAWKEY_F6, RAWKEY_F7, RAWKEY_F8, RAWKEY_F9, RAWKEY_F10,
    /* 0x44 */ RAWKEY_F11,
    /* 0x45 */ RAWKEY_F12,
    /* 0x46 */ RAWKEY_PRINTSCR,
    /* 0x47 */ BT_HID_KEY_NONE,    /* Scroll Lock, nothing to map it to */
    /* 0x48 */ RAWKEY_BREAK,       /* Pause */
    /* 0x49 */ RAWKEY_INSERT,
    /* 0x4a */ RAWKEY_HOME,
    /* 0x4b */ RAWKEY_PAGEUP,
    /* 0x4c */ RAWKEY_DEL,
    /* 0x4d */ RAWKEY_END,
    /* 0x4e */ RAWKEY_PAGEDOWN,
    /* 0x4f */ RAWKEY_CRSRRIGHT,
    /* 0x50 */ RAWKEY_CRSRLEFT,
    /* 0x51 */ RAWKEY_CRSRDOWN,
    /* 0x52 */ RAWKEY_CRSRUP,
    /* 0x53 */ BT_HID_KEY_NONE,    /* Num Lock: the Amiga keypad has no such mode */
    /* 0x54 */ 0x5C,               /* KP / */
    /* 0x55 */ 0x5D,               /* KP * */
    /* 0x56 */ 0x4A,               /* KP - */
    /* 0x57 */ 0x5E,               /* KP + */
    /* 0x58 */ RAWKEY_ENTER,       /* KP Enter */
    /* 0x59 KP 1-9 */ RAWKEY_KP_1, RAWKEY_KP_2, RAWKEY_KP_3, RAWKEY_KP_4, RAWKEY_KP_5,
                      RAWKEY_KP_6, RAWKEY_KP_7, RAWKEY_KP_8, RAWKEY_KP_9,
    /* 0x62 */ RAWKEY_KP_0,
    /* 0x63 */ 0x3C,               /* KP . */
    /* 0x64 */ 0x30,               /* non-US backslash, the key next to left shift */
    /* 0x65 */ RAWKEY_MENU,        /* Application */
};

#define BT_HID_KEYMAP_SIZE (sizeof(bt_hid_keymap) / sizeof(bt_hid_keymap[0]))

/* the keypad keys, which have to carry IEQUALIFIER_NUMERICPAD */
#define BT_HID_USAGE_KEYPAD_FIRST 0x54
#define BT_HID_USAGE_KEYPAD_LAST  0x63

/* modifiers, HID usages 0xE0..0xE7 */
#define BT_HID_USAGE_MODIFIER_FIRST 0xE0
#define BT_HID_USAGE_MODIFIER_LAST  0xE7

static const uint16_t bt_hid_modifier_keymap[] = {
    /* 0xe0 */ RAWKEY_LCTRL,
    /* 0xe1 */ RAWKEY_LSHIFT,
    /* 0xe2 */ RAWKEY_LALT,
    /* 0xe3 */ RAWKEY_LCOMMAND,
    /* 0xe4 */ RAWKEY_LCTRL,       /* keymap.h: right Ctrl is the same as left */
    /* 0xe5 */ RAWKEY_RSHIFT,
    /* 0xe6 */ RAWKEY_RALT,
    /* 0xe7 */ RAWKEY_RCOMMAND,
};

static const uint16_t bt_hid_modifier_qualifier[] = {
    /* 0xe0 */ IEQUALIFIER_CONTROL,
    /* 0xe1 */ IEQUALIFIER_LSHIFT,
    /* 0xe2 */ IEQUALIFIER_LALT,
    /* 0xe3 */ IEQUALIFIER_LCOMMAND,
    /* 0xe4 */ IEQUALIFIER_CONTROL,
    /* 0xe5 */ IEQUALIFIER_RSHIFT,
    /* 0xe6 */ IEQUALIFIER_RALT,
    /* 0xe7 */ IEQUALIFIER_RCOMMAND,
};

#endif // BT_HID_KEYMAP_H
