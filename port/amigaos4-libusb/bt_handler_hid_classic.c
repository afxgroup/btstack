/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bt_handler_hid_classic.c"

/*
 * Bluetooth Classic HID profile handler: keyboards and mice that speak HID over
 * L2CAP rather than over GATT.
 *
 * This is not an alternative to bt_handler_hid.c, it is the other half. Plenty
 * of hardware is Classic only - multi channel keyboards especially - and such a
 * device never appears in an LE scan at all, no matter how long one waits: it
 * has to be found by inquiry instead. Everything downstream is shared, because
 * a report is a report: bt_hid_report.c decodes it and injects it, and neither
 * it nor the keymap knows which transport carried it here.
 *
 * Nothing here writes to the console - see amigaos4_input.c.
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include "btstack.h"

#include "bt_handler_hid_classic.h"
#include "bt_hid_report.h"

static uint8_t hid_descriptor_storage[500];

static uint16_t         hid_cid;
static hci_con_handle_t hid_con_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t        hid_addr;
static bool             verbose;
static uint16_t         sniff_max_latency;
static uint16_t         sniff_min_timeout;

/* -------------------------------------------------------------------------- */

static void hid_classic_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) return;

    switch (hci_event_hid_meta_get_subevent_code(packet)){

        case HID_SUBEVENT_INCOMING_CONNECTION: {
            /*
             * The keyboard is connecting to us, which is how a Classic device
             * that has been paired before comes back: it is the one that
             * initiates, we are merely discoverable.
             *
             * This has to be answered. hid_host has already created a
             * connection object for it, and an unanswered one is not harmless -
             * every outgoing hid_host_connect() to that address is refused with
             * ERROR_CODE_COMMAND_DISALLOWED because a connection for it already
             * exists. Ignoring the event therefore did not just miss the
             * incoming connection, it blocked the outgoing one as well.
             */
            uint16_t incoming_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            if (hid_subevent_incoming_connection_get_status(packet) != ERROR_CODE_SUCCESS){
                DebugPrintF("hid classic: incoming connection refused by the stack\n");
                hid_host_decline_connection(incoming_cid);
                break;
            }
            if (hid_con_handle != HCI_CON_HANDLE_INVALID){
                /* already driving one; a second would fight it for the pointer */
                DebugPrintF("hid classic: declining a second incoming connection\n");
                hid_host_decline_connection(incoming_cid);
                break;
            }
            DebugPrintF("hid classic: incoming connection, accepting\n");
            hid_cid = incoming_cid;
            hid_host_accept_connection(incoming_cid, HID_PROTOCOL_MODE_REPORT);
            break;
        }

        case HID_SUBEVENT_SNIFF_SUBRATING_PARAMS:
            /*
             * What the device asks for to be allowed to doze between reports.
             *
             * These come out of its SDP record and arrive before the connection
             * is reported open, so they are kept until there is a handle to
             * apply them to. A keyboard that is refused this does not stay
             * awake to keep us happy - it goes quiet anyway, and the link dies
             * of supervision timeout a minute later.
             */
            sniff_max_latency = hid_subevent_sniff_subrating_params_get_host_max_latency(packet);
            sniff_min_timeout = hid_subevent_sniff_subrating_params_get_host_min_timeout(packet);
            DebugPrintF("hid classic: sniff subrating, max latency %u, min timeout %u\n",
                        sniff_max_latency, sniff_min_timeout);
            break;

        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, hid_addr);
            if (status != ERROR_CODE_SUCCESS){
                DebugPrintF("hid classic: connection failed, status 0x%02x\n", status);
                hid_con_handle = HCI_CON_HANDLE_INVALID;
                bt_profile_handler_report_status(&bt_handler_hid_classic, hid_addr, HCI_CON_HANDLE_INVALID, false, status);
                break;
            }
            hid_cid        = hid_subevent_connection_opened_get_hid_cid(packet);
            hid_con_handle = hid_subevent_connection_opened_get_con_handle(packet);
            DebugPrintF("hid classic: connected to %s, cid 0x%04x\n",
                        bd_addr_to_str(hid_addr), hid_cid);
            /* now there is a handle to apply the device's own sniff request to */
            if (sniff_max_latency != 0){
                uint8_t sniff_status = gap_sniff_subrating_configure(hid_con_handle,
                                                                    sniff_max_latency,
                                                                    sniff_min_timeout,
                                                                    sniff_min_timeout);
                DebugPrintF("hid classic: sniff subrating configured, status 0x%02x\n", sniff_status);
            }

            bt_profile_handler_report_status(&bt_handler_hid_classic, hid_addr, hid_con_handle, true, ERROR_CODE_SUCCESS);
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            /*
             * Reports can start arriving before this, and are decoded with the
             * boot protocol descriptor until it does - which is why
             * bt_hid_report_process() falls back to it rather than refusing.
             */
            DebugPrintF("hid classic: report descriptor available, status 0x%02x, %u bytes\n",
                        hid_subevent_descriptor_available_get_status(packet),
                        hid_descriptor_storage_get_descriptor_len(hid_cid));

            /*
             * Print the descriptor itself.
             *
             * The reports coming in fit two different layouts equally well -
             * modifiers in the second byte, which is what a standard keyboard
             * report does, or in the third with something else in the second -
             * and the keys decode correctly either way, so the reports alone
             * cannot say which. The descriptor can: it is the thing that
             * defines where each field sits. It is printed once per connection
             * and is a hundred and twenty bytes, which is worth it to stop
             * guessing between two readings that both fit.
             */
            {
                const uint8_t * d   = hid_descriptor_storage_get_descriptor_data(hid_cid);
                uint16_t        len = hid_descriptor_storage_get_descriptor_len(hid_cid);
                uint16_t        i;
                char            line[3 * 16 + 1];

                for (i = 0; (d != NULL) && (i < len); i++){
                    uint16_t col = i & 0x0f;
                    line[col * 3 + 0] = "0123456789abcdef"[d[i] >> 4];
                    line[col * 3 + 1] = "0123456789abcdef"[d[i] & 0x0f];
                    line[col * 3 + 2] = ' ';
                    if ((col == 15) || (i + 1 == len)){
                        line[col * 3 + 3] = 0;
                        DebugPrintF("hid classic: desc %s\n", line);
                    }
                }
            }
            break;

        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            /*
             * Which report layout the device is actually using.
             *
             * An incoming connection is accepted in report mode, so the host
             * sends SET_PROTOCOL and the device answers here. It matters
             * because the two layouts differ: a boot keyboard report is eight
             * bytes with no report ID, a report mode one starts with the ID. If
             * the device ends up in one and we decode with the descriptor for
             * the other, every field is off and what comes out is not the key
             * that was pressed.
             */
            DebugPrintF("hid classic: set protocol response, handshake 0x%02x\n",
                        hid_subevent_set_protocol_response_get_handshake_status(packet));
            break;

        case HID_SUBEVENT_REPORT: {
            const uint8_t * report     = hid_subevent_report_get_report(packet);
            uint16_t        report_len = hid_subevent_report_get_report_len(packet);

            /*
             * Drop the HID transaction header before decoding.
             *
             * What arrives here is the whole L2CAP payload from the interrupt
             * channel, which over Classic begins with a one byte transaction
             * header - 0xa1, DATA/Input - and only then the report itself. The
             * report parser expects the report, so leaving that byte on shifts
             * every field by one and the keyboard decodes as gibberish.
             *
             * There is no such byte over LE, where reports arrive as GATT
             * notifications, which is why this belongs here and not in the
             * shared decoder. Anything that is not an input report is not ours
             * to interpret.
             */
            if (report_len < 1) break;
            if (report[0] != 0xa1) break;
            report++;
            report_len--;

            if (verbose){
                char hex[3 * 16 + 1];
                uint16_t n = (report_len < 16) ? report_len : 16;
                uint16_t k;
                for (k = 0; k < n; k++){
                    hex[k * 3 + 0] = "0123456789abcdef"[report[k] >> 4];
                    hex[k * 3 + 1] = "0123456789abcdef"[report[k] & 0x0f];
                    hex[k * 3 + 2] = ' ';
                }
                hex[n * 3] = 0;
                DebugPrintF("hid classic: report %s(%u bytes, descriptor %u bytes)\n",
                            hex, report_len,
                            hid_descriptor_storage_get_descriptor_len(hid_cid));
            }

            bt_hid_report_process(hid_descriptor_storage_get_descriptor_data(hid_cid),
                                  hid_descriptor_storage_get_descriptor_len(hid_cid),
                                  report, report_len);
            break;
        }

        case HID_SUBEVENT_CONNECTION_CLOSED: {
            DebugPrintF("hid classic: disconnected\n");
            /* never leave a key or a button down for the whole system */
            bt_hid_report_release_all();
            hci_con_handle_t gone = hid_con_handle;
            hid_con_handle = HCI_CON_HANDLE_INVALID;
            hid_cid = 0;
            bt_profile_handler_report_status(&bt_handler_hid_classic, hid_addr, gone, false, ERROR_CODE_SUCCESS);
            break;
        }

        default:
            /*
             * Anything else the HID host has to say. It is normally nothing,
             * so printing it costs no noise - and if a device refuses report
             * protocol, or suspends, this is where that shows up instead of
             * disappearing into a silent default case.
             */
            DebugPrintF("hid classic: subevent 0x%02x\n",
                        hci_event_hid_meta_get_subevent_code(packet));
            break;
    }
}

