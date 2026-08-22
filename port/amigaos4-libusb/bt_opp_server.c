/*
 * OBEX Object Push server - receiving files. See bt_opp_server.h.
 */

#include <stdio.h>
#include <string.h>

#include "bt_opp_server.h"

#include <proto/exec.h>

/* the SDK's UNUSED is an attribute, not BTstack's UNUSED(x) */
#undef UNUSED

#include "btstack_debug.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "classic/goep_server.h"
#include "classic/obex.h"
#include "classic/obex_parser.h"
#include "classic/sdp_util.h"
#include "bluetooth.h"
#include "bluetooth_sdp.h"
#include "l2cap.h"
#include "classic/sdp_util.h"
#include "classic/sdp_server.h"

#define OPP_RFCOMM_CHANNEL     9
#define OPP_L2CAP_PSM          0x1015
#define OPP_MAX_FRAME_SIZE     0xFFFF
#define OPP_NAME_MAX           64
#define OPP_PATH_MAX           256

static uint8_t      opp_sdp_record[220];
static uint16_t     opp_goep_cid;
static bool         opp_verbose;
static char         opp_folder[OPP_PATH_MAX] = "RAM:";

static obex_parser_t opp_parser;
static char          opp_name[OPP_NAME_MAX];
static char          opp_path[OPP_PATH_MAX];
static FILE        * opp_file;
static uint32_t      opp_received;
static uint8_t       opp_response;      /* what to answer once we may send */
static bool          opp_response_is_connect;

/* -------------------------------------------------------------------------- */

void bt_opp_server_set_folder(const char * folder){
    if ((folder == NULL) || (folder[0] == 0)) return;
    btstack_strcpy(opp_folder, sizeof(opp_folder), folder);
}

const char * bt_opp_server_get_folder(void){
    return opp_folder;
}

void bt_opp_server_set_verbose(bool enabled){
    opp_verbose = enabled;
}

/*
 * Turn the name the sender chose into one that can only name a file here.
 *
 * It arrives as UTF-16 and it is the other side's to choose, so it is treated
 * as hostile: anything that could climb out of the chosen drawer - a colon, a
 * slash, a leading dot - is not translated but replaced. A file called
 * "../../C/Shell" has to land as a file, in the drawer the user picked, or not
 * at all.
 */
static void opp_name_from_utf16(const uint8_t * utf16, uint16_t len){
    uint16_t out = 0;
    uint16_t i;

    for (i = 0; (i + 1) < len; i += 2){
        uint16_t codepoint = big_endian_read_16(utf16, i);
        if (codepoint == 0) break;
        if (out >= (sizeof(opp_name) - 1)) break;

        char c = (codepoint < 0x80) ? (char) codepoint : '_';
        if ((c == '/') || (c == ':') || (c == '\\') || (c < 0x20)) c = '_';
        opp_name[out++] = c;
    }
    opp_name[out] = 0;

    if ((opp_name[0] == 0) || (opp_name[0] == '.')){
        btstack_strcpy(opp_name, sizeof(opp_name), "received.dat");
    }
}

static void opp_file_close(bool keep){
    if (opp_file == NULL) return;
    fclose(opp_file);
    opp_file = NULL;

    if (keep){
        DebugPrintF("opp: received '%s' (%lu bytes)\n", opp_path, (unsigned long) opp_received);
    } else {
        remove(opp_path);
        DebugPrintF("opp: transfer of '%s' was abandoned\n", opp_path);
    }
    opp_received = 0;
}

static bool opp_file_open(void){
    /*
     * Built by hand rather than with snprintf, which cannot be told that the
     * buffer holds a whole folder and a whole name with room over, and warns
     * about a truncation that cannot happen.
     */
    uint16_t len = (uint16_t) strlen(opp_folder);
    bool     needs_slash = (len > 0) && (opp_folder[len - 1] != ':') && (opp_folder[len - 1] != '/');

    btstack_strcpy(opp_path, sizeof(opp_path), opp_folder);
    if (needs_slash) btstack_strcat(opp_path, sizeof(opp_path), "/");
    btstack_strcat(opp_path, sizeof(opp_path), opp_name);

    opp_file = fopen(opp_path, "wb");
    if (opp_file == NULL){
        DebugPrintF("opp: cannot write '%s'\n", opp_path);
        return false;
    }
    opp_received = 0;
    return true;
}

/* -------------------------------------------------------------------------- */

