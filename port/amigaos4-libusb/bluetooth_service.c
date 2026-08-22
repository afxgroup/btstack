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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include <btstack_tlv.h>

#include "btstack_config.h"
#include "btstack.h"

#include "amigaos4_input.h"
#include "bt_handler_hid.h"
#include "bt_handler_hid_classic.h"
#include "bt_usb_watch.h"
#include "bt_opp_server.h"
#include "bt_profile_handler.h"
#include "bt_service_port.h"
#include "btstack_run_loop_amigaos.h"

#define MAX_DEVICES 32

/* the known devices are kept in the TLV, so a restart - or a reboot - finds the
 * mouse again without the user pairing it a second time */
#define TLV_TAG_FOLDER  ((((uint32_t)'B')<<24)|(((uint32_t)'T')<<16)|(((uint32_t)'F')<<8)|((uint32_t)'D'))
#define TLV_TAG_NAME    ((((uint32_t)'B')<<24)|(((uint32_t)'T')<<16)|(((uint32_t)'N')<<8)|((uint32_t)'M'))
#define TLV_TAG_DEVICES ((((uint32_t)'B')<<24)|(((uint32_t)'T')<<16)|(((uint32_t)'D')<<8)|'V')

typedef struct {
    uint8_t bd_addr[6];
    uint8_t addr_type;
    uint8_t kind;
    char    name[32];
} bt_stored_device_t;

/* how long to wait for an outgoing connection before giving up */
#define CONNECTION_TIMEOUT_MS 10000

/*
 * Classic gets a little longer to establish itself, and far longer once a
 * person is actually involved.
 *
 * The distinction matters both ways: pairing a keyboard means reading six
 * digits, typing them and pressing Enter, which ten seconds does not cover -
 * but applying that patience from the start would let one keyboard that is
 * simply not in pairing mode block every other device for a whole minute,
 * since only one connection attempt runs at a time. So start short, and extend
 * the timer when a pairing event proves someone is at the keyboard.
 */
#define CONNECTION_TIMEOUT_CLASSIC_MS 15000

/*
 * How long the controller itself pages before giving up.
 *
 * BTstack defaults to 0x6000, about 15.36 s, which lands just after the 15 s
 * watchdog above - so the watchdog always won by a third of a second, and we
 * walked away from an attempt the controller was still busy with. It went on
 * paging a keyboard that was not there, and everything queued behind it: the
 * mouse advertised, we called gap_connect(), and it sat at "connecting" while
 * the radio was still occupied.
 *
 * Set it well below the watchdog and the ordering is the right way round. A
 * page that fails is then reported through the normal path, as a connection
 * opened with a non-zero status, which is handled properly and clears the
 * attempt; the watchdog goes back to being what it should be, a backstop for
 * the case where nothing is reported at all.
 *
 * 0x2000 is about 5.12 s, which is long enough to reach a device that is awake.
 */
#define PAGE_TIMEOUT_SLOTS 0x2000
#define CONNECTION_TIMEOUT_PAIRING_MS 60000

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
static btstack_timer_source_t retry_timer;
static btstack_timer_source_t page_timer;

/*
 * How often to page a bonded Classic device that is not connected.
 *
 * A flat twenty seconds was both too slow to be pleasant and too fast to be
 * free: every attempt at a device that is switched off costs a page timeout,
 * 5.12 s during which the radio is doing nothing else, and it made no
 * difference whether the keyboard had just been put down or had been in a
 * drawer since yesterday.
 *
 * So start quickly and give up gradually. The interval doubles after each
 * failure and is reset the moment anything happens that suggests the device is
 * back - it connects, or something disconnects, or a client asks to scan. In
 * practice that means a keyboard picked up again is reached in a few seconds,
 * while one that is genuinely away is paged twice a minute rather than three
 * times.
 *
 * The page timeout itself stays where it is. It looks like the obvious thing to
 * shorten, but a device page scans in repetition mode R1 or R2 - up to 1.28 s or
 * 2.56 s between scans - and paging for less than twice that risks missing a
 * device that is awake and listening, which is the one case that must not fail.
 */
#define CLASSIC_RECONNECT_MIN_MS  4000
#define CLASSIC_RECONNECT_MAX_MS 60000
static uint32_t classic_reconnect_ms = CLASSIC_RECONNECT_MIN_MS;

/* device we already asked gap_connect_cancel() for, used as a watchdog: see
 * connection_timeout_handler() */
static bt_device_t * connection_cancel_pending_for;

/* true while scanning on our own initiative: a device a handler claims is then
 * connected right away. A scan asked for by a client does not do that - the
 * user is choosing, and connecting behind their back would be rude. */

/*
 * Classic inquiry runs alongside the LE scan, because the two find completely
 * different devices: a Classic keyboard never advertises and is invisible to an
 * LE scan no matter how long it runs, which is exactly how one went missing for
 * a long time while twenty LE devices were being found around it.
 *
 * Unlike scanning, inquiry is not a state one simply leaves on: it runs for a
 * bounded number of 1.28 s periods and then reports GAP_EVENT_INQUIRY_COMPLETE,
 * so it is restarted from there for as long as we want to keep looking.
 */
#define INQUIRY_DURATION 4   /* 4 * 1.28 s ~ 5 s per round */
static bool inquiring;
static bool inquiry_wanted;
static bool le_reception_confirmed;
static bool inquiry_requested;   /* a client asked to look for new devices */

/*
 * How long an explicitly requested discovery runs for.
 *
 * Long enough for a device to be switched on and found, short enough that the
 * radio goes back to the devices already connected without anyone having to
 * remember to stop it.
 */
#define DISCOVERY_DURATION_MS 30000
static uint32_t discovery_until_ms;

/*
 * The name other devices see, and the one they offer to connect to.
 *
 * BTstack keeps the pointer rather than a copy, so this buffer has to outlive
 * every call - a local would be a dangling pointer the moment it went out of
 * scope, and the failure would be a garbled name rather than a crash, which is
 * worse to track down.
 */
#define LOCAL_NAME_MAX 32
static char local_name[LOCAL_NAME_MAX] = "AmigaOS";

static const btstack_tlv_t * tlv_impl;
static void *                tlv_context;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

/* polled every run loop wake-up: recycles the input.device requests */
static btstack_data_source_t input_data_source;

static bool verbose;

/* true as soon as a handler is driving a device, i.e. we may be injecting
 * input events - see service_log() */
static bool injecting_input;

/*
 * Is there a console to print to at all?
 *
 * Started from a Shell there is, and printing there is what the user wants.
 * Started by bt.usbfd there is not: the process is launched with its handles
 * on NIL:, so every line printed goes nowhere and the service becomes
 * impossible to diagnose - which is exactly the state it was in when it failed
 * to reconnect anything at boot and had no way of saying why. Serial is then
 * the only place a message can be seen, so that is where they all go.
 */
static bool have_console;

/*
 * Progress messages go to the console until a device is in use, and to the
 * serial debug output from then on.
 *
 * The reason for serial is real: once a handler is driving a device we may be
 * injecting input events, and Intuition can be blocked waiting for exactly
 * those while it drags or sizes a window. printf() goes through the console
 * handler, which needs Intuition, so printing there deadlocks the machine.
 * Before that point nothing is being injected and the console is safe - and
 * being able to watch the service come up without a serial cable is worth a
 * great deal when something does not work.
 *
 * The switch is one way on purpose: after a device has disconnected a
 * reconnection can happen at any moment, so going back to the console would
 * reopen the very window this avoids.
 */
static void service_log(const char * format, ...){
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (injecting_input || !have_console){
        DebugPrintF("%s", buffer);
    } else {
        printf("%s", buffer);
        fflush(stdout);
    }
}

/* require LE Secure Connections, refusing devices that only do legacy pairing */
static bool secure_connections_only;

/*
 * Start with no bonds at all.
 *
 * A device held in pairing mode is waiting to create a new bond, and will not
 * settle for being authenticated with the key from an old one - it accepts the
 * connection, never completes its own pairing, and drops the link when its
 * pairing window closes. Having a stale key for such a device is therefore
 * worse than having none, and there was no way to get rid of one short of
 * deleting the TLV file by hand and losing every other device with it.
 */
