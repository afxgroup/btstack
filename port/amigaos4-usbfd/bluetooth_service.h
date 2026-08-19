/*
 * bluetooth_service.h - protocol of the AmigaOS 4 Bluetooth service
 *
 * Shared by everyone who talks to the service:
 *   - bt.usbfd, which starts it when a controller is plugged in
 *   - the future GUI, to scan, pair, connect and disconnect
 *   - any application that wants to use a Bluetooth device
 *
 * The service is a normal process owning a public MsgPort. Clients send it a
 * struct BTServiceMsg and get it replied; there is no shared state and no
 * library, so a client crash can never take the Bluetooth stack down with it.
 *
 * Why a process and not a library or a device:
 *   - the Bluetooth stack has to own a run loop and a single controller, which
 *     is a poor fit for a library called from arbitrary contexts
 *   - message passing keeps the GUI in its own process. That matters more than
 *     it looks: the service injects input events, and Intuition can be busy
 *     waiting for those, so the service must never call anything that needs
 *     Intuition - including printf() to a console window.
 */

#ifndef BLUETOOTH_SERVICE_H
#define BLUETOOTH_SERVICE_H

#include <exec/types.h>
#include <exec/ports.h>

#define BLUETOOTH_SERVICE_PORT_NAME "bluetooth.service"
#define BLUETOOTH_SERVICE_VERSION   2

/* how the service was asked to behave with a device */
typedef enum {
    BT_DEVICE_STATE_UNKNOWN = 0,
    BT_DEVICE_STATE_FOUND,        /* seen while scanning */
    BT_DEVICE_STATE_BONDED,       /* keys stored, not connected */
    BT_DEVICE_STATE_CONNECTING,
    BT_DEVICE_STATE_CONNECTED,    /* connected, no profile handler attached */
    BT_DEVICE_STATE_IN_USE,       /* connected and driven by a profile handler */
} bt_device_state_t;

/* what the service thinks a device is, used to pick the profile handler */
typedef enum {
    BT_DEVICE_KIND_UNKNOWN = 0,
    BT_DEVICE_KIND_MOUSE,
    BT_DEVICE_KIND_KEYBOARD,
    BT_DEVICE_KIND_GAMEPAD,
    BT_DEVICE_KIND_AUDIO,
    BT_DEVICE_KIND_SERIAL,
} bt_device_kind_t;

typedef struct {
    uint8            bd_addr[6];
    uint8            addr_type;        /* 0 = public, 1 = random, 0xff = classic */
    bt_device_state_t state;
    bt_device_kind_t  kind;
    int16            rssi;
    uint32           class_of_device;  /* classic only */
    char             name[32];
    char             handler[16];      /* profile handler in use, "" if none */
} BTDeviceInfo;

typedef enum {
    /* service */
    BTCMD_PING = 0,           /* is the service alive, and which version */
    BTCMD_SHUTDOWN,           /* stop the service (used by bt.usbfd on detach) */
    BTCMD_CONTROLLER_ATTACHED,/* a Bluetooth USB controller appeared */
    BTCMD_CONTROLLER_DETACHED,

    /* discovery */
    BTCMD_SCAN_START,
    BTCMD_SCAN_STOP,
    BTCMD_LIST_DEVICES,       /* fills the caller's BTDeviceInfo array */

    /* device */
    BTCMD_PAIR,
    BTCMD_UNPAIR,
    BTCMD_CONNECT,
    BTCMD_DISCONNECT,

    /* events: a client may ask to be notified instead of polling */
    BTCMD_SUBSCRIBE_EVENTS,
    BTCMD_UNSUBSCRIBE_EVENTS,
} bt_command_t;

typedef enum {
    BT_RESULT_OK = 0,
    BT_RESULT_FAILED,
    BT_RESULT_BUSY,
    BT_RESULT_NO_CONTROLLER,
    BT_RESULT_UNKNOWN_DEVICE,
    BT_RESULT_UNSUPPORTED,
    BT_RESULT_TOO_SMALL,      /* the caller's array could not hold the answer */
} bt_result_t;

/*
 * A command message. The client allocates it, sends it to the service port and
 * waits for the reply on its own port; the service never frees it.
 *
 * Buffers (bsm_Devices) belong to the client and are filled in place, so no
 * memory changes hands: the service has no way of leaking a client's memory,
 * and a client that dies while a message is outstanding is the only one at risk.
 */
typedef struct {
    struct Message   bsm_Message;
    uint32           bsm_Version;     /* BLUETOOTH_SERVICE_VERSION */
    bt_command_t     bsm_Command;
    bt_result_t      bsm_Result;

    /* device the command applies to (PAIR, CONNECT, ...) */
    uint8            bsm_Addr[6];
    uint8            bsm_AddrType;

    /* LIST_DEVICES: caller provides the array, service fills it and sets Count */
    BTDeviceInfo   * bsm_Devices;
    uint32           bsm_DevicesMax;
    uint32           bsm_DevicesCount;

    /* SUBSCRIBE_EVENTS: where to send BTServiceEvent messages */
    struct MsgPort * bsm_EventPort;
} BTServiceMsg;

/*
 * Sent by the service to subscribers when something changes, so a GUI can show
 * live state without polling.
 *
 * One way: the service allocates the message and the subscriber frees it with
 * FreeSysObject(ASOT_MESSAGE, ...). It is deliberately not replied, and carries
 * no reply port to reply to.
 *
 * These used to be replied to the service's own public port, which crashed any
 * subscriber that was holding one when the service stopped: the service frees
 * that port on the way out, so the reply went to memory that was gone, and
 * nothing the subscriber could check would have told it. Leaving the port
 * allocated would not have helped either - replying signals the task that owns
 * it, and by then that task has exited.
 *
 * The cost is that a subscriber which dies holding messages leaks them, which
 * is a few hundred bytes against a crash.
 */
typedef enum {
    BTEVENT_DEVICE_FOUND = 0,
    BTEVENT_DEVICE_UPDATED,
    BTEVENT_DEVICE_REMOVED,
    BTEVENT_SCAN_STARTED,
    BTEVENT_SCAN_STOPPED,
    BTEVENT_CONTROLLER_READY,
    BTEVENT_CONTROLLER_GONE,
    BTEVENT_PAIRING_REQUEST,  /* user confirmation needed, see bse_Passkey */
} bt_event_t;

typedef struct {
    struct Message   bse_Message;
    bt_event_t       bse_Event;
    BTDeviceInfo     bse_Device;
    uint32           bse_Passkey;
} BTServiceEvent;

#endif /* BLUETOOTH_SERVICE_H */
