/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "hci_transport_h2_libusb.c"

/*
 * HCI Transport for AmigaOS4 using the libusb-1.library async API.
 *
 * Uses libusb_async_alloc/submit/check/wait (AmigaOS4-specific extension).
 * libusb_async_check wraps IExec->CheckIO() — completely non-blocking.
 * libusb_async_wait wraps IExec->WaitIO() — returns immediately when CheckIO
 * already confirmed completion.  A 5 ms BTstack timer polls both IN endpoints
 * without ever blocking the run-loop.
 *
 * HCI commands are sent with the synchronous libusb_control_transfer() on
 * endpoint 0. ACL packets use the async API as well: the synchronous
 * libusb_bulk_transfer() blocks the run loop, and on this libusb-1.library every
 * other call only returns when its timeout expires, which added exactly 2 s to
 * every second ACL transaction and made GATT service discovery crawl.
 * HCI_EVENT_TRANSPORT_PACKET_SENT is reported when the OUT transfer completes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <exec/types.h>
#include <exec/exectags.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libusb.h>

/* AmigaOS 4 SDK defines UNUSED as __attribute__((unused)) */
#undef UNUSED

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_linked_list.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_amigaos.h"
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#define USB_MAX_PATH_LEN        7
#define USB_DEFAULT_TIMEOUT_MS  2000
#define USB_POLL_INTERVAL_MS    50  /* safety net only: the run loop also wakes up
                                     * on the USB MsgPort signal, so this timer just
                                     * catches a missed completion. Each tick costs a
                                     * timer.device SendIO/AbortIO round trip. */
#define USB_STATS_INTERVAL_MS   2000 /* how often to report that we are still alive */

static int      event_in_addr;
static int      acl_in_addr;
static int      acl_out_addr;

/* wMaxPacketSize of the IN endpoints - the AmigaOS USB stack expects the
 * requested transfer length to be a multiple of it */
static uint16_t event_in_packet_size;
static uint16_t acl_in_packet_size;

/* requested read length: largest multiple of wMaxPacketSize that fits the buffer */
static uint16_t event_in_read_len;
static uint16_t acl_in_read_len;

static uint8_t  usb_bus;
static int      usb_path_len;
static uint8_t  usb_path[USB_MAX_PATH_LEN];
static uint16_t usb_vendor_id;
static uint16_t usb_product_id;

static libusb_context       * usb_ctx;
static libusb_device_handle * usb_handle;
static int                    usb_transport_open;

/* Async transfer objects and their data buffers */
static struct MsgPort         * usb_port;
static libusb_async_transfer  * event_transfer;
static libusb_async_transfer  * acl_transfer;
/* ACL OUT is submitted asynchronously as well: the synchronous
 * libusb_bulk_transfer() blocks the whole run loop, and on this libusb-1.library
 * every other call only returns after its full timeout */
static libusb_async_transfer  * acl_out_transfer;
static bool                     acl_out_pending;
static bool                     acl_out_async;   /* false: fall back to sync bulk */
/* Buffers are over-allocated by USB_MAX_PACKET_SIZE so that rounding the requested
 * length down to a multiple of wMaxPacketSize still leaves room for a full packet */
#define USB_MAX_PACKET_SIZE 64
static uint8_t                  event_buf[HCI_EVENT_BUFFER_SIZE + USB_MAX_PACKET_SIZE];
/* ACL data is delivered to the stack with HCI_INCOMING_PRE_BUFFER_SIZE bytes in
 * front of the HCI ACL header (used by BNEP to avoid a memcpy) */
static uint8_t                  acl_buf[HCI_INCOMING_PRE_BUFFER_SIZE + HCI_ACL_BUFFER_SIZE + USB_MAX_PACKET_SIZE];
static uint8_t                * acl_data = &acl_buf[HCI_INCOMING_PRE_BUFFER_SIZE];