static void opp_parser_callback(void * user_data, uint8_t header_id, uint16_t total_len,
                                uint16_t data_offset, const uint8_t * data_buffer, uint16_t data_len){
    UNUSED(user_data);
    UNUSED(total_len);

    switch (header_id){
        case OBEX_HEADER_NAME:
            /* arrives in chunks, and the first chunk is where the file is opened
             * - a body can follow in the very same packet */
            if (data_offset == 0){
                opp_name_from_utf16(data_buffer, data_len);
                if (opp_verbose) log_info("opp: receiving '%s'", opp_name);
            }
            break;

        case OBEX_HEADER_BODY:
        case OBEX_HEADER_END_OF_BODY:
            if (opp_file == NULL){
                if (opp_name[0] == 0){
                    btstack_strcpy(opp_name, sizeof(opp_name), "received.dat");
                }
                if (!opp_file_open()) break;
            }
            if (fwrite(data_buffer, 1, data_len, opp_file) != data_len){
                DebugPrintF("opp: writing '%s' failed\n", opp_path);
                opp_file_close(false);
                break;
            }
            opp_received += data_len;
            break;

        default:
            break;
    }
}

static void opp_handle_request(void){

    obex_parser_operation_info_t info;
    obex_parser_get_operation_info(&opp_parser, &info);

    opp_response_is_connect = false;

    switch (info.opcode){
        case OBEX_OPCODE_CONNECT:
            opp_response            = OBEX_RESP_SUCCESS;
            opp_response_is_connect = true;
            opp_name[0]             = 0;
            break;

        case OBEX_OPCODE_PUT:
            /* not the final packet: more is coming */
            opp_response = OBEX_RESP_CONTINUE;
            break;

        case (OBEX_OPCODE_PUT | 0x80):
            /* the final packet of a PUT. An empty one with no body at all is
             * how a sender deletes, which is not something to honour here. */
            if (opp_file != NULL){
                opp_file_close(true);
                opp_response = OBEX_RESP_SUCCESS;
            } else {
                opp_response = OBEX_RESP_FORBIDDEN;
            }
            opp_name[0] = 0;
            break;

        case OBEX_OPCODE_ABORT:
            opp_file_close(false);
            opp_response = OBEX_RESP_SUCCESS;
            break;

        case OBEX_OPCODE_DISCONNECT:
            opp_response = OBEX_RESP_SUCCESS;
            break;

        default:
            /* Object Push means push. Pulling the "owner's business card" is
             * the one other thing the profile allows and there is none to give,
             * so anything else is answered honestly rather than ignored. */
            opp_response = OBEX_RESP_NOT_IMPLEMENTED;
            break;
    }

    goep_server_request_can_send_now(opp_goep_cid);
}

static void opp_send_response(void){
    if (opp_response_is_connect){
        goep_server_response_create_connect(opp_goep_cid, OBEX_VERSION, 0,
                                            goep_server_response_get_max_message_size(opp_goep_cid));
    } else {
        goep_server_response_create_general(opp_goep_cid);
    }
    goep_server_execute(opp_goep_cid, opp_response);
}

static void opp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);

    switch (packet_type){
        case HCI_EVENT_PACKET:
            if (hci_event_packet_get_type(packet) != HCI_EVENT_GOEP_META) break;

            switch (hci_event_goep_meta_get_subevent_code(packet)){
                case GOEP_SUBEVENT_INCOMING_CONNECTION:
                    /*
                     * Accepted without asking. A file arriving is not dangerous
                     * by itself - it lands in a drawer and nothing runs it - and
                     * there is nowhere to ask from at this point. What protects
                     * the machine is where it can be written and under what
                     * name, which is dealt with above.
                     */
                    opp_goep_cid = goep_subevent_incoming_connection_get_goep_cid(packet);
                    DebugPrintF("opp: incoming connection, accepting\n");
                    goep_server_accept_connection(opp_goep_cid);
                    break;

                case GOEP_SUBEVENT_CONNECTION_OPENED:
                    opp_goep_cid = goep_subevent_connection_opened_get_goep_cid(packet);
                    opp_name[0]  = 0;
                    obex_parser_init_for_request(&opp_parser, &opp_parser_callback, NULL);
                    DebugPrintF("opp: connection opened\n");
                    break;

                case GOEP_SUBEVENT_CONNECTION_CLOSED:
                    /* a transfer cut off half way leaves a partial file, which
                     * is worse than no file: it looks like it worked */
                    opp_file_close(false);
                    opp_goep_cid = 0;
                    break;

                case GOEP_SUBEVENT_CAN_SEND_NOW:
                    opp_send_response();
                    obex_parser_init_for_request(&opp_parser, &opp_parser_callback, NULL);
                    break;

                default:
                    break;
            }
            break;

        case GOEP_DATA_PACKET:
            if (obex_parser_process_data(&opp_parser, packet, size) == OBEX_PARSER_OBJECT_STATE_COMPLETE){
                opp_handle_request();
            }
            break;

        default:
            break;
    }
}

