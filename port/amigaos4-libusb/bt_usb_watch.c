/*
 * Watching for the last Bluetooth controller leaving. See bt_usb_watch.h for
 * why this is here and not in the USB function driver.
 */

#include "bt_usb_watch.h"

#include <exec/exec.h>
#include <exec/exectags.h>
#include <usb/usb.h>
#include <usb/system.h>

#include <proto/exec.h>
#include <proto/usbresource.h>

/* the SDK defines UNUSED as an attribute, which is not BTstack's UNUSED(x) */
#undef UNUSED

#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_amigaos.h"

/* answered by the transport, which already knows how to look for one */
extern bool hci_transport_usb_controller_present(void);

/*
 * Declared by the proto header and defined by nobody: the inline macros reach
 * straight through these, so they are ours to fill in and ours to give back.
 */
struct Library          * USBResourceBase;
struct USBResourceIFace * IUSBResource;
static struct MsgPort         * notify_port;
static APTR                     subscription;
static void                  (* gone_callback)(void);
static btstack_data_source_t    watch_data_source;

static void bt_usb_watch_process(btstack_data_source_t * ds, btstack_data_source_callback_type_t type){
    UNUSED(ds);
    UNUSED(type);

    if (notify_port == NULL) return;

    bool something_left = false;

    struct Message * message;
    while ((message = GetMsg(notify_port)) != NULL){
        struct USBNotifyMsg * notify = (struct USBNotifyMsg *) message;
        uint16_t kind = notify->Type;
        ReplyMsg(message);

        if (kind == USBNM_TYPE_FUNCTIONREMOVED){
            something_left = true;   /* worth asking, once, after the queue is drained */
        }
    }

    if (!something_left) return;

    /*
     * Asked once per batch rather than per message, and only after every
     * message has been replied: unplugging a hub removes a great many
     * functions at once, and the answer to "is there still a controller" is the
     * same for all of them.
     */
    if (hci_transport_usb_controller_present()) return;

    log_info("usb watch: the last Bluetooth controller is gone");
    if (gone_callback != NULL) gone_callback();
}

bool bt_usb_watch_open(void (*last_controller_gone)(void)){

    if (notify_port != NULL) return true;

    USBResourceBase = OpenLibrary("usbresource.library", 53);
    if (USBResourceBase == NULL){
        log_info("usb watch: no usbresource.library, not watching");
        return false;
    }
    IUSBResource = (struct USBResourceIFace *) GetInterface(USBResourceBase, "main", 1, NULL);
    if (IUSBResource == NULL){
        CloseLibrary(USBResourceBase);
        USBResourceBase = NULL;
        return false;
    }

    notify_port = AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (notify_port == NULL){
        bt_usb_watch_close();
        return false;
    }

    subscription = USBResAddNotify(USBNM_TYPE_FUNCTIONREMOVED, notify_port);
    if (subscription == NULL){
        log_info("usb watch: the stack refused the subscription");
        bt_usb_watch_close();
        return false;
    }

    gone_callback = last_controller_gone;

    /* the run loop has to wake for this, not only for USB transfers */
    btstack_run_loop_amigaos_add_signal_mask(1UL << notify_port->mp_SigBit);

    btstack_run_loop_set_data_source_handler(&watch_data_source, &bt_usb_watch_process);
    btstack_run_loop_enable_data_source_callbacks(&watch_data_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&watch_data_source);

    return true;
}

void bt_usb_watch_close(void){

    if (subscription != NULL){
        USBResRemNotify(subscription);
        subscription = NULL;
        btstack_run_loop_remove_data_source(&watch_data_source);
    }

    if (notify_port != NULL){
        /*
         * Drain first. Unsubscribing stops new ones, and anything already
         * delivered is still ours to reply - the stack is waiting for it.
         */
        struct Message * message;
        while ((message = GetMsg(notify_port)) != NULL){
            ReplyMsg(message);
        }
        btstack_run_loop_amigaos_remove_signal_mask(1UL << notify_port->mp_SigBit);
        SetSignal(0, 1UL << notify_port->mp_SigBit);
        FreeSysObject(ASOT_PORT, notify_port);
        notify_port = NULL;
    }

    if (IUSBResource != NULL){
        DropInterface((struct Interface *) IUSBResource);
        IUSBResource = NULL;
    }
    if (USBResourceBase != NULL){
        CloseLibrary(USBResourceBase);
        USBResourceBase = NULL;
    }
    gone_callback = NULL;
}
