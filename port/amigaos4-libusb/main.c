/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "main.c"

// *****************************************************************************
//
// minimal setup for HCI code
//
// *****************************************************************************

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

// note: __USE_INLINE__ is defined globally by CMakeLists.txt
#include <exec/exec.h>
#include <interfaces/exec.h>
#include <libraries/libusb-1.h>
#include <interfaces/libusb-1.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/libusb-1.h>

// AmigaOS 4 SDK defines UNUSED as __attribute__((unused)); BTstack needs the (void) form
#undef UNUSED

#include <hci_dump_posix_stdout.h>

#include <btstack_config.h>

#include "ble/le_device_db_tlv.h"
#include "ble/sm.h"
#include "bluetooth_company_id.h"
#include "btstack_audio.h"
#include "btstack_chipset_realtek.h"
#include "btstack_chipset_zephyr.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_amigaos.h"
#include "btstack_stdin.h"
#include "amigaos4_input.h"
#include "bt_service_port.h"
#include "bt_usb_watch.h"
#include "btstack_tlv_posix.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "hal_led.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_posix_fs.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#define USB_VENDOR_ID_REALTEK 0x0bda

// Bonding keys live here. Not T:, which is wiped on reboot: a pairing the user
// made once has to survive, or every restart means pairing the mouse again.
#define TLV_DB_FOLDER      "ENVARC:Bluetooth"
#define TLV_DB_PATH_PREFIX TLV_DB_FOLDER "/btstack_"
// used when the folder cannot be created, e.g. a read-only ENVARC:
#define TLV_DB_PATH_PREFIX_FALLBACK "T:btstack_"
#define TLV_DB_PATH_POSTFIX ".tlv"

// AmigaOS 4 libusb-1.library explicit open/close
//
// Note: proto/libusb-1.h only declares 'extern struct Libusb1IFace *ILibusb1'
// (the inline4 macros dereference it). The definition normally comes from the
// libusb-1.a link stubs; as we open the library and get the interface ourselves,
// this port provides the global instead and does not link the stubs.
struct Libusb1IFace       * ILibusb1;

static struct Library     * libusb1_base;
static struct Libusb1IFace * libusb1_iface;

static void amigaos4_libusb1_open(void){
    if (libusb1_base == NULL){
        libusb1_base = OpenLibrary("libusb-1.library", 0L);
        if (libusb1_base == NULL){
            fprintf(stderr, "ERROR: cannot open libusb-1.library\n");
            exit(EXIT_FAILURE);
        }
    }
    if (libusb1_iface == NULL){
        libusb1_iface = (struct Libusb1IFace *) GetInterface(libusb1_base, "main", 1, NULL);
        if (libusb1_iface == NULL){
            fprintf(stderr, "ERROR: cannot get libusb-1.library main interface\n");
            CloseLibrary(libusb1_base);
            libusb1_base = NULL;
            exit(EXIT_FAILURE);
        }
    }
    // Publish the global interface pointer used by the inline macros
    ILibusb1 = libusb1_iface;
}

static void amigaos4_libusb1_close(void){
    if (libusb1_iface){
        DropInterface((struct Interface *) libusb1_iface);
        libusb1_iface = NULL;
        ILibusb1 = NULL;
    }
    if (libusb1_base){
        CloseLibrary(libusb1_base);
        libusb1_base = NULL;
    }
}
static char tlv_db_path[100];
static bool tlv_reset;
// folder that contains the Realtek firmware/config files, NULL = current directory
static const char * firmware_folder_path;
// accept LE Legacy Pairing, i.e. leave LE Secure Connections Only mode
static bool allow_legacy_pairing;
static const btstack_tlv_t * tlv_impl;
static btstack_tlv_posix_t   tlv_context;
static bd_addr_t             local_addr;

int btstack_main(int argc, const char * argv[]);

static bd_addr_t static_address;
static int using_static_address;

static btstack_packet_callback_registration_t hci_event_callback_registration;

// shutdown
static bool shutdown_triggered;

