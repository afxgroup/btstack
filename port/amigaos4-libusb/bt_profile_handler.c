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

static const bt_profile_handler_t * handlers[] = {
    &bt_handler_hid,
    NULL
};

static bt_profile_status_callback_t status_callback;

void bt_profile_handler_set_status_callback(bt_profile_status_callback_t callback){
    status_callback = callback;
}

void bt_profile_handler_report_status(hci_con_handle_t con_handle, bool in_use, uint8_t status){
    if (status_callback == NULL) return;
    status_callback(con_handle, in_use, status);
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
