/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * ...license header omitted for brevity...
 *
 */

#define BTSTACK_FILE__ "bt_profile_handler.c"

/*
 * The profile handlers built into the service.
 *
 * Adding a profile means writing the handler and adding one line here; nothing
 * in the service itself has to change.
 */

#include "bt_profile_handler.h"
#include "bt_handler_hid.h"
#include "bt_handler_hid_classic.h"

static const bt_profile_handler_t * handlers[] = {
    &bt_handler_hid,
    &bt_handler_hid_classic,
    NULL
};

static bt_profile_status_callback_t status_callback;

void bt_profile_handler_set_status_callback(bt_profile_status_callback_t callback){
    status_callback = callback;
}

void bt_profile_handler_report_status(const struct bt_profile_handler * handler,
                                      const bd_addr_t addr, hci_con_handle_t con_handle,
                                      bool in_use, uint8_t status){
    if (status_callback == NULL) return;
    status_callback(handler, addr, con_handle, in_use, status);
}

/*
 * Which handler can drive a Classic device of this kind.
 *
 * A device restored from storage has never been probed - there was no
 * advertisement and no inquiry result to probe, it just came back from the TLV
 * with its address and what it is. Without this it had no handler, and the code
 * that reconnects bonded Classic devices skipped it for exactly that reason,
 * which is a poor way to treat the one device we are most sure about.
 *
 * A Classic-capable handler is one that can connect to a bare address; kind
 * picks between them, and any of them will do when the stored kind is unknown.
 */
const bt_profile_handler_t * bt_profile_handler_classic_for_kind(bt_device_kind_t kind){
    uint8_t i;
    const bt_profile_handler_t * fallback = NULL;

    for (i = 0; handlers[i] != NULL; i++){
        if (handlers[i]->connect_addr == NULL) continue;
        if (handlers[i]->kind == kind) return handlers[i];
        if (fallback == NULL) fallback = handlers[i];
    }
    return fallback;
}

const bt_profile_handler_t ** bt_profile_handlers(void){
    return handlers;
}

const bt_profile_handler_t * bt_profile_handler_probe(const uint8_t * ad_data, uint8_t ad_len){
    uint8_t i;
    for (i = 0; handlers[i] != NULL; i++){
        if (handlers[i]->probe == NULL) continue;
        if (handlers[i]->probe(ad_data, ad_len)) return handlers[i];
    }
    return NULL;
}

const bt_profile_handler_t * bt_profile_handler_probe_classic(uint32_t class_of_device){
    uint8_t i;
    for (i = 0; handlers[i] != NULL; i++){
        if (handlers[i]->probe_classic == NULL) continue;
        if (handlers[i]->probe_classic(class_of_device)) return handlers[i];
    }
    return NULL;
}