// Where the Realtek firmware and config files live.
//
// The chipset driver opens them relative to the current directory, which is
// wherever the user happened to be - for a service started by bt.usbfd that is
// C:, where they certainly are not. So look for them in the places they belong,
// in order of how explicit the choice is:
//
//   -f FOLDER        the user said so
//   BT:Firmware      an assign for a self contained installation of the stack
//   SYS:Firmware/bt  the system firmware drawer, where AmigaOS keeps such files
//   .                current directory, which is how the demos were used
static const char * realtek_firmware_folder(void){

    if (firmware_folder_path != NULL) return firmware_folder_path;

    static const char * const candidates[] = { "BT:Firmware", "SYS:Firmware/bt", "." };

    // Probing an assign that is not mounted would pop up a "please insert
    // volume" requester, which is the last thing a background service should do.
    APTR old_window = SetProcWindow((APTR) -1);

    const char * folder = ".";
    uint8_t i;
    for (i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); i++){
        BPTR lock = Lock(candidates[i], SHARED_LOCK);
        if (lock == ZERO) continue;
        UnLock(lock);
        folder = candidates[i];
        break;
    }

    SetProcWindow(old_window);
    return folder;
}

// Make sure the bonding folder exists and return the prefix to use for the TLV
// file. Falls back to T: rather than failing: better a stack that works and
// forgets its pairings than one that does not come up at all.
static const char * tlv_db_path_prefix(void){
    BPTR lock = Lock(TLV_DB_FOLDER, SHARED_LOCK);
    if (lock == ZERO){
        lock = CreateDir(TLV_DB_FOLDER);
        if (lock == ZERO){
            printf("WARNING: cannot create %s, pairings will not survive a reboot\n", TLV_DB_FOLDER);
            return TLV_DB_PATH_PREFIX_FALLBACK;
        }
    }
    UnLock(lock);
    return TLV_DB_PATH_PREFIX;
}

static void local_version_information_handler(uint8_t * packet){
    printf("Local version information:\n");
    uint16_t hci_version    = packet[6];
    uint16_t hci_revision   = little_endian_read_16(packet, 7);
    uint16_t lmp_version    = packet[9];
    uint16_t manufacturer   = little_endian_read_16(packet, 10);
    uint16_t lmp_subversion = little_endian_read_16(packet, 12);
    printf("- HCI Version    0x%04x\n", hci_version);
    printf("- HCI Revision   0x%04x\n", hci_revision);
    printf("- LMP Version    0x%04x\n", lmp_version);
    printf("- LMP Subversion 0x%04x\n", lmp_subversion);
    printf("- Manufacturer   0x%04x\n", manufacturer);
    switch (manufacturer){
        case BLUETOOTH_COMPANY_ID_THE_LINUX_FOUNDATION:
            printf("- Linux Foundation - assume Zephyr hci_usb firmware running on nRF52xx\n");
            // setup Zephyr chipset support
            hci_set_chipset(btstack_chipset_zephyr_instance());
            // sm required to setup static random Bluetooth address
            sm_init();
            break;
        default:
            break;
    }
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    (void)channel;
    (void)size;
    uint8_t i;
    uint8_t usb_path_len;
    const uint8_t * usb_path;
    uint16_t product_id;
    uint16_t vendor_id;
    const uint8_t * params;

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)){
        case HCI_EVENT_TRANSPORT_USB_INFO:
            usb_path_len = hci_event_transport_usb_info_get_path_len(packet);
            usb_path = hci_event_transport_usb_info_get_path(packet);
            // print device path
            product_id = hci_event_transport_usb_info_get_product_id(packet);
            vendor_id = hci_event_transport_usb_info_get_vendor_id(packet);
            printf("USB device 0x%04x/0x%04x, path: %u:",
                vendor_id, product_id, hci_event_transport_usb_info_get_bus(packet));
            for (i=0;i<usb_path_len;i++){
                if (i) printf(".");
                printf("%u", usb_path[i]);
            }
            printf("\n");

            // set Product ID for Realtek Controllers and use Realtek-specific stack startup
            if (vendor_id == USB_VENDOR_ID_REALTEK) {
                printf("Realtek Controller - requires firmware and config download\n");
                printf("Note: files must be uncompressed (no .zst) and named exactly as printed below\n");
                {
                    const char * folder = realtek_firmware_folder();
                    btstack_chipset_realtek_set_firmware_folder_path(folder);
                    btstack_chipset_realtek_set_config_folder_path(folder);
                }
                /*
                 * Say so when BTstack has never heard of this controller.
                 *
                 * The Realtek chipset driver looks the firmware up by USB
                 * product id, and a product id it does not recognise makes it
                 * give up - with a log_info nobody sees, and without even
                 * printing which firmware it would have used, because that line
                 * comes after the return. The controller then starts perfectly
                 * well and runs with no firmware at all.
                 *
                 * That failure is almost impossible to recognise for what it
                 * is: Classic works, so inquiry finds devices and a keyboard
                 * pairs and types, while LE reports nothing whatsoever and a
                 * mouse is simply never seen. It looks like a Bluetooth Low
                 * Energy problem, and it is a missing file.
                 *
                 * New dongles appear faster than the table is updated, so this
                 * is worth checking rather than assuming.
                 */
                {
                    uint16_t known = btstack_chipset_realtek_get_num_usb_controllers();
                    bool     found = false;
                    uint16_t i;
                    for (i = 0; i < known; i++){
                        uint16_t v = 0, p = 0;
                        btstack_chipset_realtek_get_vendor_product_id(i, &v, &p);
                        if ((v == vendor_id) && (p == product_id)){
                            found = true;
                            break;
                        }
                    }
                    if (!found){
                        printf("*** WARNING: BTstack does not know the Realtek controller %04x:%04x\n",
                               vendor_id, product_id);
                        printf("*** No firmware will be loaded, and nothing else will say so:\n");
                        printf("*** the chipset driver gives up before it prints which files it wanted.\n");
                        printf("*** The controller still runs on its ROM firmware, so this is not\n");
                        printf("*** necessarily fatal - but anything the patch fixes stays unfixed.\n");
                        printf("*** The HCI Revision and LMP Subversion below identify the chip:\n");
                        printf("*** look them up in chipset/realtek/btstack_chipset_realtek.c and add\n");
                        printf("*** this product id to fw_patch_table_usb.\n");
                    }
                }

                btstack_chipset_realtek_set_product_id(product_id);
                hci_set_chipset(btstack_chipset_realtek_instance());
                hci_enable_custom_pre_init();
            }
            break;
        case BTSTACK_EVENT_STATE:
            switch (btstack_event_state_get_state(packet)){
                case HCI_STATE_WORKING:
                    gap_local_bd_addr(local_addr);
                    if (using_static_address){
                        memcpy(local_addr, static_address, 6);
                    }
                    btstack_strcpy(tlv_db_path, sizeof(tlv_db_path), tlv_db_path_prefix());
                    btstack_strcat(tlv_db_path, sizeof(tlv_db_path), bd_addr_to_str_with_delimiter(local_addr, '-'));
                    btstack_strcat(tlv_db_path, sizeof(tlv_db_path), TLV_DB_PATH_POSTFIX);
                    printf("TLV path: %s", tlv_db_path);
                    if (tlv_reset){
                        int rc = unlink(tlv_db_path);
                        if (rc == 0) {
                            printf(", reset ok");
                        } else {
                            printf(", reset failed with result = %d", rc);
                        }
                    }
                    printf("\n");
                    tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, tlv_db_path);
                    btstack_tlv_set_instance(tlv_impl, &tlv_context);
#ifdef ENABLE_CLASSIC
                    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));