/* -------------------------------------------------------------------------- */

void bt_handler_hid_classic_set_verbose(bool enabled){
    verbose = enabled;
}

static void hid_classic_init(void){
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(&hid_classic_packet_handler);
}

/*
 * Classic devices do not advertise, so there is nothing to probe: the service
 * calls probe_classic() with the Class of Device from the inquiry result
 * instead. This one exists only to say "not mine" to LE advertisements.
 */
static bool hid_classic_probe(const uint8_t * ad_data, uint8_t ad_len){
    UNUSED(ad_data);
    UNUSED(ad_len);
    return false;
}

/*
 * Class of Device: bits 8..12 are the major device class, 0x05 being
 * Peripheral, and within it bit 6 marks a keyboard and bit 7 a pointing
 * device. A combo device sets both.
 */
static bool hid_classic_probe_classic(uint32_t class_of_device){
    if (((class_of_device >> 8) & 0x1F) != 0x05) return false;
    return (class_of_device & 0xC0) != 0;
}

static uint8_t hid_classic_connect_addr(const bd_addr_t addr){
    /* one device at a time: input.device has a single pointer and a single
     * keyboard focus, so a second one would only fight the first */
    if (hid_con_handle != HCI_CON_HANDLE_INVALID){
        return ERROR_CODE_COMMAND_DISALLOWED;
    }
    return hid_host_connect((uint8_t *) addr, HID_PROTOCOL_MODE_REPORT, &hid_cid);
}

static uint8_t hid_classic_connect(hci_con_handle_t con_handle){
    UNUSED(con_handle);
    /* Classic connections are started from the address, see connect_addr:
     * there is no ACL connection yet at this point */
    return ERROR_CODE_COMMAND_DISALLOWED;
}

static void hid_classic_disconnect(hci_con_handle_t con_handle){
    if (con_handle != hid_con_handle) return;
    bt_hid_report_release_all();
    if (hid_cid != 0){
        hid_host_disconnect(hid_cid);
    }
    hid_con_handle = HCI_CON_HANDLE_INVALID;
    hid_cid = 0;
}

const bt_profile_handler_t bt_handler_hid_classic = {
    "hid-classic",
    BT_DEVICE_KIND_KEYBOARD,
    &hid_classic_init,
    &hid_classic_probe,
    &hid_classic_connect,
    &hid_classic_disconnect,
    &hid_classic_probe_classic,
    &hid_classic_connect_addr,
};
