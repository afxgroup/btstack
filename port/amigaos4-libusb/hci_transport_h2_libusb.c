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
 * Sending uses the synchronous libusb_control_transfer / libusb_bulk_transfer
 * on endpoint 0 / the bulk-OUT endpoint; these use a separate shared IOReq
 * and do not interfere with the outstanding async reads.
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
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#define USB_MAX_PATH_LEN        7
#define USB_DEFAULT_TIMEOUT_MS  2000
#define USB_POLL_INTERVAL_MS    5   /* timer period between non-blocking checks */

static int      event_in_addr;
static int      acl_in_addr;
static int      acl_out_addr;

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
static uint8_t                  event_buf[256];
static uint8_t                  acl_buf[HCI_ACL_PAYLOAD_SIZE];

static btstack_timer_source_t   usb_poll_timer;

static void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = NULL;

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
        printf("USB: %04x:%04x\n", desc.idVendor, desc.idProduct);
        btstack_linked_list_iterator_t it;
        btstack_linked_list_iterator_init(&it, &usb_known_devices);
        while (btstack_linked_list_iterator_has_next(&it)){
            usb_known_device_t * k = (usb_known_device_t *) btstack_linked_list_iterator_next(&it);
            if (desc.idVendor == k->vendor_id && desc.idProduct == k->product_id){
                found = list[i];
                usb_vendor_id = desc.idVendor;
                usb_product_id = desc.idProduct;
                break;
            }
        }
        if (found) break;
    }
    if (found) libusb_ref_device(found);
    libusb_free_device_list(list, 1);
    return found;
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
                if      (is_in  && attr == LIBUSB_TRANSFER_TYPE_INTERRUPT) event_in_addr = addr;
                else if (is_in  && attr == LIBUSB_TRANSFER_TYPE_BULK)      acl_in_addr   = addr;
                else if (!is_in && attr == LIBUSB_TRANSFER_TYPE_BULK)      acl_out_addr  = addr;
            }
        }
    }
    if (!event_in_addr || !acl_in_addr || !acl_out_addr){
        printf("usb_open: endpoints not found (evt=%02x acl_in=%02x acl_out=%02x)\n",
               event_in_addr, acl_in_addr, acl_out_addr);
        return -1;
    }
    printf("usb_open: endpoints event=0x%02x acl_in=0x%02x acl_out=0x%02x\n",
           event_in_addr, acl_in_addr, acl_out_addr);
    return 0;
}

/*
 * Timer callback — runs every USB_POLL_INTERVAL_MS on the BTstack main thread.
 * libusb_async_check wraps CheckIO (non-blocking); libusb_async_wait wraps
 * WaitIO which returns immediately because CheckIO already confirmed done.
 */
