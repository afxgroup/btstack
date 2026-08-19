#ifndef BLUETOOTH_GUI_CAT_H
#define BLUETOOTH_GUI_CAT_H

/*
 * String IDs and their built-in English text, in the shape CatComp produces.
 *
 * Written by hand because CatComp is an Amiga tool and this cross builds on a
 * host that has none, but deliberately kept to the same layout: running CatComp
 * over BluetoothGUI.cd should produce a drop-in replacement for this file.
 *
 * The text here is the fallback GetString() hands to GetCatalogStr(), so the
 * program is fully readable with no catalog installed at all - a translation
 * that is missing, or in a language nobody chose, costs nothing.
 */

#include <exec/types.h>

#define MSG_WINDOW_TITLE         1000
#define MSG_NEARBY               1001
#define MSG_KNOWN                1002
#define MSG_CI_NAME              1003
#define MSG_CI_ADDRESS           1004
#define MSG_CI_SIGNAL            1005
#define MSG_CI_KIND              1006
#define MSG_CI_STATE             1007
#define MSG_BUTTON_SCAN          1008
#define MSG_BUTTON_PAIR          1009
#define MSG_BUTTON_CONNECT       1010
#define MSG_BUTTON_DISCONNECT    1011
#define MSG_BUTTON_FORGET        1012
#define MSG_HINT_SCAN            1013
#define MSG_HINT_PAIR            1014
#define MSG_HINT_CONNECT         1015
#define MSG_HINT_DISCONNECT      1016
#define MSG_HINT_FORGET          1017
#define MSG_KIND_MOUSE           1018
#define MSG_KIND_KEYBOARD        1019
#define MSG_KIND_GAMEPAD         1020
#define MSG_KIND_AUDIO           1021
#define MSG_KIND_SERIAL          1022
#define MSG_STATE_FOUND          1023
#define MSG_STATE_PAIRED         1024
#define MSG_STATE_CONNECTING     1025
#define MSG_STATE_CONNECTED      1026
#define MSG_STATE_IN_USE         1027
#define MSG_NO_SERVICE           1028
#define MSG_NO_CLASSES           1029
#define MSG_NO_WINDOW            1030
#define MSG_NO_NAME              1031
#define MSG_BUTTON_START_SERVICE 1032
#define MSG_BUTTON_STOP_SERVICE  1033
#define MSG_HINT_START_SERVICE   1034
#define MSG_HINT_STOP_SERVICE    1035
#define MSG_SERVICE_STOPPED      1036
#define MSG_VERSION_MISMATCH     1037
#define MSG_COMMAND_FAILED       1038
#define MSG_LOCAL_NAME           1039
#define MSG_BUTTON_SET_NAME      1040
#define MSG_HINT_LOCAL_NAME      1041
#define MSG_BUTTON_SCANNING      1042

#define MSG_WINDOW_TITLE_STR         "Bluetooth Devices"
#define MSG_NEARBY_STR               "Nearby"
#define MSG_KNOWN_STR                "Known devices"
#define MSG_CI_NAME_STR              "Name"
#define MSG_CI_ADDRESS_STR           "Address"
#define MSG_CI_SIGNAL_STR            "Signal"
#define MSG_CI_KIND_STR              "Kind"
#define MSG_CI_STATE_STR             "State"
#define MSG_BUTTON_SCAN_STR          "_Scan"
#define MSG_BUTTON_PAIR_STR          "_Pair"
#define MSG_BUTTON_CONNECT_STR       "_Connect"
#define MSG_BUTTON_DISCONNECT_STR    "_Disconnect"
#define MSG_BUTTON_FORGET_STR        "_Forget"
#define MSG_HINT_SCAN_STR            "Look for devices that are not known yet"
#define MSG_HINT_PAIR_STR            "Pair with the selected device and start using it"
#define MSG_HINT_CONNECT_STR         "Connect to the selected device"
#define MSG_HINT_DISCONNECT_STR      "Disconnect the selected device, keeping it paired"
#define MSG_HINT_FORGET_STR          "Forget the selected device, so it has to be paired again"
#define MSG_KIND_MOUSE_STR           "Mouse"
#define MSG_KIND_KEYBOARD_STR        "Keyboard"
#define MSG_KIND_GAMEPAD_STR         "Gamepad"
#define MSG_KIND_AUDIO_STR           "Audio"
#define MSG_KIND_SERIAL_STR          "Serial"
#define MSG_STATE_FOUND_STR          "Found"
#define MSG_STATE_PAIRED_STR         "Paired"
#define MSG_STATE_CONNECTING_STR     "Connecting"
#define MSG_STATE_CONNECTED_STR      "Connected"
#define MSG_STATE_IN_USE_STR         "In use"
#define MSG_NO_SERVICE_STR           "BluetoothService is not running - start it first."
#define MSG_NO_CLASSES_STR           "Cannot open the ReAction classes this needs."
#define MSG_NO_WINDOW_STR            "Cannot open the window."
#define MSG_NO_NAME_STR              "(no name)"
#define MSG_BUTTON_START_SERVICE_STR "Start _service"
#define MSG_BUTTON_STOP_SERVICE_STR  "Stop _service"
#define MSG_HINT_START_SERVICE_STR   "Start the Bluetooth service, so devices can connect"
#define MSG_HINT_STOP_SERVICE_STR    "Stop the Bluetooth service and disconnect everything"
#define MSG_SERVICE_STOPPED_STR      "The Bluetooth service is not running"
#define MSG_VERSION_MISMATCH_STR     "Bluetooth Devices - service is a different version, update it"
#define MSG_COMMAND_FAILED_STR       "Bluetooth Devices - the service refused that"
#define MSG_LOCAL_NAME_STR           "This computer is known as"
#define MSG_BUTTON_SET_NAME_STR      "Se_t"
#define MSG_HINT_LOCAL_NAME_STR      "The name other devices see, and can connect to"
#define MSG_BUTTON_SCANNING_STR      "Searching..."