static bool forget_all;

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
    uint8_t slot = MAX_DEVICES;

    for (i = 0; i < MAX_DEVICES; i++){
        if (!devices[i].in_use){
            slot = i;
            break;
        }
    }

    /*
     * Full: take the slot of something we merely walked past.
     *
     * Every advertising device ends up in here, and a room with a couple of
     * dozen BLE beacons in it fills the table in seconds - after which nothing
     * new could be added at all. That is not an abstract worry: it is why a
     * keyboard that had connected perfectly well was then ignored by the
     * service, because registering it needed a slot and there was none.
     *
     * Anything the user has a relationship with stays: bonded devices, ones we
     * are connected to, and ones a handler wants. What gets dropped is a
     * passing beacon, which costs nothing - it will be re-added next time it
     * advertises.
     */
    if (slot == MAX_DEVICES){
        for (i = 0; i < MAX_DEVICES; i++){
            if (devices[i].autoconnect) continue;
            if (devices[i].con_handle != HCI_CON_HANDLE_INVALID) continue;
            if (devices[i].handler != NULL) continue;
            if (devices[i].info.state != BT_DEVICE_STATE_FOUND) continue;
            slot = i;
            break;
        }
    }

    if (slot == MAX_DEVICES){
        service_log("service: device table full, cannot add %s\n", bd_addr_to_str(addr));
        return NULL;
    }

    if (devices[slot].in_use){
        bt_service_port_notify(BTEVENT_DEVICE_REMOVED, &devices[slot].info, 0);
    }

    memset(&devices[slot], 0, sizeof(bt_device_t));
    devices[slot].in_use = true;
    devices[slot].con_handle = HCI_CON_HANDLE_INVALID;
    memcpy(devices[slot].info.bd_addr, addr, 6);
    devices[slot].info.addr_type = addr_type;
    devices[slot].info.state = BT_DEVICE_STATE_FOUND;
    return &devices[slot];
}

static void device_set_state(bt_device_t * device, bt_device_state_t state){
    if (device->info.state == state) return;
    device->info.state = state;
    bt_service_port_notify(BTEVENT_DEVICE_UPDATED, &device->info, 0);
}

/* -------------------------------------------------------------------------- */
/* connecting                                                                 */

static bt_result_t   device_connect(bt_device_t * device);
static void          scan_start(bool autoconnect);
static void          scan_resume_if_idle(void);
static void          devices_store(void);
static void          devices_forget_superseded(const bt_device_t * keep);
static bt_result_t   classic_connect_now(bt_device_t * device);
static bool          classic_connect_deferred;
static void          connection_timeout_handler(btstack_timer_source_t * ts);
static void          inquiry_start(void);
static void          inquiry_stop(void);
static bool          discovery_needed(void);

/*
 * After a failed attempt, go back to scanning. Known devices reconnect when
 * they are seen advertising, so this never dials an absent device again.
 * Called only from a fresh timer event - never from inside the call stack that
 * is unwinding a connection attempt - see connection_timeout_handler().
 */
static void connection_retry_timeout(btstack_timer_source_t * ts){
    UNUSED(ts);
    scan_start(true);
}

/*
 * A pairing event arrived for the device we are connecting to: someone is
 * dealing with it, so stop counting against the connection timeout.
 */
static void connection_extend_for_pairing(const bd_addr_t addr){
    if (pending_device == NULL) return;
    if (memcmp(pending_device->info.bd_addr, addr, 6) != 0) return;
    if (connection_cancel_pending_for != NULL) return;   /* already giving up */
    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer_handler(&connection_timer, &connection_timeout_handler);
    btstack_run_loop_set_timer(&connection_timer, CONNECTION_TIMEOUT_PAIRING_MS);
    btstack_run_loop_add_timer(&connection_timer);
}

/*
 * Retry on a timer of its own.
 *
 * This used to re-arm connection_timer, the very timer device_connect() uses to
 * watch over an attempt - so a failed Classic page armed it for 500 ms, and a
 * mouse advertising inside that window had device_connect() add a timer that
 * was still in the run loop's list. BTstack asserts on exactly that, and its
 * source says why: the list is singly linked, adding a member of it again links
 * it to itself. With asserts compiled out the list simply breaks, and from then
 * on no timer in the whole service fires again - which is why one failed page
 * at a keyboard that was switched off took the mouse down with it.
 *
 * Two timers, and both are removed before being armed, which is what BTstack
 * asks callers to do.
 */
static void connection_retry_later(void){
    btstack_run_loop_remove_timer(&retry_timer);
    btstack_run_loop_set_timer_handler(&retry_timer, &connection_retry_timeout);
    btstack_run_loop_set_timer(&retry_timer, 500);
    btstack_run_loop_add_timer(&retry_timer);
}

static void connection_timeout_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    if (pending_device == NULL) return;

    if (pending_device == connection_cancel_pending_for){
        /*
         * We already cancelled this very attempt and re-armed this same timer
         * as a watchdog - being back here means no GAP_SUBEVENT_LE_CONNECTION_
         * COMPLETE ever arrived for it. That should not happen (a cancel is
         * followed by a connection complete event one way or another), but
         * device_connect() refuses to start anything new while pending_device
         * is set, so waiting forever here would wedge every future reconnect
         * on one unresponsive attempt. Give up on it and move on instead.
         */
        service_log("service: no response cancelling connection to %s, giving up\n",
                    bd_addr_to_str(pending_device->info.bd_addr));
        device_set_state(pending_device, BT_DEVICE_STATE_BONDED);
        pending_device = NULL;
        connection_cancel_pending_for = NULL;
        connection_retry_later();
        return;
    }

    /*
     * Classic ends here and now.
     *
     * gap_connect_cancel() only knows about LE connections, and no
     * GAP_SUBEVENT_LE_CONNECTION_COMPLETE will ever arrive for a Classic one -
     * so the wait-for-the-outcome path below would sit through its watchdog and
     * only then give up, keeping pending_device set the whole time. That is why
     * a keyboard that failed to connect once was never tried again: every later
     * inquiry result found an attempt still "in progress".
     */
    if (pending_device->info.addr_type == 0xff){
        service_log("service: connection to %s timed out (classic)\n",
                    bd_addr_to_str(pending_device->info.bd_addr));
        if ((pending_device->handler != NULL) && (pending_device->handler->disconnect != NULL)){
            pending_device->handler->disconnect(pending_device->con_handle);
        }
        device_set_state(pending_device, BT_DEVICE_STATE_FOUND);
        pending_device = NULL;
        connection_cancel_pending_for = NULL;
        connection_retry_later();
        return;
    }

    service_log("service: connection to %s timed out, cancelling\n", bd_addr_to_str(pending_device->info.bd_addr));
    gap_connect_cancel();

    /*
     * pending_device is deliberately left set here, not cleared. This used to
     * clear it and immediately call gap_connect() again on the next device -
     * but gap_connect_cancel() does not guarantee the attempt is gone by the
     * time it returns: for a command already sent to the controller, it only
     * queues the actual "LE Create Connection Cancel" and the real outcome -
     * cancelled, or the connection completing anyway because it raced the
     * cancel - always arrives later as GAP_SUBEVENT_LE_CONNECTION_COMPLETE.
     * Calling gap_connect() again before that, for the very address whose
     * hci_connection_t is still sitting there mid-cancel, hits
     * ERROR_CODE_COMMAND_DISALLOWED inside gap_connect() - a return value
     * nothing here checked - so the retry silently did nothing while the
     * original attempt was the one still live. When that original attempt's
     * completion event then arrived, pending_device already pointed at a
     * *second*, never-actually-sent connect call for the same device, so the
     * event was accepted as if it belonged to that one: an unchecked status
     * (see below) meant a cancelled/failed attempt could be logged and treated
     * as "connected" while nothing was actually connected.
     *
     * Only the event handler below now decides what happens next; this
     * function's only remaining job is to ask for the cancellation and wait,
     * with the watchdog above as a bound on "wait" actually meaning something.
     */
    if (pending_device == NULL){
        /* the cancel resolved synchronously (device was not sent to the
         * controller yet) - the event handler below already dealt with it */
        connection_cancel_pending_for = NULL;
        return;
    }

    connection_cancel_pending_for = pending_device;
    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer_handler(&connection_timer, &connection_timeout_handler);
    btstack_run_loop_set_timer(&connection_timer, 2000);
    btstack_run_loop_add_timer(&connection_timer);
}

