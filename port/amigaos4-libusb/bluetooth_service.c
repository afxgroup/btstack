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

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include <btstack_tlv.h>

#include "btstack_config.h"
#include "btstack.h"

#include "amigaos4_input.h"
#include "bt_handler_hid.h"
#include "bt_profile_handler.h"
#include "bt_service_port.h"
#include "btstack_run_loop_amigaos.h"

#define MAX_DEVICES 32

/* the known devices are kept in the TLV, so a restart - or a reboot - finds the
 * mouse again without the user pairing it a second time */
#define TLV_TAG_DEVICES ((((uint32_t)'B')<<24)|(((uint32_t)'T')<<16)|(((uint32_t)'D')<<8)|'V')

typedef struct {
    uint8_t bd_addr[6];
    uint8_t addr_type;
    uint8_t kind;
    char    name[32];
} bt_stored_device_t;

/* how long to wait for an outgoing connection before giving up */
#define CONNECTION_TIMEOUT_MS 10000

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
/* device we already asked gap_connect_cancel() for, used as a watchdog: see
 * connection_timeout_handler() */
static bt_device_t * connection_cancel_pending_for;

/* true while scanning on our own initiative: a device a handler claims is then
 * connected right away. A scan asked for by a client does not do that - the
 * user is choosing, and connecting behind their back would be rude. */
static bool autoconnect_on_find;

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
    if (injecting_input){
        DebugPrintF("%s", buffer);
    } else {
        printf("%s", buffer);
        fflush(stdout);
    }
}

/* require LE Secure Connections, refusing devices that only do legacy pairing */
static bool secure_connections_only;

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
    for (i = 0; i < MAX_DEVICES; i++){
        if (devices[i].in_use) continue;
        memset(&devices[i], 0, sizeof(bt_device_t));
        devices[i].in_use = true;
        devices[i].con_handle = HCI_CON_HANDLE_INVALID;
        memcpy(devices[i].info.bd_addr, addr, 6);
        devices[i].info.addr_type = addr_type;
        devices[i].info.state = BT_DEVICE_STATE_FOUND;
        return &devices[i];
    }
    /* table full: the oldest entries are not evicted on purpose - a device the
     * user is using must never be dropped to make room for one just seen */
    return NULL;
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

static void connection_retry_later(void){
    btstack_run_loop_set_timer_handler(&connection_timer, &connection_retry_timeout);
    btstack_run_loop_set_timer(&connection_timer, 500);
    btstack_run_loop_add_timer(&connection_timer);
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

    btstack_run_loop_set_timer_handler(&connection_timer, &connection_timeout_handler);
    btstack_run_loop_set_timer(&connection_timer, CONNECTION_TIMEOUT_MS);
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

    gap_connect(device->info.bd_addr, (bd_addr_type_t) device->info.addr_type);
    return BT_RESULT_OK;
}