struct CatCompArrayType {
    LONG   cca_ID;
    STRPTR cca_Str;
};

static const struct CatCompArrayType CatCompArray[] = {
    { MSG_WINDOW_TITLE,        (STRPTR) MSG_WINDOW_TITLE_STR },
    { MSG_NEARBY,              (STRPTR) MSG_NEARBY_STR },
    { MSG_KNOWN,               (STRPTR) MSG_KNOWN_STR },
    { MSG_CI_NAME,             (STRPTR) MSG_CI_NAME_STR },
    { MSG_CI_ADDRESS,          (STRPTR) MSG_CI_ADDRESS_STR },
    { MSG_CI_SIGNAL,           (STRPTR) MSG_CI_SIGNAL_STR },
    { MSG_CI_KIND,             (STRPTR) MSG_CI_KIND_STR },
    { MSG_CI_STATE,            (STRPTR) MSG_CI_STATE_STR },
    { MSG_BUTTON_SCAN,         (STRPTR) MSG_BUTTON_SCAN_STR },
    { MSG_BUTTON_PAIR,         (STRPTR) MSG_BUTTON_PAIR_STR },
    { MSG_BUTTON_CONNECT,      (STRPTR) MSG_BUTTON_CONNECT_STR },
    { MSG_BUTTON_DISCONNECT,   (STRPTR) MSG_BUTTON_DISCONNECT_STR },
    { MSG_BUTTON_FORGET,       (STRPTR) MSG_BUTTON_FORGET_STR },
    { MSG_HINT_SCAN,           (STRPTR) MSG_HINT_SCAN_STR },
    { MSG_HINT_PAIR,           (STRPTR) MSG_HINT_PAIR_STR },
    { MSG_HINT_CONNECT,        (STRPTR) MSG_HINT_CONNECT_STR },
    { MSG_HINT_DISCONNECT,     (STRPTR) MSG_HINT_DISCONNECT_STR },
    { MSG_HINT_FORGET,         (STRPTR) MSG_HINT_FORGET_STR },
    { MSG_KIND_MOUSE,          (STRPTR) MSG_KIND_MOUSE_STR },
    { MSG_KIND_KEYBOARD,       (STRPTR) MSG_KIND_KEYBOARD_STR },
    { MSG_KIND_GAMEPAD,        (STRPTR) MSG_KIND_GAMEPAD_STR },
    { MSG_KIND_AUDIO,          (STRPTR) MSG_KIND_AUDIO_STR },
    { MSG_KIND_SERIAL,         (STRPTR) MSG_KIND_SERIAL_STR },
    { MSG_STATE_FOUND,         (STRPTR) MSG_STATE_FOUND_STR },
    { MSG_STATE_PAIRED,        (STRPTR) MSG_STATE_PAIRED_STR },
    { MSG_STATE_CONNECTING,    (STRPTR) MSG_STATE_CONNECTING_STR },
    { MSG_STATE_CONNECTED,     (STRPTR) MSG_STATE_CONNECTED_STR },
    { MSG_STATE_IN_USE,        (STRPTR) MSG_STATE_IN_USE_STR },
    { MSG_NO_SERVICE,          (STRPTR) MSG_NO_SERVICE_STR },
    { MSG_NO_CLASSES,          (STRPTR) MSG_NO_CLASSES_STR },
    { MSG_NO_WINDOW,           (STRPTR) MSG_NO_WINDOW_STR },
    { MSG_NO_NAME,             (STRPTR) MSG_NO_NAME_STR },
    { MSG_BUTTON_START_SERVICE,(STRPTR) MSG_BUTTON_START_SERVICE_STR },
    { MSG_BUTTON_STOP_SERVICE, (STRPTR) MSG_BUTTON_STOP_SERVICE_STR },
    { MSG_HINT_START_SERVICE,  (STRPTR) MSG_HINT_START_SERVICE_STR },
    { MSG_HINT_STOP_SERVICE,   (STRPTR) MSG_HINT_STOP_SERVICE_STR },
    { MSG_SERVICE_STOPPED,     (STRPTR) MSG_SERVICE_STOPPED_STR },
    { MSG_VERSION_MISMATCH,    (STRPTR) MSG_VERSION_MISMATCH_STR },
    { MSG_COMMAND_FAILED,      (STRPTR) MSG_COMMAND_FAILED_STR },
    { MSG_LOCAL_NAME,          (STRPTR) MSG_LOCAL_NAME_STR },
    { MSG_BUTTON_SET_NAME,     (STRPTR) MSG_BUTTON_SET_NAME_STR },
    { MSG_HINT_LOCAL_NAME,     (STRPTR) MSG_HINT_LOCAL_NAME_STR },
    { MSG_BUTTON_SCANNING,     (STRPTR) MSG_BUTTON_SCANNING_STR },
};

#define CATCOMP_ARRAY_SIZE (sizeof(CatCompArray) / sizeof(CatCompArray[0]))

#endif /* BLUETOOTH_GUI_CAT_H */