#endif
#ifdef ENABLE_BLE
                    le_device_db_tlv_configure(tlv_impl, &tlv_context);
#endif
                    printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));
                    break;
                case HCI_STATE_OFF:
                    btstack_tlv_posix_deinit(&tlv_context);
                    if (!shutdown_triggered) break;
                    log_info("Good bye, see you.\n");
                    printf("Bluetooth off, leaving run loop\n");
                    // leave the run loop instead of exit(): main() then does the
                    // cleanup in one single, well defined shutdown path
                    btstack_run_loop_trigger_exit();
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_COMMAND_COMPLETE:
            switch (hci_event_command_complete_get_command_opcode(packet)){
                case HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION:
                    local_version_information_handler(packet);
                    break;
                case HCI_OPCODE_HCI_ZEPHYR_READ_STATIC_ADDRESS:
                    params = hci_event_command_complete_get_return_parameters(packet);
                    if(params[0] != 0)
                        break;
                    if(size < 13)
                        break;
                    reverse_48(&params[2], static_address);
                    gap_random_address_set(static_address);
                    using_static_address = 1;
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void trigger_shutdown(void){
    log_info("trigger_shutdown: powering off");
    shutdown_triggered = true;
    hci_power_control(HCI_POWER_OFF);
    // the run loop keeps running until HCI reports HCI_STATE_OFF, see above
}

static void amigaos4_atexit_close_libusb(void){
    amigaos4_libusb1_close();
}

static int led_state = 0;

void hal_led_toggle(void){
    led_state = 1 - led_state;
    printf("LED State %u\n", led_state);
}

static char short_options[] = "hu:l:rf:pd:";

static struct option long_options[] = {
    {"help",        no_argument,        NULL,   'h'},
    {"logfile",    required_argument,  NULL,   'l'},
    {"reset-tlv",    no_argument,       NULL,   'r'},
    {"usbpath",    required_argument,  NULL,   'u'},
    {"fwpath",     required_argument,  NULL,   'f'},
    {"legacy-pairing", no_argument,    NULL,   'p'},
    {"device",     required_argument,  NULL,   'd'},
    {0, 0, 0, 0}
};

static char *help_options[] = {
    "print (this) help.",
    "set file to store debug output and HCI trace (off by default: costs CPU).",
    "reset bonding information stored in TLV.",
    "set USB path, format BUS:PORT-PORT-PORT, e.g. 1:1.2.3 of Bluetooth Controller.",
    "set folder with Realtek firmware/config files, default: current directory.",
    "accept LE Legacy Pairing, for devices without LE Secure Connections.",
    "use this USB controller, format VID:PID e.g. 3151:3020.",
};

static char *option_arg_name[] = {
    "",
    "LOGFILE",
    "",
    "USBPATH",
    "FWPATH",
    "",
    "VID:PID",
};

static void usage(const char *name){
    unsigned int i;
    printf( "usage:\n\t%s [options]\n", name );
    printf("valid options:\n");
    for( i=0; long_options[i].name != 0; i++) {
        printf("--%-10s| -%c  %-10s\t\t%s\n", long_options[i].name, long_options[i].val, option_arg_name[i], help_options[i] );
    }
}

#define USB_MAX_PATH_LEN 7
/*
 * Give everything back, whichever way we are leaving.
 *
 * There used to be two exits: the full one at the end, and a short one for when
 * btstack_main() refused to start - which closed the transport, the run loop and
 * libusb, and nothing else. Starting a second service while one was running took
 * that short path, and amigaos4_input_open() had already created two message
 * ports by then, so the process exited with their two signal bits still
 * allocated: 0x0C000000, reported by the Shell.
 *
 * Every step is printed on the way out, so if this ever hangs, the last line
 * says which step did not return. Each of these does nothing when the thing it
 * releases was never set up, so the early exit can call it just as safely.
 */
static void shutdown_everything(bool ran)
{
    // put the console back into normal mode
    btstack_stdin_reset();

    // release input.device if an application (e.g. bthid) opened it. Does
    // nothing otherwise, and releases held mouse buttons before closing.
    if (ran) amigaos4_input_dump_stats();
    amigaos4_input_close();

    // Remove the public MsgPort if an application (BluetoothService) created
    // one. Does nothing otherwise. Without this the port stays registered after
    // the process is gone, its signal bits are never freed, and the next start
    // finds the name taken by a port belonging to a task that no longer exists.
    bt_service_port_close();

    // Stop listening for USB devices coming and going. Nothing is subscribed
    // unless the service set it up, so this does nothing for bthid.
    bt_usb_watch_close();

    // A forced exit (second CTRL-C) leaves the HCI state machine mid-flight, so
    // close the transport explicitly - this releases the USB interface and closes
    // the device in any case.
    hci_transport_usb_instance()->close();
    printf("shutdown: transport closed\n");

    hci_dump_posix_fs_close();
    printf("shutdown: packet log closed\n");

    btstack_run_loop_amigaos_deinit();
    printf("shutdown: run loop deinit done\n");

    amigaos4_libusb1_close();
    printf("shutdown: libusb-1.library closed\n");
}

int main(int argc, const char * argv[]){

    uint8_t usb_path[USB_MAX_PATH_LEN];
    uint8_t usb_bus = 0;
    int usb_path_len = 0;
    const char * usb_path_string = NULL;
    const char * log_file_path = NULL;

    // parse command line parameters.
    // Options we do not know belong to the application (btstack_main gets the
    // same argv and e.g. bthid has -t and -v), so skip them instead of
    // complaining and stopping - that would silently drop our own options
    // appearing after them.
    opterr = 0;
    while(true){
        int c = getopt_long(argc, (char* const*)argv, short_options, long_options, NULL);
        if (c < 0) {
            break;
        }
        if (c == '?'){
            continue;
        }
        switch (c) {
            case 'u':
                usb_path_string = optarg;
                break;
            case 'l':
                log_file_path = optarg;
                break;
            case 'r':
                tlv_reset = true;
                break;
            case 'f':
                firmware_folder_path = optarg;
                break;
            case 'p':
                allow_legacy_pairing = true;
                break;
            case 'd':
                // A controller that does not declare the standard USB class can
                // be named here. Needed because the interfaces, where such a
                // device carries its class, cannot be read before opening it.
                {
                    char * end = NULL;
                    long vid = strtol(optarg, &end, 16);
                    long pid = (end && (*end == ':')) ? strtol(end + 1, NULL, 16) : -1;
                    if ((vid <= 0) || (pid < 0)){
                        printf("ERROR: -d wants VID:PID in hex, e.g. -d 3151:3020\n");
                        return EXIT_FAILURE;
                    }
                    printf("Using USB controller %04x:%04x\n", (unsigned) vid, (unsigned) pid);
                    hci_transport_usb_add_device((uint16_t) vid, (uint16_t) pid);
                }
                break;
            case 'h':
            default:
                usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (usb_path_string != NULL){
        // parse command line options for "-u 1:1-2-3"
        printf("Specified USB ");
        bool have_bus = false;
        while (1){
            char * delimiter;
            int number = (int) strtol(usb_path_string, &delimiter, 16);
            if (have_bus == false && *delimiter == ':') {
                usb_bus = number;
                have_bus = true;
                printf("Bus %u and ", usb_bus);
                usb_path_string = delimiter+1;
                continue;
            }
            if (usb_path_len == 0) {
                printf("Path ");
            }
            usb_path[usb_path_len] = number;
            usb_path_len++;
            printf("%u ", number);
            if (!delimiter) break;
            if ((*delimiter != '-') && (*delimiter != '.')) break;
            usb_path_string = delimiter+1;
        }
        printf("\n");
    }

    /// GET STARTED with BTstack ///

    // Open AmigaOS 4 libusb-1.library before any libusb call
    amigaos4_libusb1_open();
    atexit(amigaos4_atexit_close_libusb);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_amigaos_get_instance());

    if (usb_path_len){
        // Note: if usb_bus was not set, has the same effect as calling
        // hci_transport_usb_set_bus.
        hci_transport_usb_set_bus_and_path(usb_bus, usb_path_len, usb_path);
    }

    // Packet log in HCI_DUMP_PACKETLOGGER format, only when asked for with -l.
    // It is not free: every packet costs a formatting pass plus unbuffered
    // write() calls, and with hci_dump uninitialised every log_info() in the
    // stack short-circuits as well. That matters for a data path like a mouse
    // that produces packets continuously.
    if (log_file_path != NULL){
        hci_dump_posix_fs_open(log_file_path, HCI_DUMP_PACKETLOGGER);
        const hci_dump_t * hci_dump_impl = hci_dump_posix_fs_get_instance();
        hci_dump_init(hci_dump_impl);
        printf("Packet Log: %s\n", log_file_path);
    }

    // init HCI
    hci_init(hci_transport_usb_instance(), NULL);

    // inform about BTstack state
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // register callback for CTRL-C. Note: not btstack_signal_register_callback(),
    // that one needs the posix run loop's pipe/select mechanism which does not
    // work here - the AmigaOS run loop waits on SIGBREAKF_CTRL_C itself.
    btstack_run_loop_amigaos_set_break_handler(&trigger_shutdown);

    // register known Realtek USB Controllers
    uint16_t realtek_num_controllers = btstack_chipset_realtek_get_num_usb_controllers();
    uint16_t i;
    for (i=0;i<realtek_num_controllers;i++){
        uint16_t vendor_id;
        uint16_t product_id;
        btstack_chipset_realtek_get_vendor_product_id(i, &vendor_id, &product_id);
        hci_transport_usb_add_device(vendor_id, product_id);
    }

    // setup app. A negative return means the application refused to start -
    // BluetoothService does that when another instance already owns the public
    // port. Running the loop anyway would leave a second, useless process
    // behind, which is exactly what happened.
    if (btstack_main(argc, argv) < 0){
        shutdown_everything(false);
        return EXIT_FAILURE;
    }

    // sm_init() - called by the example above - always enables LE Secure
    // Connections Only mode when ENABLE_LE_SECURE_CONNECTIONS is configured, so
    // this has to be undone here, after the example is set up. Without it,
    // pairing with a device that only supports LE Legacy Pairing is rejected
    // with SM_REASON_AUTHENTHICATION_REQUIREMENTS (reason 3).
    if (allow_legacy_pairing){
        printf("LE Legacy Pairing accepted (LE Secure Connections not required)\n");
        sm_set_secure_connections_only_mode(false);
    }

    // go
    btstack_run_loop_execute();

    printf("shutdown: run loop left\n");
    shutdown_everything(true);
    printf("shutdown: bye\n");
    return 0;
}