static bt_result_t device_connect(bt_device_t * device){

    if (controller_ready == false) return BT_RESULT_NO_CONTROLLER;
    if (pending_device != NULL)     return BT_RESULT_BUSY;
    if (device->info.state >= BT_DEVICE_STATE_CONNECTED) return BT_RESULT_OK;

    /*
     * Scanning is deliberately left running.
     *
     * BTstack already interleaves the two: hci_run_general_gap_le() pauses the
     * scan when it has to send LE Create Connection and its "restore state"
     * phase turns it back on afterwards, tracking what the application asked
     * for (le_scanning_enabled) apart from what the controller is doing
     * (le_scanning_active). Calling gap_stop_scan() here clears that intent, so
     * the scan stayed off after every connection until something happened to
     * start it again - which is why scanning appeared to stop by itself after a
     * while, and why a second device could not be found.
     */

    pending_device = device;
    device_set_state(device, BT_DEVICE_STATE_CONNECTING);

    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer_handler(&connection_timer, &connection_timeout_handler);
    btstack_run_loop_set_timer(&connection_timer, (device->info.addr_type == 0xff)
                                                  ? CONNECTION_TIMEOUT_CLASSIC_MS
                                                  : CONNECTION_TIMEOUT_MS);
    btstack_run_loop_add_timer(&connection_timer);

    /*
     * Classic goes a different way round. On LE we bring up the connection
     * ourselves and hand the finished thing to a handler; a Classic profile
     * opens its own L2CAP channels, so the handler is given the address and
     * does the connecting - there is no ACL connection to hand over yet. The
     * handler then reports back through handler_status() exactly as the LE one
     * does, so everything downstream stays the same.
     */
    if (device->info.addr_type == 0xff){
        if ((device->handler == NULL) || (device->handler->connect_addr == NULL)){
            device_set_state(device, BT_DEVICE_STATE_FOUND);
            pending_device = NULL;
            btstack_run_loop_remove_timer(&connection_timer);
            return BT_RESULT_UNSUPPORTED;
        }

        /*
         * Not while the controller is running an inquiry.
         *
         * Whether a controller accepts Create Connection in the middle of one
         * is up to the controller. An older Realtek dongle allowed it; a
         * Bluetooth 6.0 one refuses with COMMAND_DISALLOWED, and since a
         * Classic device is dialled the moment an inquiry finds it, every
         * attempt failed with status 0x0C - the same device over and over,
         * stuck in Connecting, and no pairing possible at all.
         *
         * BTstack has a flag for serialising these,
         * ENABLE_HCI_SERIALIZED_CONTROLLER_OPERATIONS, but it does not compile
         * in this version: it switches on a connection state,
         * SENT_CANCEL_CONNECTION, that the enum does not have. So the inquiry
         * is stopped here and the connection goes out when it reports itself
         * finished, which is the same ordering by our own hand.
         */
        if (inquiring){
            classic_connect_deferred = true;
            service_log("service: waiting for the inquiry to finish before dialling %s\n",
                        bd_addr_to_str(device->info.bd_addr));
            gap_inquiry_stop();
            return BT_RESULT_OK;
        }

        return classic_connect_now(device);
    }

    gap_connect(device->info.bd_addr, (bd_addr_type_t) device->info.addr_type);
    return BT_RESULT_OK;
}

/* the Classic half of device_connect(), once the controller is free to do it */
static bt_result_t classic_connect_now(bt_device_t * device){

    {
        uint8_t status = device->handler->connect_addr(device->info.bd_addr);
        if (status != ERROR_CODE_SUCCESS){
            service_log("service: classic connect to %s refused, status 0x%02x\n",
                        bd_addr_to_str(device->info.bd_addr), status);
            device_set_state(device, BT_DEVICE_STATE_BONDED);
            pending_device = NULL;
            btstack_run_loop_remove_timer(&connection_timer);
            connection_retry_later();
            return BT_RESULT_FAILED;
        }
        return BT_RESULT_OK;
    }
}

/* hand the device to the handler that claimed it */
static void device_attach_handler(bt_device_t * device){

    /*
     * A device connected by address has never been probed.
     *
     * The handler is normally decided from the advertisement, but Pair works on
     * an address - the device may not be in the table at all, and a client is
     * entitled to name one it remembers from a previous run. Such a device
     * arrived here with no handler and was dropped by a bare return: it paired,
     * became Paired, and then simply stopped, with nothing said anywhere.
     *
     * So try the handlers that can drive a connection. For LE that means asking
     * one to look, which is what it does anyway - it discovers the device's
     * services and reports back whether it found what it needs. Guessing here
     * costs a round trip and answers the question properly, where refusing
     * costs the whole point of the button.
     */
    if ((device->handler == NULL) && (device->info.addr_type != 0xff)){
        const bt_profile_handler_t ** handlers = bt_profile_handlers();
        uint8_t i;
        for (i = 0; handlers[i] != NULL; i++){
            if (handlers[i]->connect == NULL) continue;
            device->handler   = handlers[i];
            device->info.kind = handlers[i]->kind;
            service_log("service: %s was never probed, letting '%s' look\n",
                        bd_addr_to_str(device->info.bd_addr), handlers[i]->name);
            break;
        }
    }

    if (device->handler == NULL){
        service_log("service: nothing here can drive %s\n", bd_addr_to_str(device->info.bd_addr));
        return;
    }

    uint8_t status = device->handler->connect(device->con_handle);
    if (status != ERROR_CODE_SUCCESS){
        service_log("service: handler '%s' refused %s, status 0x%02x\n",
                    device->handler->name, bd_addr_to_str(device->info.bd_addr), status);
        device->handler = NULL;
        device->info.handler[0] = 0;
        return;
    }

    /* the handler reports back through handler_status() once it really has the
     * device - discovering the HID services takes a few round trips */
    service_log("service: handler '%s' taking %s\n",
                device->handler->name, bd_addr_to_str(device->info.bd_addr));
}