/* -------------------------------------------------------------------------- */

static void opp_create_sdp_record(uint8_t * service, uint32_t service_record_handle,
                                  uint8_t rfcomm_channel, uint16_t l2cap_psm, const char * name){
    uint8_t * attribute;
    de_create_sequence(service);

    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(service, DE_UINT, DE_SIZE_32, service_record_handle);

    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    attribute = de_push_sequence(service);
    de_add_number(attribute, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH);
    de_pop_sequence(service, attribute);

    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST);
    attribute = de_push_sequence(service);
    {
        uint8_t * l2cp = de_push_sequence(attribute);
        de_add_number(l2cp, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_L2CAP);
        de_pop_sequence(attribute, l2cp);

        uint8_t * rfcomm = de_push_sequence(attribute);
        de_add_number(rfcomm, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_RFCOMM);
        de_add_number(rfcomm, DE_UINT, DE_SIZE_8,  rfcomm_channel);
        de_pop_sequence(attribute, rfcomm);

        uint8_t * obex = de_push_sequence(attribute);
        de_add_number(obex, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_OBEX);
        de_pop_sequence(attribute, obex);
    }
    de_pop_sequence(service, attribute);

    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BROWSE_GROUP_LIST);
    attribute = de_push_sequence(service);
    de_add_number(attribute, DE_UUID, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
    de_pop_sequence(service, attribute);

    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BLUETOOTH_PROFILE_DESCRIPTOR_LIST);
    attribute = de_push_sequence(service);
    {
        uint8_t * profile = de_push_sequence(attribute);
        de_add_number(profile, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH);
        de_add_number(profile, DE_UINT, DE_SIZE_16, 0x0102);   /* OPP 1.2 */
        de_pop_sequence(attribute, profile);
    }
    de_pop_sequence(service, attribute);

    /* GOEP L2CAP PSM, which is how a modern sender avoids RFCOMM entirely */
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_GOEP_L2CAP_PSM);
    de_add_number(service, DE_UINT, DE_SIZE_16, l2cap_psm);

    de_add_number(service, DE_UINT, DE_SIZE_16, 0x0100);   /* ServiceName */
    de_add_data(service, DE_STRING, (uint16_t) strlen(name), (uint8_t *) name);

    /*
     * Supported formats: 0xFF, "any". Listing vCard and the rest would be a
     * promise to understand them, and this writes whatever arrives to a file
     * without looking inside it.
     */
    de_add_number(service, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SUPPORTED_FORMATS_LIST);
    attribute = de_push_sequence(service);
    de_add_number(attribute, DE_UINT, DE_SIZE_8, 0xFF);
    de_pop_sequence(service, attribute);
}

void bt_opp_server_init(const char * service_name){

    goep_server_init();

    /*
     * The result matters. A channel already taken, or a transport the stack
     * cannot offer, fails here and quietly - and the SDP record still gets
     * registered, so the service is advertised, browsed, chosen, and then
     * nothing answers. From the other end that is a connection attempt that
     * hangs, which is a far worse failure than being refused.
     */
    uint8_t status = goep_server_register_service(&opp_packet_handler,
                                                  OPP_RFCOMM_CHANNEL, OPP_MAX_FRAME_SIZE,
                                                  OPP_L2CAP_PSM, l2cap_max_mtu(), LEVEL_0);
    if (status != ERROR_CODE_SUCCESS){
        DebugPrintF("opp: cannot offer object push, status 0x%02x - not advertising it\n", status);
        return;
    }

    memset(opp_sdp_record, 0, sizeof(opp_sdp_record));
    opp_create_sdp_record(opp_sdp_record, sdp_create_service_record_handle(),
                          OPP_RFCOMM_CHANNEL, OPP_L2CAP_PSM,
                          (service_name != NULL) ? service_name : "Object Push");
    sdp_register_service(opp_sdp_record);

    DebugPrintF("opp: object push registered on RFCOMM %u / L2CAP 0x%04x, files go to %s\n",
                OPP_RFCOMM_CHANNEL, OPP_L2CAP_PSM, opp_folder);
}