/* diagnostics - reported while the controller has not sent anything yet */
static uint32_t                 usb_poll_ticks;
static bool                     usb_first_packet_received;
static uint32_t                 usb_event_empty_count;
static uint32_t                 usb_event_error_count;
static int                      usb_event_last_error;
static uint32_t                 usb_acl_empty_count;
static uint32_t                 usb_acl_error_count;
static uint32_t                 usb_submit_error_count;

static btstack_timer_source_t   usb_poll_timer;
/* polled by the run loop on every wake-up, e.g. when the USB MsgPort is signalled */
static btstack_data_source_t    usb_poll_data_source;

/* set after a packet was handed to the controller, reported to HCI from the
 * run loop timer (never from within send_packet to avoid recursion) */
static bool                     packet_sent_pending;

static void dummy_handler(uint8_t pt, uint8_t *p, uint16_t s);

static void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = &dummy_handler;

typedef struct {
    btstack_linked_item_t next;
    uint16_t vendor_id;
    uint16_t product_id;
} usb_known_device_t;

static btstack_linked_list_t usb_known_devices;

/* -------------------------------------------------------------------------- */

static void dummy_handler(uint8_t pt, uint8_t *p, uint16_t s){
    UNUSED(pt); UNUSED(p); UNUSED(s);
}

static void hci_transport_h2_libusb_emit_usb_info(void){
    uint8_t event[8 + USB_MAX_PATH_LEN];
    uint16_t pos = 0;
    event[pos++] = HCI_EVENT_TRANSPORT_USB_INFO;
    event[pos++] = 6 + usb_path_len;
    little_endian_store_16(event, pos, usb_vendor_id); pos += 2;
    little_endian_store_16(event, pos, usb_product_id); pos += 2;
    event[pos++] = usb_bus;
    event[pos++] = usb_path_len;
    memcpy(&event[pos], usb_path, usb_path_len);
    pos += usb_path_len;
    if (packet_handler) packet_handler(HCI_EVENT_PACKET, event, pos);
}

void hci_transport_usb_add_device(uint16_t vendor_id, uint16_t product_id){
    usb_known_device_t * d = (usb_known_device_t *) malloc(sizeof(usb_known_device_t));
    if (!d) return;
    d->vendor_id = vendor_id; d->product_id = product_id;
    btstack_linked_list_add(&usb_known_devices, (btstack_linked_item_t *) d);
}

void hci_transport_usb_set_path(int len, uint8_t * port_numbers){
    if (len > USB_MAX_PATH_LEN || !port_numbers) return;
    usb_path_len = len;
    memcpy(usb_path, port_numbers, len);
}

void hci_transport_usb_set_bus_and_path(uint8_t bus, int len, uint8_t* port_numbers){
    hci_transport_usb_set_path(len, port_numbers);
    usb_bus = bus;
}

static libusb_device * usb_find_device(void){
    libusb_device ** list;
    ssize_t cnt = libusb_get_device_list(usb_ctx, &list);
    if (cnt < 0){ log_error("libusb_get_device_list failed: %d", (int)cnt); return NULL; }
    libusb_device * found = NULL;
    for (ssize_t i = 0; i < cnt; i++){
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        printf("USB: %04x:%04x (class %02x/%02x/%02x)\n", desc.idVendor, desc.idProduct,
               desc.bDeviceClass, desc.bDeviceSubClass, desc.bDeviceProtocol);

        /*
         * Any controller that says it speaks Bluetooth is taken: Wireless
         * Controller / RF Controller / Bluetooth programming. This is what makes
         * a dongle nobody ever heard of work, as long as it is a plain HCI
         * controller - which most are. A chipset that needs a firmware upload
         * still needs its own driver on top, see btstack_chipset_realtek.
         */
        if ((desc.bDeviceClass == 0xE0) && (desc.bDeviceSubClass == 0x01) && (desc.bDeviceProtocol == 0x01)){
            found = list[i];
        } else {
            /* known device that does not declare the standard class */
            btstack_linked_list_iterator_t it;
            btstack_linked_list_iterator_init(&it, &usb_known_devices);
            while (btstack_linked_list_iterator_has_next(&it)){
                usb_known_device_t * k = (usb_known_device_t *) btstack_linked_list_iterator_next(&it);
                if ((desc.idVendor == k->vendor_id) && (desc.idProduct == k->product_id)){
                    found = list[i];
                    break;
                }
            }
        }

        if (found){
            usb_vendor_id  = desc.idVendor;
            usb_product_id = desc.idProduct;
            break;
        }
    }
    if (found) libusb_ref_device(found);
    libusb_free_device_list(list, 1);
    return found;
}