/* called by a handler when it starts or stops driving a device */
static void handler_status(const bt_profile_handler_t * handler, const bd_addr_t addr,
                           hci_con_handle_t con_handle, bool in_use, uint8_t status){
    /*
     * By address, not by handle. A Classic device that has been paired before
     * reconnects by connecting to us, so the first the service hears of it is
     * this call: there is no handle of ours to match, and the device may not be
     * in the table at all yet.
     */
    bt_device_t * device = device_for_addr(addr);
    if (device == NULL){
        device = device_add(addr, 0xff);
        if (device == NULL){
            service_log("service: no room for %s, it is running unmanaged\n",
                        bd_addr_to_str(addr));
            return;
        }
        service_log("service: %s connected to us\n", bd_addr_to_str(addr));
    }

    if (in_use){
        /* a Classic device connects through its handler, so this is where its
         * attempt concludes - the LE path has already cleared these */
        if (pending_device == device){
            pending_device = NULL;
            btstack_run_loop_remove_timer(&connection_timer);
        }
        device->con_handle  = con_handle;
        device->autoconnect = true;
        /* the handler reporting is the one driving it - a device that connected
         * to us was never probed, so there is nothing else to go on */
        device->handler   = handler;
        device->info.kind = handler->kind;
        btstack_strcpy(device->info.handler, sizeof(device->info.handler), device->handler->name);
        classic_reconnect_ms = CLASSIC_RECONNECT_MIN_MS;
        device_set_state(device, BT_DEVICE_STATE_IN_USE);
        devices_forget_superseded(device);
        devices_store();
        service_log("service: %s in use by handler '%s'\n",
                    bd_addr_to_str(device->info.bd_addr), device->info.handler);
        /* nothing left to find means nothing worth disturbing the radio for */
        if (!discovery_needed()){
            service_log("service: everything known is connected, discovery idle\n");
            inquiry_stop();
        }

        /* from here on a handler is driving a device, so console printing is no
         * longer safe - see service_log() */
        injecting_input = true;
        /* keep looking: a mouse being in use says nothing about the keyboard */
        scan_resume_if_idle();
        return;
    }

    device->info.handler[0] = 0;
    if (status != ERROR_CODE_SUCCESS){
        service_log("service: handler failed on %s, status 0x%02x\n",
                    bd_addr_to_str(device->info.bd_addr), status);
        /*
         * The handler is kept. It says what the device is - a keyboard is still
         * a keyboard when it is switched off - and clearing it here meant one
         * page timeout permanently unteaching us how to reach it: the paging
         * that reconnects bonded Classic devices needs a handler, so after the
         * first failure the device was skipped for ever.
         */
        /*
         * Back to where it was before the attempt.
         *
         * device_connect() sets CONNECTING and nothing here ever undid it, so a
         * device that failed to connect stayed Connecting for the rest of the
         * session - which is what a keyboard asleep in a drawer looked like in
         * the window, while the log showed page timeout after page timeout and
         * the state that was being shown had stopped meaning anything.
         */
        device_set_state(device, device->autoconnect ? BT_DEVICE_STATE_BONDED
                                                     : BT_DEVICE_STATE_FOUND);

        if (pending_device == device){
            pending_device = NULL;
            btstack_run_loop_remove_timer(&connection_timer);
            connection_retry_later();
        }
        if (con_handle != HCI_CON_HANDLE_INVALID){
            gap_disconnect(con_handle);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* known devices                                                              */

/*
 * Drop older entries for a device that has come back under a new address.
 *
 * This mouse gives itself a different address every time it is switched on -
 * 13:08:AC:00:00:83, then ...01:E3, then ...02:54 - so every boot added another
 * bonded ATL-MU55 to a list that never lost one. Address resolution is no help:
 * the top two bits of the first byte are 00, which makes these non-resolvable
 * private addresses, random by definition and not derivable from an IRK.
 *
 * The name is the only handle such a device offers, so a bonded device with the
 * same name and kind is taken to be the same device, and the older entry goes -
 * along with its bonding, which is dead weight in the controller's list once the
 * address it belongs to will never appear again.
 *
 * Only devices with a name are considered, and only against another bonded one:
 * two nameless devices are not evidence of anything.
 */
static void devices_forget_superseded(const bt_device_t * keep){
    if (keep->info.name[0] == 0) return;

    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        bt_device_t * device = &devices[i];
        if (device == keep)          continue;
        if (!device->in_use)         continue;
        if (!device->autoconnect)    continue;
        if (device->info.kind != keep->info.kind) continue;
        if (strcmp(device->info.name, keep->info.name) != 0) continue;
        if (device->con_handle != HCI_CON_HANDLE_INVALID) continue;

        service_log("service: %s superseded by %s, forgetting it\n",
                    bd_addr_to_str(device->info.bd_addr),
                    bd_addr_to_str(keep->info.bd_addr));

        if (device->info.addr_type == 0xff){
            gap_drop_link_key_for_bd_addr(device->info.bd_addr);
        } else {
            gap_delete_bonding((bd_addr_type_t) device->info.addr_type, device->info.bd_addr);
        }
        bt_service_port_notify(BTEVENT_DEVICE_REMOVED, &device->info, 0);
        memset(device, 0, sizeof(bt_device_t));
    }
}

static void devices_store(void){
    if (tlv_impl == NULL) return;

    bt_stored_device_t stored[MAX_DEVICES];
    uint8_t count = 0;
    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        if (!devices[i].in_use) continue;
        if (!devices[i].autoconnect) continue;   /* only what we should reconnect */
        memcpy(stored[count].bd_addr, devices[i].info.bd_addr, 6);
        stored[count].addr_type = devices[i].info.addr_type;
        stored[count].kind      = (uint8_t) devices[i].info.kind;
        btstack_strcpy(stored[count].name, sizeof(stored[count].name), devices[i].info.name);
        count++;
    }

    tlv_impl->store_tag(tlv_context, TLV_TAG_DEVICES, (const uint8_t *) stored,
                        count * sizeof(bt_stored_device_t));
    service_log("service: %u known device(s) stored\n", count);
}

static void name_load(void){
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl == NULL) return;

    char stored[LOCAL_NAME_MAX];
    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_NAME, (uint8_t *) stored, sizeof(stored));
    if ((len > 0) && (stored[0] != 0)){
        stored[sizeof(stored) - 1] = 0;
        btstack_strcpy(local_name, sizeof(local_name), stored);
    }
    gap_set_local_name(local_name);
    service_log("service: known to others as '%s'\n", local_name);

    char folder[256];
    len = tlv_impl->get_tag(tlv_context, TLV_TAG_FOLDER, (uint8_t *) folder, sizeof(folder));
    if ((len > 0) && (folder[0] != 0)){
        folder[sizeof(folder) - 1] = 0;
        bt_opp_server_set_folder(folder);
    }
    service_log("service: files sent to us go to %s\n", bt_opp_server_get_folder());
}

static void name_store(void){
    if (tlv_impl == NULL) return;
    tlv_impl->store_tag(tlv_context, TLV_TAG_NAME, (const uint8_t *) local_name, sizeof(local_name));
}

static void folder_store(void){
    if (tlv_impl == NULL) return;
    char folder[256];
    btstack_strcpy(folder, sizeof(folder), bt_opp_server_get_folder());
    tlv_impl->store_tag(tlv_context, TLV_TAG_FOLDER, (const uint8_t *) folder, sizeof(folder));
}

static uint8_t devices_load(void){
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl == NULL) return 0;

    bt_stored_device_t stored[MAX_DEVICES];
    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_DEVICES, (uint8_t *) stored, sizeof(stored));
    if (len <= 0) return 0;

    uint8_t count = (uint8_t) (len / sizeof(bt_stored_device_t));
    uint8_t i;
    for (i = 0; i < count; i++){
        bt_device_t * device = device_add(stored[i].bd_addr, stored[i].addr_type);
        if (device == NULL) break;
        device->autoconnect = true;
        device->info.state  = BT_DEVICE_STATE_BONDED;
        device->info.kind   = (bt_device_kind_t) stored[i].kind;
        /* a stored device was never probed, so give it back the handler its
         * kind implies - without one it can never be reconnected */
        if (stored[i].addr_type == 0xff){
            device->handler = bt_profile_handler_classic_for_kind(device->info.kind);
        }
        btstack_strcpy(device->info.name, sizeof(device->info.name), stored[i].name);
        service_log("service: known device %s '%s'\n",
                    bd_addr_to_str(device->info.bd_addr), device->info.name);
    }
    return count;
}

/*
 * Resume scanning after a connection attempt has concluded.
 *
 * device_connect() stops the scan while it runs, and nothing used to turn it
 * back on: with a mouse and a keyboard, whichever connected first left the
 * other one invisible for ever. Scanning is also how a known device that is
 * switched on later gets reconnected, so idle means scanning.
 */
static void scan_resume_if_idle(void){
    if (shutdown_requested)     return;
    if (pending_device != NULL) return;   /* another attempt is running */
    if (controller_ready == false) return;
    scan_start(true);
}

/*
 * Reconnect a bonded Classic device by paging it.
 *
 * Classic devices are only connected when an inquiry finds them, and a bonded
 * keyboard sitting idle is never found by one: once paired it stops answering
 * inquiry and only listens for a page. It is discoverable while the user holds
 * it in pairing mode, which is why connecting worked every time it was tested
 * by hand and never once at boot - by then the keyboard was simply awake and
 * bonded, which is exactly the state it should reconnect from.
 *
 * So page it rather than wait to be told it exists. This is not the blind
 * gap_connect() that had to be removed: a page is bounded by the controller's
 * page timeout and fails in a few seconds if the device is off, where an LE
 * connect would have waited for ever.
 *
 * Retried on a slow tick rather than every inquiry round, since each attempt at
 * a device that is off costs a page timeout during which nothing else happens.
 */

static void classic_reconnect_bonded(void);

static void page_timer_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    classic_reconnect_bonded();
}

static bool classic_bonded_exists(void){
    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        if (!devices[i].in_use)             continue;
        if (!devices[i].autoconnect)        continue;
        if (devices[i].info.addr_type != 0xff) continue;
        return true;
    }
    return false;
}

static void classic_reconnect_bonded(void){
    if (shutdown_requested) return;

    /* keep the tick going whatever happens this time round */
    btstack_run_loop_remove_timer(&page_timer);
    btstack_run_loop_set_timer_handler(&page_timer, &page_timer_handler);
    btstack_run_loop_set_timer(&page_timer, (uint32_t) classic_reconnect_ms);
    btstack_run_loop_add_timer(&page_timer);

    if (pending_device != NULL)    return;
    if (controller_ready == false) return;

    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        bt_device_t * device = &devices[i];
        if (!device->in_use)          continue;
        if (!device->autoconnect)     continue;
        if (device->info.addr_type != 0xff) continue;   /* Classic only */
        if (device->con_handle != HCI_CON_HANDLE_INVALID) continue;
        if (device->handler == NULL)  continue;

        service_log("service: paging known device %s (next in %lus)\n",
                    bd_addr_to_str(device->info.bd_addr),
                    (unsigned long) (classic_reconnect_ms / 1000));
        classic_reconnect_ms *= 2;
        if (classic_reconnect_ms > CLASSIC_RECONNECT_MAX_MS){
            classic_reconnect_ms = CLASSIC_RECONNECT_MAX_MS;
        }
        device_connect(device);
        return;   /* one at a time - device_connect() refuses the rest anyway */
    }
}