/* hand the device to the handler that claimed it */
static void device_attach_handler(bt_device_t * device){

    if (device->handler == NULL) return;

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
static void handler_status(hci_con_handle_t con_handle, bool in_use, uint8_t status){
    bt_device_t * device = device_for_handle(con_handle);
    if (device == NULL) return;

    if (in_use){
        /* a Classic device connects through its handler, so this is where its
         * attempt concludes - the LE path has already cleared these */
        if (pending_device == device){
            pending_device = NULL;
            btstack_run_loop_remove_timer(&connection_timer);
        }
        device->con_handle  = con_handle;
        device->autoconnect = true;
        btstack_strcpy(device->info.handler, sizeof(device->info.handler), device->handler->name);
        device_set_state(device, BT_DEVICE_STATE_IN_USE);
        devices_store();
        service_log("service: %s in use by handler '%s'\n",
                    bd_addr_to_str(device->info.bd_addr), device->info.handler);
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
        device->handler = NULL;
        if (pending_device == device){
            pending_device = NULL;
            btstack_run_loop_remove_timer(&connection_timer);
            connection_retry_later();
        }
        gap_disconnect(con_handle);
    }
}

/* -------------------------------------------------------------------------- */
/* known devices                                                              */

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

static void inquiry_start(void){
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

static void scan_start(bool autoconnect){
    if (scanning) return;
    autoconnect_on_find = autoconnect;
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
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();
    scanning = true;
    bt_service_port_notify(BTEVENT_SCAN_STARTED, NULL, 0);
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
static void service_start_working(void){
    devices_load();
    scan_start(true);
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

            /* every advertisement, so "we never see it" can be told apart from
             * "we see it and reject it" without guessing */
            if (verbose){
                service_log("service: adv from %s, %u bytes of data, rssi %d\n",
                            bd_addr_to_str(addr), ad_len, rssi);
            }

            bool is_new = device_for_addr(addr) == NULL;
            device = device_add(addr, addr_type);
            if (device == NULL) break;

            device->info.rssi = rssi;

            /* remember which handler wants it, so connecting is a decision the
             * user makes and not a guess made later */
            const bt_profile_handler_t * handler = bt_profile_handler_probe(ad_data, ad_len);
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
                memcpy(device->info.name, ad_iterator_get_data(&context), name_len);
                device->info.name[name_len] = 0;
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

            /* connect what we can actually drive, when scanning by ourselves */
            if (autoconnect_on_find && (handler != NULL) && (pending_device == NULL)){
                service_log("service: connecting to %s\n", bd_addr_to_str(addr));
                device_connect(device);
            }
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
            device = device_for_addr(addr);
            bt_service_port_notify(BTEVENT_PAIRING_REQUEST, device ? &device->info : NULL,
                                   little_endian_read_32(packet, 8));
            service_log("service: pairing with %s, numeric value %"PRIu32"\n",
                        bd_addr_to_str(addr), little_endian_read_32(packet, 8));
            gap_ssp_confirmation_response(addr);
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

            /* known Classic device seen again, or a new one we can drive */
            if ((pending_device == NULL) && !shutdown_requested && (handler != NULL) &&
                (device->con_handle == HCI_CON_HANDLE_INVALID) &&
                (device->autoconnect || autoconnect_on_find)){
                service_log("service: connecting to %s (classic)\n", bd_addr_to_str(addr));
                device_connect(device);
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            inquiring = false;
            /* inquiry is bounded, so keep it going for as long as we are
             * looking - otherwise a Classic device switched on a minute later
             * would never be found */
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
            service_log("service: %s disconnected\n", bd_addr_to_str(device->info.bd_addr));

            /* a device we are meant to use reconnects when we see it again -
             * advertising on LE, inquiry on Classic, both of which scan_start()
             * turns on. Dialling it now would just time out if it went to
             * sleep, blocking everything else meanwhile */
            if (device->autoconnect && !shutdown_requested){
                scan_start(true);
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
                service_log("service: pairing failed, status 0x%02x reason 0x%02x\n",
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

        case BTCMD_SCAN_START:
            if (controller_ready == false) return BT_RESULT_NO_CONTROLLER;
            if (pending_device != NULL)     return BT_RESULT_BUSY;
            scan_start(false);
            return BT_RESULT_OK;

        case BTCMD_SCAN_STOP:
            inquiry_stop();
            if (scanning){
                gap_stop_scan();
                scanning = false;
                bt_service_port_notify(BTEVENT_SCAN_STOPPED, NULL, 0);
            }
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

        case BTCMD_UNPAIR:
            device = device_for_addr(msg->bsm_Addr);
            if (device == NULL) return BT_RESULT_UNKNOWN_DEVICE;
            device->autoconnect = false;
            if (device->con_handle != HCI_CON_HANDLE_INVALID){
                gap_disconnect(device->con_handle);
            }
            gap_delete_bonding((bd_addr_type_t) device->info.addr_type, device->info.bd_addr);
            device->info.state = BT_DEVICE_STATE_FOUND;
            devices_store();
            bt_service_port_notify(BTEVENT_DEVICE_UPDATED, &device->info, 0);
            return BT_RESULT_OK;

        case BTCMD_SUBSCRIBE_EVENTS:
            return bt_service_port_subscribe(msg->bsm_EventPort) ? BT_RESULT_OK : BT_RESULT_BUSY;

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
    }
    bt_handler_hid_set_verbose(verbose);

    /* startup banner - the console is still safe here, nothing is injected yet */
    printf("BluetoothService starting, port '%s'\n", BLUETOOTH_SERVICE_PORT_NAME);

    if (amigaos4_input_open() == false){
        printf("ERROR: cannot open input.device\n");
        return -1;
    }
    btstack_run_loop_set_data_source_handler(&input_data_source, &input_poll_ds);
    btstack_run_loop_enable_data_source_callbacks(&input_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&input_data_source);

    if (bt_service_port_open(&handle_command) == false){
        printf("ERROR: cannot create the service port - is a service already running?\n");
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
        printf("Accepting LE Legacy Pairing (start with --secure-only to require Secure Connections)\n");
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
    gap_discoverable_control(1);

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