static void usb_poll(btstack_timer_source_t * ts){
    if (CheckSignal(SIGBREAKF_CTRL_C)){
        printf("CTRL-C received, shutting down\n");
        btstack_run_loop_trigger_exit();
        return;
    }

    if (!usb_transport_open) return;

    /* --- HCI event (interrupt IN) --- */
    if (ILibusb1->libusb_async_check(event_transfer) == 1){
        int32 transferred = 0;
        int r = ILibusb1->libusb_async_wait(event_transfer, &transferred);
        if (r == LIBUSB_SUCCESS && transferred > 0){
            printf("usb_poll: HCI event %ld bytes (0x%02x)\n", (long)transferred, event_buf[0]);
            if (packet_handler) packet_handler(HCI_EVENT_PACKET, event_buf, (uint16_t)transferred);
        } else if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_INTERRUPTED){
            log_error("event async_wait: %d", r);
        }
        ILibusb1->libusb_async_submit(event_transfer, event_in_addr, event_buf, sizeof(event_buf));
    }

    /* --- ACL data (bulk IN) --- */
    if (ILibusb1->libusb_async_check(acl_transfer) == 1){
        int32 transferred = 0;
        int r = ILibusb1->libusb_async_wait(acl_transfer, &transferred);
        if (r == LIBUSB_SUCCESS && transferred > 0){
            printf("usb_poll: ACL %ld bytes\n", (long)transferred);
            if (packet_handler) packet_handler(HCI_ACL_DATA_PACKET, acl_buf, (uint16_t)transferred);
        } else if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_INTERRUPTED){
            log_error("acl async_wait: %d", r);
        }
        ILibusb1->libusb_async_submit(acl_transfer, acl_in_addr, acl_buf, sizeof(acl_buf));
    }

    btstack_run_loop_set_timer(ts, USB_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void usb_init(const void * transport_config){ UNUSED(transport_config); }

static int usb_open(void){
    if (usb_transport_open) return 0;
    packet_handler = &dummy_handler;

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

    /* Allocate one async transfer per IN endpoint */
    event_transfer = ILibusb1->libusb_async_alloc(usb_handle, usb_port);
    acl_transfer   = ILibusb1->libusb_async_alloc(usb_handle, usb_port);
    if (!event_transfer || !acl_transfer){
        printf("usb_open: libusb_async_alloc failed\n");
        if (event_transfer){ ILibusb1->libusb_async_free(event_transfer); event_transfer = NULL; }
        if (acl_transfer)  { ILibusb1->libusb_async_free(acl_transfer);   acl_transfer   = NULL; }
        FreeSysObject(ASOT_PORT, usb_port); usb_port = NULL;
        libusb_release_interface(usb_handle, 0); libusb_close(usb_handle); libusb_exit(usb_ctx); usb_ctx = NULL;
        return -1;
    }

    /* Submit the first reads — they stay in flight until explicitly aborted */
    ILibusb1->libusb_async_submit(event_transfer, event_in_addr, event_buf, sizeof(event_buf));
    ILibusb1->libusb_async_submit(acl_transfer,   acl_in_addr,   acl_buf,   sizeof(acl_buf));

    usb_transport_open = 1;
    usb_bus = libusb_get_bus_number(usb_handle->dev);

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
    btstack_run_loop_remove_timer(&usb_poll_timer);
    if (event_transfer){ ILibusb1->libusb_async_abort(event_transfer); ILibusb1->libusb_async_free(event_transfer); event_transfer = NULL; }
    if (acl_transfer)  { ILibusb1->libusb_async_abort(acl_transfer);   ILibusb1->libusb_async_free(acl_transfer);   acl_transfer   = NULL; }
    if (usb_port)      { FreeSysObject(ASOT_PORT, usb_port); usb_port = NULL; }
    if (usb_handle){ libusb_release_interface(usb_handle, 0); libusb_close(usb_handle); usb_handle = NULL; }
    if (usb_ctx){ libusb_exit(usb_ctx); usb_ctx = NULL; }
    printf("usb_close: done\n");
    return 0;
}

static void usb_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)){
    packet_handler = handler ? handler : &dummy_handler;
}

static int usb_can_send_packet_now(uint8_t packet_type){ UNUSED(packet_type); return usb_transport_open ? 1 : 0; }

static int usb_send_packet(uint8_t packet_type, uint8_t * packet, int size){
    if (!usb_transport_open) return -1;
    int r; int32 transferred;
    switch (packet_type){
        case HCI_COMMAND_DATA_PACKET:
            r = libusb_control_transfer(usb_handle,
                                        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE,
                                        0, 0, 0, (uint8_t *) packet, size, USB_DEFAULT_TIMEOUT_MS);
            if (r < 0){ log_error("HCI command failed: %d", r); printf("usb_send: command failed %d\n", r); return -1; }
            return 0;
        case HCI_ACL_DATA_PACKET:
            r = libusb_bulk_transfer(usb_handle, acl_out_addr, packet, size, &transferred, USB_DEFAULT_TIMEOUT_MS);
            if (r != 0){ log_error("HCI ACL bulk failed: %d", r); return -1; }
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