/*
 * Is there anything left to look for?
 *
 * Inquiry is the most disruptive thing this service does to the radio: five
 * seconds of hopping the inquiry sequence, restarted for ever, during which
 * established links get what is left. That is a fine price while a device we
 * want is still missing, and pure damage once they are all connected - it was
 * a good part of why a connected keyboard could not keep its link alive.
 */
static bool discovery_needed(void){
    uint8_t i;
    for (i = 0; i < MAX_DEVICES; i++){
        if (!devices[i].in_use)      continue;
        if (!devices[i].autoconnect) continue;
        if (devices[i].con_handle == HCI_CON_HANDLE_INVALID) return true;
    }
    return false;
}

static void inquiry_start(void){
    /*
     * Inquiry is for finding a device we do not have yet.
     *
     * It costs five seconds of the radio at a time and it repeats, so running
     * it permanently starved everything else: an LE mouse advertising the whole
     * while took minutes to be noticed, and a Classic link had no room to stay
     * alive. None of that bought anything once the keyboard was already bonded,
     * because a bonded Classic device is reached by paging it, or by it paging
     * us - never by discovering it again.
     *
     * So it runs while there is no Classic device bonded, which is how a fresh
     * install finds its first keyboard, and stops once there is one. A client
     * that wants to look for something new asks, and then it runs regardless.
     */
    if (!inquiry_requested && classic_bonded_exists()) return;

    inquiry_wanted = true;
    if (inquiring) return;
    if (controller_ready == false) return;
    if (gap_inquiry_start(INQUIRY_DURATION) == ERROR_CODE_SUCCESS){
        inquiring = true;
    }
}

static void inquiry_stop(void){
    inquiry_wanted = false;
    if (!inquiring) return;
    gap_inquiry_stop();
    /* inquiring is cleared by GAP_EVENT_INQUIRY_COMPLETE, which still arrives */
}

/*
 * autoconnect no longer decides anything, it only says so in the log: known
 * devices are reconnected whenever they are seen, and unknown ones are never
 * connected to without being asked for.
 */
static void scan_start(bool autoconnect){
    if (scanning) return;
    /*
     * Active scanning (1), not passive.
     *
     * Passive means never sending SCAN_REQ, so the scan response never arrives -
     * and that is where a lot of devices put their name and their service
     * UUIDs, keyboards especially. With passive scanning such a device is
     * either invisible or shows up nameless and unrecognised, while the same
     * hardware is found immediately by anything scanning actively, which is
     * what Linux does by default.
     */
    /*
     * Scan a quarter of the time, not all of it.
     *
     * This used to pass the same value for interval and window, which is a
     * hundred percent duty cycle: the radio scans continuously and every other
     * link has to fit in around it. Together with an inquiry running five
     * seconds out of every five, that left a Classic keyboard in sniff mode
     * with no room to complete an exchange, and its link died of supervision
     * timeout - twenty seconds without one successful packet - while looking
     * for all the world like the keyboard had gone away.
     *
     * 60 ms interval, 15 ms window. A device advertising at any normal rate is
     * still found within a second or so.
     */
    gap_set_scan_parameters(1, 96, 24);
    gap_start_scan();
    scanning = true;
    le_reception_confirmed = false;
    /*
     * Deliberately no BTEVENT_SCAN_STARTED here.
     *
     * This scan is permanent infrastructure - it is how a known device is
     * noticed coming back into range - and it starts before any client exists
     * to hear about it. Reporting it as a scan starting made the event mean two
     * different things, and a GUI could only ever catch the one it was not
     * present for. The event now means an explicit discovery, which is the only
     * kind anybody asked for and the only kind that ends.
     */
    service_log("service: scanning%s\n", autoconnect ? " (connecting to what we can drive)" : "");

    /* Classic devices are found by inquiry, never by this scan */
    inquiry_start();
}

/*
 * What to do once the controller is up: load what we know, then scan.
 *
 * Deliberately no direct connects here. A stored device that is switched off
 * or out of range would make gap_connect() run into its timeout, over and
 * over, and while an attempt is pending nothing else can connect - one absent
 * mouse would stall the keyboard forever. Scanning instead costs nothing when
 * nobody is there, and a known device showing up in the scan results is the
 * proof it is awake and in range: that is the moment to reconnect, handled in
 * the advertising report below.
 */
/*
 * The last Bluetooth controller has been unplugged.
 *
 * Not the same as "a controller was unplugged": two dongles are perfectly
 * possible and pulling one out is no reason to stop. Once there is none there
 * is nothing to run for, and staying up would leave a service holding devices
 * it can no longer reach.
 */
static void controller_all_gone(void){
    service_log("service: the last Bluetooth controller was unplugged, stopping\n");
    bt_service_port_notify(BTEVENT_CONTROLLER_GONE, NULL, 0);
    shutdown_requested = true;
    hci_power_control(HCI_POWER_OFF);
}