/* largest multiple of packet_size that fits into buffer_size */
static uint16_t usb_read_len(uint16_t buffer_size, uint16_t packet_size){
    if (packet_size == 0) return buffer_size;
    return (uint16_t) ((buffer_size / packet_size) * packet_size);
}

static int usb_find_endpoints(const struct libusb_config_descriptor * config){
    event_in_addr = acl_in_addr = acl_out_addr = 0;
    if (!config || !config->interface){ log_error("usb_find_endpoints: NULL config"); return -1; }
    for (int i = 0; i < config->bNumInterfaces; i++){
        const struct libusb_interface * iface = &config->interface[i];
        if (!iface || !iface->altsetting) continue;
        for (int a = 0; a < iface->num_altsetting; a++){
            const struct libusb_interface_descriptor * alt = &iface->altsetting[a];
            if (!alt || !alt->endpoint) continue;
            for (int e = 0; e < alt->bNumEndpoints; e++){
                const struct libusb_endpoint_descriptor * ep = &alt->endpoint[e];
                if (!ep) continue;
                uint8_t addr = ep->bEndpointAddress;
                uint8_t attr = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                int is_in = (addr & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
                if      (is_in  && attr == LIBUSB_TRANSFER_TYPE_INTERRUPT){
                    event_in_addr = addr;
                    event_in_packet_size = ep->wMaxPacketSize;
                } else if (is_in  && attr == LIBUSB_TRANSFER_TYPE_BULK){
                    acl_in_addr = addr;
                    acl_in_packet_size = ep->wMaxPacketSize;
                } else if (!is_in && attr == LIBUSB_TRANSFER_TYPE_BULK){
                    acl_out_addr = addr;
                }
            }
        }
    }
    if (!event_in_addr || !acl_in_addr || !acl_out_addr){
        printf("usb_open: endpoints not found (evt=%02x acl_in=%02x acl_out=%02x)\n",
               event_in_addr, acl_in_addr, acl_out_addr);
        return -1;
    }
    /* The AmigaOS USB stack rejects IN requests that are not a multiple of
     * wMaxPacketSize, so round the read length down. The buffers are large
     * enough that the result still holds a complete HCI event / ACL packet. */
    event_in_read_len = usb_read_len(sizeof(event_buf), event_in_packet_size);
    acl_in_read_len   = usb_read_len(sizeof(acl_buf) - HCI_INCOMING_PRE_BUFFER_SIZE, acl_in_packet_size);

    printf("usb_open: endpoints event=0x%02x (max %u, read %u) acl_in=0x%02x (max %u, read %u) acl_out=0x%02x\n",
           event_in_addr, event_in_packet_size, event_in_read_len,
           acl_in_addr, acl_in_packet_size, acl_in_read_len,
           acl_out_addr);
    return 0;
}

/* submit a read and report a failing submit - a silently rejected submit means
 * the controller never appears to answer */
static int usb_submit_transfer(libusb_async_transfer * transfer, int endpoint, uint8_t * buffer, uint16_t len,
                           const char * name){
    int r = ILibusb1->libusb_async_submit(transfer, endpoint, buffer, len);
    if (r != LIBUSB_SUCCESS){
        usb_submit_error_count++;
        log_error("%s submit (ep 0x%02x, %u bytes) failed: %d", name, endpoint, len, r);
        /* report the first few only - this is called from the poll timer */
        if (usb_submit_error_count <= 3){
            printf("usb: %s submit (ep 0x%02x, %u bytes) failed: %d\n", name, endpoint, len, r);
        }
    }
    return r;
}

/*
 * Poll both IN endpoints once and report a completed outgoing packet.
 * libusb_async_check wraps CheckIO (non-blocking); libusb_async_wait wraps
 * WaitIO which returns immediately because CheckIO already confirmed done.
 */
static void usb_poll_once(void){

    /* --- outgoing ACL packet finished? --- */
    if (acl_out_pending && (ILibusb1->libusb_async_check(acl_out_transfer) == 1)){
        int32 transferred = 0;
        int r = ILibusb1->libusb_async_wait(acl_out_transfer, &transferred);
        acl_out_pending = false;
        if (r != LIBUSB_SUCCESS){
            log_error("acl out async_wait: %d", r);
        }
        /* HCI may release the packet buffer now */
        packet_sent_pending = true;
    }

    /* --- report completion of the last outgoing packet ---
     * Our send_packet() is synchronous, but as we do provide can_send_packet_now(),
     * HCI treats this transport as asynchronous and keeps the outgoing packet
     * buffer reserved until it sees HCI_EVENT_TRANSPORT_PACKET_SENT. Emit it here,
     * from the run loop, so HCI can send the next packet. Without this the stack
     * stalls right after the first HCI command (Reset).
     */
    if (packet_sent_pending){
        packet_sent_pending = false;
        static const uint8_t packet_sent_event[] = { HCI_EVENT_TRANSPORT_PACKET_SENT, 0 };
        packet_handler(HCI_EVENT_PACKET, (uint8_t *) &packet_sent_event[0], sizeof(packet_sent_event));
        /* handling the event may have closed the transport */
        if (!usb_transport_open) return;
    }

    /* --- HCI event (interrupt IN) --- */
    if (ILibusb1->libusb_async_check(event_transfer) == 1){
        int32 transferred = 0;
        int r = ILibusb1->libusb_async_wait(event_transfer, &transferred);
        if (r == LIBUSB_SUCCESS && transferred > 0){
            log_debug("usb_poll: HCI event %ld bytes (0x%02x)", (long)transferred, event_buf[0]);
            /* only a real packet counts as 'controller is talking to us' */
            usb_first_packet_received = true;
            packet_handler(HCI_EVENT_PACKET, event_buf, (uint16_t)transferred);
        } else if (r == LIBUSB_SUCCESS){
            /* completed without data - normal for a polled interrupt endpoint */
            usb_event_empty_count++;
        } else {
            usb_event_error_count++;
            usb_event_last_error = r;
            log_error("event async_wait: %d", r);
        }
        if (!usb_transport_open) return;
        usb_submit_transfer(event_transfer, event_in_addr, event_buf, event_in_read_len, "event");
    }

    /* --- ACL data (bulk IN) --- */
    if (ILibusb1->libusb_async_check(acl_transfer) == 1){
        int32 transferred = 0;
        int r = ILibusb1->libusb_async_wait(acl_transfer, &transferred);
        if (r == LIBUSB_SUCCESS && transferred > 0){
            log_debug("usb_poll: ACL %ld bytes", (long)transferred);
            usb_first_packet_received = true;
            packet_handler(HCI_ACL_DATA_PACKET, acl_data, (uint16_t)transferred);
        } else if (r == LIBUSB_SUCCESS){
            usb_acl_empty_count++;
        } else {
            usb_acl_error_count++;
            log_error("acl async_wait: %d", r);
        }
        if (!usb_transport_open) return;
        usb_submit_transfer(acl_transfer, acl_in_addr, acl_data, acl_in_read_len, "acl");
    }
}

/*
 * Timer callback - runs every USB_POLL_INTERVAL_MS on the BTstack main thread.
 *
 * IMPORTANT: btstack_run_loop_base_process_timers() removes the timer *before*
 * calling us, so this function must re-arm it on every path. If it does not,
 * the timer list becomes empty, the posix run loop passes timeout = NULL to
 * select() and the whole process freezes. Hence the single exit point below.
 */
static void usb_poll_ds(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(ds); UNUSED(type);
    if (usb_transport_open){
        usb_poll_once();
    }
}

static void usb_poll(btstack_timer_source_t * ts){

    usb_poll_ticks++;

    /* While the controller has not sent anything yet, report progress every
     * USB_STATS_INTERVAL_MS. Printed first, so it also shows up if something
     * below blocks. */
    if ((usb_first_packet_received == false) &&
        ((usb_poll_ticks % (USB_STATS_INTERVAL_MS / USB_POLL_INTERVAL_MS)) == 0)){
        printf("usb_poll: alive %lu ms, no packet yet - event: %lu empty, %lu err (last %d); acl: %lu empty, %lu err; submit err %lu; tx pending %u\n",
               (unsigned long)(usb_poll_ticks * USB_POLL_INTERVAL_MS),
               (unsigned long)usb_event_empty_count, (unsigned long)usb_event_error_count, usb_event_last_error,
               (unsigned long)usb_acl_empty_count, (unsigned long)usb_acl_error_count,
               (unsigned long)usb_submit_error_count, (unsigned int)packet_sent_pending);
    }

    if (CheckSignal(SIGBREAKF_CTRL_C)){
        printf("CTRL-C received, shutting down\n");
        /* deliberately not re-arming the timer here */
        btstack_run_loop_trigger_exit();
        return;
    }

    if (usb_transport_open){
        usb_poll_once();
    }

    /* single exit point: always re-arm while open */
    if (usb_transport_open){
        btstack_run_loop_set_timer(ts, USB_POLL_INTERVAL_MS);
        btstack_run_loop_add_timer(ts);
    }
}

static void usb_init(const void * transport_config){ UNUSED(transport_config); }

static int usb_open(void){
    if (usb_transport_open) return 0;

    /* NOTE: do not touch packet_handler here - it has been set by hci_init() via
     * register_packet_handler() and resetting it to dummy_handler would silently
     * drop all packets from the controller. */
    packet_sent_pending = false;
    usb_first_packet_received = false;
    usb_poll_ticks = 0;
    usb_event_empty_count = 0;
    usb_event_error_count = 0;
    usb_event_last_error = 0;
    usb_acl_empty_count = 0;
    usb_acl_error_count = 0;
    usb_submit_error_count = 0;

    int r = libusb_init(&usb_ctx);
    if (r != 0){ printf("usb_open: libusb_init failed: %d\n", r); return -1; }
    printf("usb_open: libusb_init OK\n");

    libusb_set_debug(usb_ctx, LIBUSB_LOG_LEVEL_WARNING);

    libusb_device * device = usb_find_device();
    if (!device){ printf("usb_open: no BT device found\n"); libusb_exit(usb_ctx); usb_ctx = NULL; return -1; }
    printf("usb_open: found device %04x:%04x\n", usb_vendor_id, usb_product_id);

    r = libusb_open(device, &usb_handle);
    libusb_unref_device(device);
    if (r != 0){ printf("usb_open: libusb_open failed: %d\n", r); libusb_exit(usb_ctx); usb_ctx = NULL; return -1; }
    printf("usb_open: device opened OK\n");

    struct libusb_config_descriptor * config;
    r = libusb_get_config_descriptor(usb_handle->dev, 0, &config);
    if (r != 0){ printf("usb_open: get_config_descriptor failed: %d\n", r); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL; return -1; }

    if (usb_find_endpoints(config) != 0){ libusb_free_config_descriptor(config); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL; return -1; }

    int32 current_config = 0;
    libusb_get_configuration(usb_handle, &current_config);
    if (current_config != config->bConfigurationValue){
        r = libusb_set_configuration(usb_handle, config->bConfigurationValue);
        if (r != 0){ libusb_free_config_descriptor(config); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL; return -1; }
    }
    libusb_free_config_descriptor(config);

    r = libusb_claim_interface(usb_handle, 0);
    if (r != 0){ printf("usb_open: claim_interface failed: %d\n", r); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL; return -1; }
    printf("usb_open: interface claimed\n");

    /* Create the MsgPort that receives async USB completion messages */
    usb_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!usb_port){
        printf("usb_open: AllocSysObjectTags(PORT) failed\n");
        libusb_release_interface(usb_handle, 0); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL;
        return -1;
    }

    /* Allocate one async transfer per IN endpoint, plus one for ACL OUT */
    event_transfer   = ILibusb1->libusb_async_alloc(usb_handle, usb_port);
    acl_transfer     = ILibusb1->libusb_async_alloc(usb_handle, usb_port);
    acl_out_transfer = ILibusb1->libusb_async_alloc(usb_handle, usb_port);
    acl_out_async    = (acl_out_transfer != NULL);
    acl_out_pending  = false;
    if (!event_transfer || !acl_transfer){
        printf("usb_open: libusb_async_alloc failed\n");
        if (event_transfer)  { ILibusb1->libusb_async_free(event_transfer);   event_transfer   = NULL; }
        if (acl_transfer)    { ILibusb1->libusb_async_free(acl_transfer);     acl_transfer     = NULL; }
        if (acl_out_transfer){ ILibusb1->libusb_async_free(acl_out_transfer); acl_out_transfer = NULL; }
        FreeSysObject(ASOT_PORT, usb_port); usb_port = NULL;
        libusb_release_interface(usb_handle, 0); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL;
        return -1;
    }

    /* Submit the first reads — they stay in flight until explicitly aborted */
    usb_submit_transfer(event_transfer, event_in_addr, event_buf, event_in_read_len, "event");
    usb_submit_transfer(acl_transfer,   acl_in_addr,   acl_data,  acl_in_read_len,   "acl");

    usb_transport_open = 1;
    usb_bus = libusb_get_bus_number(usb_handle->dev);

    /* let the run loop wake up as soon as a transfer completes, instead of
     * waiting for the next poll tick */
    btstack_run_loop_amigaos_add_signal_mask(1UL << usb_port->mp_SigBit);

    /* processed on every run loop wake-up */
    btstack_run_loop_set_data_source_handler(&usb_poll_data_source, &usb_poll_ds);
    btstack_run_loop_enable_data_source_callbacks(&usb_poll_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&usb_poll_data_source);

    /* safety net in case a completion signal is missed */
    usb_poll_timer.process = &usb_poll;
    btstack_run_loop_set_timer(&usb_poll_timer, USB_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(&usb_poll_timer);
    printf("usb_open: async polling started\n");

    hci_transport_h2_libusb_emit_usb_info();
    return 0;
}

static int usb_close(void){
    if (!usb_transport_open) return 0;
    usb_transport_open = 0;
    packet_sent_pending = false;
    btstack_run_loop_remove_timer(&usb_poll_timer);
    btstack_run_loop_remove_data_source(&usb_poll_data_source);
    if (event_transfer){ ILibusb1->libusb_async_abort(event_transfer); ILibusb1->libusb_async_free(event_transfer); event_transfer = NULL; }
    if (acl_transfer)  { ILibusb1->libusb_async_abort(acl_transfer);   ILibusb1->libusb_async_free(acl_transfer);   acl_transfer   = NULL; }
    if (acl_out_transfer){
        if (acl_out_pending) ILibusb1->libusb_async_abort(acl_out_transfer);
        ILibusb1->libusb_async_free(acl_out_transfer); acl_out_transfer = NULL;
    }
    acl_out_pending = false;
    if (usb_port)      {
        btstack_run_loop_amigaos_remove_signal_mask(1UL << usb_port->mp_SigBit);
        /* the aborted transfers may still have replies queued - drain the port
         * before freeing it, or FreeSysObject trips over them */
        struct Message * msg;
        while ((msg = GetMsg(usb_port)) != NULL){}
        UNUSED(msg);
        /* and drop the signal, so the bit is clean for whoever allocates it next */
        SetSignal(0, 1UL << usb_port->mp_SigBit);
        FreeSysObject(ASOT_PORT, usb_port); usb_port = NULL;
    }
    if (usb_handle){ libusb_release_interface(usb_handle, 0); libusb_close(usb_handle); usb_handle = NULL; }
    if (usb_ctx){ libusb_exit(usb_ctx); usb_ctx = NULL; }
    printf("usb_close: done\n");
    return 0;
}

static void usb_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)){
    packet_handler = handler ? handler : &dummy_handler;
}