static void service_start_working(void){

    name_load();

    if (forget_all){
        /* the TLV instance is what devices_load() would fetch, and both the
         * link keys and our own list live in it */
        btstack_tlv_get_instance(&tlv_impl, &tlv_context);
        gap_delete_all_link_keys();
        if (tlv_impl != NULL){
            tlv_impl->delete_tag(tlv_context, TLV_TAG_DEVICES);
        }
        memset(devices, 0, sizeof(devices));
        service_log("service: all bonds forgotten, pair everything again\n");
    }

    devices_load();
    scan_start(true);
    classic_reconnect_bonded();   /* and it re-arms itself from here on */
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
                    service_log("service: controller ready\n");
                    service_start_working();
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

        /*
         * Both report events have to be handled. With ENABLE_LE_EXTENDED_ADVERTISING
         * and a controller that supports it, BTstack scans with the extended
         * commands: an advertisement marked legacy is converted to
         * GAP_EVENT_ADVERTISING_REPORT, everything else arrives as
         * GAP_EVENT_EXTENDED_ADVERTISING_REPORT. Handling only the first means
         * seeing nothing at all from a device that advertises the modern way.
         */
        case GAP_EVENT_ADVERTISING_REPORT:
        case GAP_EVENT_EXTENDED_ADVERTISING_REPORT: {
            const uint8_t * ad_data;
            uint8_t         ad_len;
            uint8_t         addr_type;
            int8_t          rssi;

            if (hci_event_packet_get_type(packet) == GAP_EVENT_ADVERTISING_REPORT){
                gap_event_advertising_report_get_address(packet, addr);
                addr_type = gap_event_advertising_report_get_address_type(packet);
                rssi      = (int8_t) gap_event_advertising_report_get_rssi(packet);
                ad_data   = gap_event_advertising_report_get_data(packet);
                ad_len    = gap_event_advertising_report_get_data_length(packet);
            } else {
                gap_event_extended_advertising_report_get_address(packet, addr);
                addr_type = gap_event_extended_advertising_report_get_address_type(packet);
                rssi      = gap_event_extended_advertising_report_get_rssi(packet);
                ad_data   = gap_event_extended_advertising_report_get_data(packet);
                ad_len    = gap_event_extended_advertising_report_get_data_length(packet);
            }

            if (!scanning) break;

            /*
             * Say once that LE reception works at all.
             *
             * Without it, a mouse missing from the list is indistinguishable
             * from an LE radio that is receiving nothing - and the two need
             * completely different things looking at. One line per scan answers
             * it, where -v answers it by burying it in hundreds.
             */
            if (!le_reception_confirmed){
                le_reception_confirmed = true;
                service_log("service: LE reception working, first advertisement from %s\n",
                            bd_addr_to_str(addr));
            }

            /* every advertisement, so "we never see it" can be told apart from
             * "we see it and reject it" without guessing */
            if (verbose){
                service_log("service: adv from %s, %u bytes of data, rssi %d\n",
                            bd_addr_to_str(addr), ad_len, rssi);
            }

            const bt_profile_handler_t * handler = bt_profile_handler_probe(ad_data, ad_len);

            /*
             * A slot is for a device we can do something with, not for whatever
             * happens to be transmitting.
             *
             * LE privacy addresses rotate every few minutes, so the supply of
             * addresses in range is effectively endless: storing each one filled
             * the table, and then every new advertisement evicted an older entry
             * and was itself evicted moments later. One boot produced six
             * thousand "found" lines that way, all of them beacons, none of them
             * anything we could drive.
             *
             * So a device earns a slot by being one a handler wants, or one we
             * already know. The rest are still announced to whoever is
             * listening on the port - a GUI wanting to show what is nearby gets
             * the sighting as it happens - they just do not take up residence.
             */
            device = device_for_addr(addr);
            bool is_new = (device == NULL);

            if (device == NULL){
                if (handler == NULL){
                    BTDeviceInfo seen;
                    memset(&seen, 0, sizeof(seen));
                    memcpy(seen.bd_addr, addr, 6);
                    seen.addr_type = addr_type;
                    seen.rssi      = rssi;
                    seen.state     = BT_DEVICE_STATE_FOUND;
                    bt_service_port_notify(BTEVENT_DEVICE_FOUND, &seen, 0);
                    break;
                }
                device = device_add(addr, addr_type);
                if (device == NULL) break;
            }

            device->info.rssi = rssi;

            /* remember which handler wants it, so connecting is a decision the
             * user makes and not a guess made later */
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
                /*
                 * Announce a name that has just been learned.
                 *
                 * A device is often seen before it says what it is called - the
                 * name is in the scan response, which arrives separately - and
                 * without this it stayed nameless in every list for as long as
                 * it was in range, since a device is only announced when it is
                 * new.
                 */
                if (memcmp(device->info.name, ad_iterator_get_data(&context), name_len) != 0){
                    memcpy(device->info.name, ad_iterator_get_data(&context), name_len);
                    device->info.name[name_len] = 0;
                    if (!is_new){
                        bt_service_port_notify(BTEVENT_DEVICE_UPDATED, &device->info, 0);
                    }
                }
                break;
            }

            if (is_new){
                service_log("service: found %s '%s' rssi %d%s\n", bd_addr_to_str(addr),
                            device->info.name, rssi, handler ? " (supported)" : "");
                bt_service_port_notify(BTEVENT_DEVICE_FOUND, &device->info, 0);
            }

            /* a known device advertising is awake and in range - reconnect now.
             * This replaces the blind gap_connect() at startup, which timed out
             * over and over on a device that was simply switched off. */
            if (device->autoconnect && (device->con_handle == HCI_CON_HANDLE_INVALID) &&
                (pending_device == NULL) && !shutdown_requested){
                service_log("service: known device %s is advertising, reconnecting\n", bd_addr_to_str(addr));
                device_connect(device);
                break;
            }

            /* an unknown device is connected to when asked for, see BTCMD_PAIR */
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            /*
             * Just Works pairing on Classic. Accepted automatically for the
             * same reason as on LE: there is no GUI to ask yet, and refusing
             * would make the hardware people own unusable. The passkey is
             * reported to subscribers so the GUI can show and confirm it once
             * it exists - that is where this decision belongs.
             */
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            /*
             * Make room for it if it is not known.
             *
             * A device that dialled us has never been in an inquiry result, so
             * it has no entry - and passing NULL here meant the pairing request
             * carried no address and no name, which is how a computer that
             * identifies itself perfectly clearly came out as "(no name)". An
             * entry also gives its name somewhere to go when the remote name
             * request answers.
             */
            device = device_for_addr(addr);
            if (device == NULL) device = device_add(addr, 0xff);
            if ((device != NULL) && (device->info.name[0] == 0)){
                gap_remote_name_request(addr, 0, 0);
            }
            bt_service_port_notify(BTEVENT_PAIRING_REQUEST, device ? &device->info : NULL,
                                   little_endian_read_32(packet, 8));
            service_log("service: pairing with %s, numeric value %"PRIu32"\n",
                        bd_addr_to_str(addr), little_endian_read_32(packet, 8));
            connection_extend_for_pairing(addr);
            gap_ssp_confirmation_response(addr);
            break;

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            /* the number the user has to type on the keyboard being paired */
            hci_event_user_passkey_notification_get_bd_addr(packet, addr);
            device = device_for_addr(addr);
            if (device == NULL) device = device_add(addr, 0xff);
            if ((device != NULL) && (device->info.name[0] == 0)){
                gap_remote_name_request(addr, 0, 0);
            }
            bt_service_port_notify(BTEVENT_PAIRING_REQUEST, device ? &device->info : NULL,
                                   hci_event_user_passkey_notification_get_numeric_value(packet));
            service_log("service: type %06"PRIu32" on %s and press Enter\n",
                        hci_event_user_passkey_notification_get_numeric_value(packet),
                        bd_addr_to_str(addr));
            connection_extend_for_pairing(addr);
            break;

        case HCI_EVENT_USER_PASSKEY_REQUEST:
            /* the other side wants *us* to type a passkey it is displaying. We
             * have nowhere to read one from, so refuse rather than leave the
             * device waiting for something that will never come. */
            hci_event_user_passkey_request_get_bd_addr(packet, addr);
            service_log("service: %s wants a passkey typed here, which we cannot do\n",
                        bd_addr_to_str(addr));
            gap_ssp_passkey_negative(addr);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            /* pre-2.1 legacy pairing, which needs a PIN we have no way to ask
             * for; refuse it rather than pretend */
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            service_log("service: %s wants a PIN code (legacy pairing), refusing\n",
                        bd_addr_to_str(addr));
            gap_pin_code_negative(addr);
            break;

        case GAP_EVENT_INQUIRY_RESULT: {
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);

            bool is_new = device_for_addr(addr) == NULL;
            /* 0xff marks a Classic address: it has no LE address type, and the
             * distinction matters when connecting and when storing it */
            device = device_add(addr, 0xff);
            if (device == NULL) break;

            device->info.class_of_device = cod;
            device->info.rssi = gap_event_inquiry_result_get_rssi(packet);

            if (gap_event_inquiry_result_get_name_available(packet)){
                uint8_t name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len >= sizeof(device->info.name)) name_len = sizeof(device->info.name) - 1;
                memcpy(device->info.name, gap_event_inquiry_result_get_name(packet), name_len);
                device->info.name[name_len] = 0;
            }

            const bt_profile_handler_t * handler = bt_profile_handler_probe_classic(cod);
            if (handler != NULL){
                device->handler   = handler;
                device->info.kind = handler->kind;
            }

            if (is_new){
                service_log("service: inquiry found %s '%s' COD 0x%06x rssi %d%s\n",
                            bd_addr_to_str(addr), device->info.name, (unsigned) cod,
                            device->info.rssi, handler ? " (supported)" : "");
                bt_service_port_notify(BTEVENT_DEVICE_FOUND, &device->info, 0);
            }

            /*
             * A known Classic device seen again. Only a known one.
             *
             * This used to dial anything it could drive, which meant every
             * keyboard within radio range and, worse, that forgetting a device
             * lasted until the next inquiry found it and adopted it back. A
             * device that is not known is connected to when the user asks, with
             * Pair, and not before.
             */
            if ((pending_device == NULL) && !shutdown_requested && (handler != NULL) &&
                (device->con_handle == HCI_CON_HANDLE_INVALID) &&
                device->autoconnect){
                service_log("service: connecting to %s (classic)\n", bd_addr_to_str(addr));
                device_connect(device);
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            inquiring = false;

            /* the controller is free now, so the connection that was waiting
             * for it can go out - and no new inquiry until it has */
            if (classic_connect_deferred){
                classic_connect_deferred = false;
                if (pending_device != NULL){
                    classic_connect_now(pending_device);
                }
                break;
            }
            /* inquiry is bounded, so keep it going for as long as we are
             * looking - otherwise a Classic device switched on a minute later
             * would never be found */
            if (inquiry_requested &&
                (btstack_time_delta(btstack_run_loop_get_time_ms(), discovery_until_ms) >= 0)){
                inquiry_requested = false;
                inquiry_wanted    = false;
                service_log("service: discovery finished\n");
                bt_service_port_notify(BTEVENT_SCAN_STOPPED, NULL, 0);
                break;
            }
            if (inquiry_wanted && !shutdown_requested){
                inquiry_start();
            }
            break;

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            btstack_run_loop_remove_timer(&connection_timer);
            connection_cancel_pending_for = NULL;
            if (pending_device == NULL) break;

            /*
             * This status was never checked before, which is what let a
             * cancelled or otherwise failed attempt - reported here with a
             * non-zero status and a connection handle that means nothing on
             * failure - be logged and treated as a real connection. That is
             * how the mouse could print "connected to ..." on the console and
             * never actually move: sm_request_pairing() was then called on a
             * connection that did not exist.
             */
            if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS){
                service_log("service: connection to %s failed, status 0x%02x\n",
                            bd_addr_to_str(pending_device->info.bd_addr),
                            gap_subevent_le_connection_complete_get_status(packet));
                device_set_state(pending_device, BT_DEVICE_STATE_BONDED);
                pending_device = NULL;
                connection_retry_later();
                break;
            }

            pending_device->con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            device_set_state(pending_device, BT_DEVICE_STATE_CONNECTED);
            service_log("service: connected to %s\n", bd_addr_to_str(pending_device->info.bd_addr));

            /* encrypt before using the device: a HID device will not deliver
             * reports otherwise, and the keys are what makes it reconnect */
            sm_request_pairing(pending_device->con_handle);
            break;

        /*
         * Whether the link is encrypted, and whether a bond exists.
         *
         * These are always logged, not just with -v: they are rare, and they
         * answer the one question the rest of the log cannot. A Classic
         * keyboard will connect, hand over its report descriptor and then send
         * not a single keystroke until the link is encrypted - which looks
         * exactly like a working connection that happens to be silent. Knowing
         * whether encryption ever happened is the difference between that and
         * a keyboard that simply considers some other host to be its active one.
         */
        case HCI_EVENT_ENCRYPTION_CHANGE:
            service_log("service: encryption %s on handle 0x%04x, status 0x%02x\n",
                        hci_event_encryption_change_get_encryption_enabled(packet) ? "on" : "off",
                        hci_event_encryption_change_get_connection_handle(packet),
                        hci_event_encryption_change_get_status(packet));
            break;

        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            service_log("service: authentication on handle 0x%04x, status 0x%02x\n",
                        hci_event_authentication_complete_get_connection_handle(packet),
                        hci_event_authentication_complete_get_status(packet));
            break;

        /*
         * A Classic device that connects to us arrives with no name.
         *
         * A name comes from an inquiry result, and a device that dialled us was
         * never in one - so a pairing request from a computer showed "(no name)"
         * however clearly that computer identifies itself. The name has to be
         * asked for, which is a request of its own.
         *
         * Asked once per device, when there is a name-shaped hole. It is only
         * for display, so failing to get one costs nothing.
         */
        case HCI_EVENT_CONNECTION_REQUEST:
            reverse_bd_addr(&packet[2], addr);
            device = device_for_addr(addr);
            if ((device == NULL) || (device->info.name[0] == 0)){
                gap_remote_name_request(addr, 0, 0);
            }
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            if (hci_event_remote_name_request_complete_get_status(packet) != ERROR_CODE_SUCCESS) break;
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            device = device_for_addr(addr);
            if (device == NULL) break;

            const char * name = hci_event_remote_name_request_complete_get_remote_name(packet);
            if ((name == NULL) || (name[0] == 0)) break;

            btstack_strcpy(device->info.name, sizeof(device->info.name), name);
            service_log("service: %s is called '%s'\n", bd_addr_to_str(addr), device->info.name);
            bt_service_port_notify(BTEVENT_DEVICE_UPDATED, &device->info, 0);
            break;
        }

        case HCI_EVENT_LINK_KEY_REQUEST:
            reverse_bd_addr(&packet[2], addr);
            service_log("service: %s asked for its link key\n", bd_addr_to_str(addr));
            break;

        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            reverse_bd_addr(&packet[2], addr);
            service_log("service: %s bonded, link key stored\n", bd_addr_to_str(addr));
            break;

        case HCI_EVENT_CONNECTION_COMPLETE:
            reverse_bd_addr(&packet[5], addr);
            service_log("service: classic link to %s, status 0x%02x\n",
                        bd_addr_to_str(addr), packet[2]);
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
            service_log("service: %s disconnected, reason 0x%02x\n",
                        bd_addr_to_str(device->info.bd_addr),
                        hci_event_disconnection_complete_get_reason(packet));

            /* a device we are meant to use reconnects when we see it again -
             * advertising on LE, inquiry on Classic, both of which scan_start()
             * turns on. Dialling it now would just time out if it went to
             * sleep, blocking everything else meanwhile */
            if (device->autoconnect && !shutdown_requested){
                /* something just went away, so it is probably nearby and about
                 * to come back - start looking eagerly again */
                classic_reconnect_ms = CLASSIC_RECONNECT_MIN_MS;
                scan_start(true);
                inquiry_start();   /* scan_start() only does this when it was idle */
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
                bt_device_t * failed = device_for_handle(con_handle);
                service_log("service: pairing with %s (addr type %u) failed, status 0x%02x reason 0x%02x\n",
                            failed ? bd_addr_to_str(failed->info.bd_addr) : "?",
                            failed ? failed->info.addr_type : 0xff,
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
        devices_store();
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

        case BTCMD_GET_NAME:
            btstack_strcpy(msg->bsm_Name, sizeof(msg->bsm_Name), local_name);
            return BT_RESULT_OK;

        case BTCMD_SET_NAME: {
            msg->bsm_Name[sizeof(msg->bsm_Name) - 1] = 0;
            if (msg->bsm_Name[0] == 0) return BT_RESULT_UNSUPPORTED;

            btstack_strcpy(local_name, sizeof(local_name), msg->bsm_Name);
            /*
             * Applied and stored, in that order: a name that the controller
             * refused is not one to remember, and gap_set_local_name() keeps
             * our buffer rather than copying it, which is why local_name is
             * static and is written before this is called.
             */
            gap_set_local_name(local_name);
            name_store();
            service_log("service: now known to others as '%s'\n", local_name);
            return BT_RESULT_OK;
        }

        case BTCMD_GET_FOLDER:
            btstack_strcpy(msg->bsm_Folder, sizeof(msg->bsm_Folder), bt_opp_server_get_folder());
            return BT_RESULT_OK;

        case BTCMD_SET_FOLDER:
            msg->bsm_Folder[sizeof(msg->bsm_Folder) - 1] = 0;
            if (msg->bsm_Folder[0] == 0) return BT_RESULT_UNSUPPORTED;
            bt_opp_server_set_folder(msg->bsm_Folder);
            folder_store();
            service_log("service: files sent to us now go to %s\n", bt_opp_server_get_folder());
            return BT_RESULT_OK;

        case BTCMD_SCAN_START:
            if (controller_ready == false) return BT_RESULT_NO_CONTROLLER;
            if (pending_device != NULL)     return BT_RESULT_BUSY;
            /*
             * Asked for explicitly, so look for Classic devices too even when
             * one is already bonded - that is how a second one gets added.
             *
             * inquiry_start() has to be called here rather than left to
             * scan_start(), which returns straight away when a scan is already
             * running and never reaches it. Since the service scans all the
             * time, that was always, and the button did nothing whatsoever.
             *
             * It runs for a bounded time. Inquiry is expensive enough that
             * leaving it on for ever is what the automatic one was changed to
             * stop doing, and a discovery that never ends is not what pressing
             * a button asks for.
             */
            inquiry_requested    = true;
            discovery_until_ms   = btstack_run_loop_get_time_ms() + DISCOVERY_DURATION_MS;
            classic_reconnect_ms = CLASSIC_RECONNECT_MIN_MS;
            scan_start(false);
            inquiry_start();
            bt_service_port_notify(BTEVENT_SCAN_STARTED, NULL, 0);
            return BT_RESULT_OK;

        case BTCMD_SCAN_STOP:
            /*
             * Ends the discovery, not the LE scan underneath it.
             *
             * That scan is how a known device is noticed coming back into
             * range, so stopping it would quietly break reconnection for as
             * long as nobody pressed the button again. What a client asks to
             * stop is the inquiry it asked to start.
             */
            inquiry_requested = false;
            inquiry_stop();
            bt_service_port_notify(BTEVENT_SCAN_STOPPED, NULL, 0);
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

        case BTCMD_UNPAIR: {
            device = device_for_addr(msg->bsm_Addr);
            if (device == NULL) return BT_RESULT_UNKNOWN_DEVICE;

            device->autoconnect = false;
            if (device->con_handle != HCI_CON_HANDLE_INVALID){
                gap_disconnect(device->con_handle);
            }

            /*
             * Classic keys are not in the LE database and gap_delete_bonding()
             * cannot reach them: it takes an address type, and 0xff - which is
             * how a Classic address is marked here - is not one. So a forgotten
             * keyboard kept its link key and carried on authenticating with it.
             */
            if (device->info.addr_type == 0xff){
                gap_drop_link_key_for_bd_addr(device->info.bd_addr);
            } else {
                gap_delete_bonding((bd_addr_type_t) device->info.addr_type, device->info.bd_addr);
            }

            /*
             * The entry goes entirely, rather than being left behind as a
             * sighting. Left in the table it was picked up again within
             * seconds, connected to, and stored afresh - so Forget appeared to
             * work and then undid itself, which is exactly what was reported.
             */
            BTDeviceInfo forgotten = device->info;
            forgotten.state = BT_DEVICE_STATE_UNKNOWN;
            service_log("service: forgot %s\n", bd_addr_to_str(device->info.bd_addr));
            memset(device, 0, sizeof(bt_device_t));

            devices_store();
            bt_service_port_notify(BTEVENT_DEVICE_REMOVED, &forgotten, 0);
            return BT_RESULT_OK;
        }

        case BTCMD_SUBSCRIBE_EVENTS:
            if (!bt_service_port_subscribe(msg->bsm_EventPort)) return BT_RESULT_BUSY;
            /*
             * Tell it what is true now, not just what changes next.
             *
             * A subscriber only ever hears about changes, so one that starts
             * while a discovery is running had no way to know - it showed the
             * button as idle until the discovery ended, which is the one moment
             * it would be told, and by then it was right for the wrong reason.
             */
            bt_service_port_notify(inquiry_requested ? BTEVENT_SCAN_STARTED
                                                     : BTEVENT_SCAN_STOPPED, NULL, 0);
            return BT_RESULT_OK;

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
        if (strcmp(argv[i], "--secure-only") == 0){
            secure_connections_only = true;
        }
        if (strcmp(argv[i], "--forget-all") == 0){
            forget_all = true;
        }
    }
    bt_handler_hid_set_verbose(verbose);
    bt_handler_hid_classic_set_verbose(verbose);

    have_console = (IsInteractive(Output()) == DOSTRUE);

    /* startup banner - the console is still safe here, nothing is injected yet */
    /*
     * Say which build this is, every time.
     *
     * Twice now a session has been spent on behaviour that had already been
     * fixed, because the binary being run was not the one that was built - once
     * a protocol version behind, once a file that could not be overwritten
     * because the old process still held it. Neither was diagnosable from the
     * log, which looked exactly like a bug that would not go away. A build
     * stamp costs one line and settles it before anything else is looked at.
     */
    service_log("BluetoothService starting, port '%s' (protocol %u, built %s %s)\n",
                BLUETOOTH_SERVICE_PORT_NAME, (unsigned) BLUETOOTH_SERVICE_VERSION,
                __DATE__, __TIME__);

    if (amigaos4_input_open() == false){
        service_log("ERROR: cannot open input.device\n");
        return -1;
    }
    btstack_run_loop_set_data_source_handler(&input_data_source, &input_poll_ds);
    btstack_run_loop_enable_data_source_callbacks(&input_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&input_data_source);

    /*
     * Watching is a nicety, not a requirement: a USB stack with no
     * notifications means the service keeps running when the dongle goes,
     * which is what it did before and is no worse than before.
     */
    bt_usb_watch_open(&controller_all_gone);

    if (bt_service_port_open(&handle_command) == false){
        service_log("ERROR: cannot create the service port - is a service already running?\n");
        return -1;
    }

    l2cap_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    /*
     * Accept LE Legacy Pairing.
     *
     * sm_init() turns on LE Secure Connections *Only* mode whenever
     * ENABLE_LE_SECURE_CONNECTIONS is configured, and then every device that
     * does not do Secure Connections - which is most mice, and every older
     * peripheral - is refused with SM_REASON_AUTHENTHICATION_REQUIREMENTS.
     *
     * TODO: this is a policy decision that belongs to the user, and once the
     * GUI exists it should be one: refuse by default and let them accept a
     * legacy device knowingly, per device, rather than lowering the bar for
     * everything. Until there is a way to ask, refusing would simply make the
     * hardware people own unusable. Start with --secure-only to try the strict
     * behaviour.
     */
    if (secure_connections_only == false){
        sm_set_secure_connections_only_mode(false);
        service_log("Accepting LE Legacy Pairing (start with --secure-only to require Secure Connections)\n");
    }

    gatt_client_init();

    /*
     * Classic setup. A HID keyboard expects to be able to go into sniff mode
     * and to ask for a role switch, and we want to end up master so the device
     * follows our timing. Being discoverable lets a keyboard that was paired
     * before start the connection itself, which is how they reconnect after
     * being switched on.
     */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    /*
     * Be reachable, not just findable.
     *
     * BTstack builds the scan enable value as (connectable << 1) | discoverable
     * - bit 1 is page scan, bit 0 is inquiry scan - and connectable defaults to
     * off. Asking only for discoverable therefore left page scan disabled, so
     * nothing could ever connect to us; we could find devices and dial them,
     * and that is all.
     *
     * That is fatal for a keyboard. A bonded HID keyboard does not sit waiting
     * to be dialled: it sleeps, and when a key is pressed it pages its host to
     * reconnect. With page scan off those pages went nowhere. It could only be
     * reached by us calling it, which works only while it is in pairing mode
     * and answering inquiry - so it ran perfectly for as long as the pairing
     * light blinked and stopped when that expired, every single time, while an
     * LE mouse on a completely different path was unaffected.
     *
     * It also means HID_SUBEVENT_INCOMING_CONNECTION could never arrive, and
     * the code handling it had never once run.
     */
    /*
     * Say what this actually is.
     *
     * BTstack defaults the class of device to 0x007a020c, a smartphone, with
     * the service bits for telephony, networking and object transfer that go
     * with one - none of which is offered here. Another machine showing a list
     * of what is nearby groups and filters by exactly this, so an Amiga
     * claiming to be a phone that answers none of a phone's services is either
     * confusing or invisible.
     *
     * 0x000104 is major class Computer, minor class Desktop workstation, with
     * no service bits claimed. Being a HID host needs none of them.
     */
    gap_set_class_of_device(0x000104);

    /*
     * Accept files pushed to us. Registered here rather than on demand: the SDP
     * record has to be in place before anything asks what we can do, and a
     * sender asks once and gives up.
     */
    bt_opp_server_init(local_name);

    gap_connectable_control(1);
    gap_discoverable_control(1);

    /*
     * Say we have a display, which for pairing purposes we do - the console.
     *
     * BTstack defaults to NO_INPUT_NO_OUTPUT, and against a keyboard (whose own
     * capability is KeyboardOnly) that picks Just Works. A keyboard is an input
     * device by definition and generally insists on Passkey Entry instead,
     * which is why pairing got as far as a confirmation request and then simply
     * stopped. DISPLAY_ONLY gives the expected model: we show a number, the
     * user types it on the keyboard and presses Enter.
     */
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    gap_set_page_timeout(PAGE_TIMEOUT_SLOTS);

    /*
     * Let the controller agree to sniff mode.
     *
     * BTstack leaves the default link policy at zero, which disallows all of
     * it, so the controller refuses every sniff request a device makes. A
     * keyboard asks: it runs on batteries and cannot stay fully awake between
     * keystrokes. Refused, it does not helpfully stay awake anyway - it goes
     * quiet, and about a minute later the link dies of supervision timeout,
     * reason 0x08, with neither side having hung up. That is precisely what a
     * keyboard that worked while its pairing light blinked and stopped when it
     * settled down was telling us.
     *
     * Role switch is allowed for the same reason it usually is: a peripheral
     * that wants to be master of its own link should be able to ask.
     */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);

    /* no ATT server: we are a central, and running one opens a re-entrancy in
     * att_server that recurses until the stack overflows. See bthid.c. */

    bt_profile_handler_set_status_callback(&handler_status);

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