/* Note: keep can_send_packet_now() - HCI then expects HCI_EVENT_TRANSPORT_PACKET_SENT,
 * which is emitted from usb_poll(). Returning NULL here instead would mark the
 * transport as synchronous, but HCI would then rely on
 * btstack_run_loop_execute_on_main_thread(), which needs working pipe()/select(). */
static int usb_can_send_packet_now(uint8_t packet_type){
    UNUSED(packet_type);
    return (usb_transport_open && !packet_sent_pending && !acl_out_pending) ? 1 : 0;
}

static int usb_send_packet(uint8_t packet_type, uint8_t * packet, int size){
    if (!usb_transport_open) return -1;
    int r; int32 transferred;
    switch (packet_type){
        case HCI_COMMAND_DATA_PACKET:
            r = libusb_control_transfer(usb_handle,
                                        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE,
                                        0, 0, 0, (uint8_t *) packet, size, USB_DEFAULT_TIMEOUT_MS);
            if (r < 0){ log_error("HCI command failed: %d", r); printf("usb_send: command failed %d\n", r); return -1; }
            packet_sent_pending = true;
            return 0;
        case HCI_ACL_DATA_PACKET:
            /* Preferred path: async submit, completion is picked up by usb_poll_once().
             * HCI keeps the packet buffer valid until we report the packet as sent,
             * so it can be submitted without copying. */
            if (acl_out_async){
                r = usb_submit_transfer(acl_out_transfer, acl_out_addr, packet, (uint16_t) size, "acl out");
                if (r == LIBUSB_SUCCESS){
                    acl_out_pending = true;
                    return 0;
                }
                /* submit not supported for OUT endpoints: stop trying */
                log_error("acl out async submit failed, falling back to sync bulk transfer");
                printf("usb: async ACL out not available, using sync bulk transfer\n");
                acl_out_async = false;
            }
            r = libusb_bulk_transfer(usb_handle, acl_out_addr, packet, size, &transferred, USB_DEFAULT_TIMEOUT_MS);
            if (r != 0){ log_error("HCI ACL bulk failed: %d", r); return -1; }
            packet_sent_pending = true;
            return 0;
        default:
            return -1;
    }
}

static int  usb_set_baudrate(uint32_t b){ UNUSED(b); return 0; }
static void usb_reset_link(void){}
static void usb_set_sco_config(uint16_t v, int n){ UNUSED(v); UNUSED(n); }

static const hci_transport_t hci_transport_usb = {
    "H2_USB_AMIGAOS4",
    &usb_init, &usb_open, &usb_close,
    &usb_register_packet_handler,
    &usb_can_send_packet_now,
    &usb_send_packet,
    &usb_set_baudrate, &usb_reset_link, &usb_set_sco_config,
};

const hci_transport_t * hci_transport_usb_instance(void){
    return &hci_transport_usb;
}
