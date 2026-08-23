/*
 * A2DP Source - sending audio to headphones and speakers. See the header.
 */

#include <stdio.h>
#include <string.h>

#include "bt_handler_a2dp.h"

#include <proto/exec.h>

/* the SDK's UNUSED is an attribute, not BTstack's UNUSED(x) */
#undef UNUSED

#include "btstack_defines.h"

/* proto/exec.h brought the SDK's attribute-style UNUSED back; BTstack's
 * UNUSED(x) is what this file means by it */
#undef UNUSED
#define UNUSED(x) (void)(x)

#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "classic/a2dp_source.h"
#include "classic/avdtp.h"
#include "classic/btstack_sbc.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "bluetooth_sdp.h"

/*
 * How often to produce audio.
 *
 * Short enough that a hiccup is not audible and long enough that a run loop
 * turn is not spent on a handful of samples. The demo in BTstack uses the same
 * figure, and the sink's own buffering covers the jitter.
 */
#define AUDIO_TIMEOUT_MS      10

#define SBC_STORAGE_SIZE     1030
#define MAX_FRAMES_PER_ROUND  256

static const uint32_t PREFERRED_SAMPLE_RATE = 44100;

/* what we can do, and what we ask for */
static uint8_t sbc_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF,
    2, 53
};
static uint8_t sbc_configuration[4];

static uint8_t                  a2dp_sdp_record[150];
static avdtp_stream_endpoint_t * stream_endpoint;
static uint8_t                  local_seid;

static uint16_t                 a2dp_cid;
static bd_addr_t                a2dp_addr;
static hci_con_handle_t         a2dp_con_handle = HCI_CON_HANDLE_INVALID;
static bool                     verbose;

/* streaming state */
static btstack_timer_source_t   audio_timer;
static bool                     streaming;
static bool                     sbc_ready_to_send;
static uint8_t                  sbc_storage[SBC_STORAGE_SIZE];
static uint16_t                 sbc_storage_count;
static uint16_t                 max_media_payload;
static uint32_t                 stream_sample_rate;
static uint32_t                 samples_ready;
static uint32_t                 acc_missed_samples;
static uint32_t                 time_audio_sent_ms;

static const btstack_sbc_encoder_t * sbc_encoder;
static btstack_sbc_encoder_bluedroid_t sbc_encoder_state;

static bt_audio_source_t        audio_source;

/* -------------------------------------------------------------------------- */

void bt_handler_a2dp_set_verbose(bool enabled){
    verbose = enabled;
}

uint32_t bt_handler_a2dp_sample_rate(void){
    return streaming ? stream_sample_rate : 0;
}

void bt_handler_a2dp_set_source(bt_audio_source_t source){
    audio_source = source;
}

/*
 * The built-in source: a quiet tone.
 *
 * Not music, and not meant to be. It exists so the chain - connection, codec
 * negotiation, encoding, pacing, the sink's own buffering - can be proved end to
 * end without also depending on AHI. Something audible and steady makes a
 * dropout obvious in a way silence never would.
 */
static uint16_t tone_source(int16_t * buffer, uint16_t num_frames){
    static uint32_t phase;
    uint16_t i;

    for (i = 0; i < num_frames; i++){
        /* about 440 Hz at 44100, at a level that will not startle anyone */
        int16_t sample = (int16_t) (((phase % 100) < 50) ? 2000 : -2000);
        phase += 441;
        buffer[i * 2]     = sample;
        buffer[i * 2 + 1] = sample;
    }
    return num_frames;
}

/* -------------------------------------------------------------------------- */

static void fill_sbc_buffer(void){
    uint16_t frames_per_sbc = sbc_encoder->num_audio_frames(&sbc_encoder_state);
    uint16_t sbc_frame_size = sbc_encoder->sbc_buffer_length(&sbc_encoder_state);

    while (samples_ready >= frames_per_sbc){
        if ((sbc_storage_count + sbc_frame_size) > max_media_payload) break;
        if (frames_per_sbc > MAX_FRAMES_PER_ROUND) break;

        int16_t pcm[MAX_FRAMES_PER_ROUND * 2];
        bt_audio_source_t source = (audio_source != NULL) ? audio_source : &tone_source;

        uint16_t got = source(pcm, frames_per_sbc);
        if (got < frames_per_sbc){
            /* a source with nothing to say is silence, not a failure */
            memset(&pcm[got * 2], 0, (frames_per_sbc - got) * 2 * sizeof(int16_t));
        }

        sbc_encoder->encode_signed_16(&sbc_encoder_state, pcm,
                                      &sbc_storage[1 + sbc_storage_count]);
        sbc_storage_count += sbc_frame_size;
        samples_ready     -= frames_per_sbc;
    }
}

/*
 * Produce audio for however long has actually passed.
 *
 * Not for the timer's nominal period: a run loop that was busy elsewhere hands
 * this a longer gap, and producing a fixed amount regardless would drift
 * against the sink's clock until it ran dry or overflowed. The remainder is
 * carried rather than dropped, which is what keeps 44100 from becoming 44000.
 */
static void audio_timeout_handler(btstack_timer_source_t * timer){
    UNUSED(timer);

    btstack_run_loop_set_timer(&audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&audio_timer);

    if (!streaming) return;

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t elapsed_ms = (time_audio_sent_ms > 0) ? (now - time_audio_sent_ms) : AUDIO_TIMEOUT_MS;
    time_audio_sent_ms = now;

    uint32_t num_samples = (elapsed_ms * stream_sample_rate) / 1000;
    acc_missed_samples  += (elapsed_ms * stream_sample_rate) % 1000;
    while (acc_missed_samples >= 1000){
        num_samples++;
        acc_missed_samples -= 1000;
    }
    samples_ready += num_samples;

    if (sbc_ready_to_send) return;

    fill_sbc_buffer();

    if ((sbc_storage_count + sbc_encoder->sbc_buffer_length(&sbc_encoder_state)) > max_media_payload){
        sbc_ready_to_send = true;
        a2dp_source_stream_endpoint_request_can_send_now(a2dp_cid, local_seid);
    }
}

static void audio_timer_start(void){
    max_media_payload  = btstack_min(a2dp_max_media_payload_size(a2dp_cid, local_seid), SBC_STORAGE_SIZE);
    sbc_storage_count  = 0;
    sbc_ready_to_send  = false;
    samples_ready      = 0;
    acc_missed_samples = 0;
    time_audio_sent_ms = 0;
    streaming          = true;

    btstack_run_loop_remove_timer(&audio_timer);
    btstack_run_loop_set_timer_handler(&audio_timer, &audio_timeout_handler);
    btstack_run_loop_set_timer(&audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&audio_timer);
}

static void audio_timer_stop(void){
    streaming          = false;
    sbc_ready_to_send  = false;
    sbc_storage_count  = 0;
    samples_ready      = 0;
    acc_missed_samples = 0;
    time_audio_sent_ms = 0;
    btstack_run_loop_remove_timer(&audio_timer);
}

/* -------------------------------------------------------------------------- */

static void a2dp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)){

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
            uint8_t sampling_frequency_index =
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            stream_sample_rate = sampling_frequency_index;

            uint8_t num_channels =
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);

            sbc_encoder->configure(&sbc_encoder_state, SBC_MODE_STANDARD,
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet),
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet),
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) == AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS
                    ? SBC_ALLOCATION_METHOD_LOUDNESS : SBC_ALLOCATION_METHOD_SNR,
                (uint16_t) stream_sample_rate,
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet),
                (num_channels == 1) ? SBC_CHANNEL_MODE_MONO : SBC_CHANNEL_MODE_JOINT_STEREO);

            DebugPrintF("a2dp: codec agreed, %lu Hz, %u channels, bitpool %u\n",
                        (unsigned long) stream_sample_rate,
                        num_channels,
                        a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet));
            break;
        }

        case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
            uint8_t status = a2dp_subevent_stream_established_get_status(packet);
            a2dp_subevent_stream_established_get_bd_addr(packet, a2dp_addr);
            if (status != ERROR_CODE_SUCCESS){
                DebugPrintF("a2dp: stream to %s failed, status 0x%02x\n",
                            bd_addr_to_str(a2dp_addr), status);
                a2dp_cid = 0;
                bt_profile_handler_report_status(&bt_handler_a2dp, a2dp_addr,
                                                 HCI_CON_HANDLE_INVALID, false, status);
                break;
            }
            a2dp_cid   = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            DebugPrintF("a2dp: stream to %s established\n", bd_addr_to_str(a2dp_addr));

            /* nothing plays until something asks it to, so start it here: a
             * speaker that connects and stays silent looks broken */
            a2dp_source_start_stream(a2dp_cid, local_seid);
            break;
        }

        case A2DP_SUBEVENT_STREAM_STARTED:
            DebugPrintF("a2dp: streaming\n");
            audio_timer_start();
            bt_profile_handler_report_status(&bt_handler_a2dp, a2dp_addr,
                                             a2dp_con_handle, true, ERROR_CODE_SUCCESS);
            break;

        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            sbc_ready_to_send = false;
            if (sbc_storage_count == 0) break;
            /* the media payload header is the number of SBC frames it carries */
            sbc_storage[0] = (uint8_t) (sbc_storage_count / sbc_encoder->sbc_buffer_length(&sbc_encoder_state));
            a2dp_source_stream_send_media_payload_rtp(a2dp_cid, local_seid, 0, 0,
                                                      sbc_storage, sbc_storage_count + 1);
            sbc_storage_count = 0;
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
        case A2DP_SUBEVENT_STREAM_STOPPED:
            DebugPrintF("a2dp: streaming stopped\n");
            audio_timer_stop();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            DebugPrintF("a2dp: disconnected\n");
            audio_timer_stop();
            a2dp_cid        = 0;
            a2dp_con_handle = HCI_CON_HANDLE_INVALID;
            bt_profile_handler_report_status(&bt_handler_a2dp, a2dp_addr,
                                             HCI_CON_HANDLE_INVALID, false, ERROR_CODE_SUCCESS);
            break;

        default:
            if (verbose){
                DebugPrintF("a2dp: subevent 0x%02x\n",
                            hci_event_a2dp_meta_get_subevent_code(packet));
            }
            break;
    }
}

/* -------------------------------------------------------------------------- */

static void a2dp_init(void){

    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_packet_handler);

    stream_endpoint = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
                                                         sbc_capabilities, sizeof(sbc_capabilities),
                                                         sbc_configuration, sizeof(sbc_configuration));
    if (stream_endpoint == NULL){
        DebugPrintF("a2dp: cannot create the stream endpoint\n");
        return;
    }
    avdtp_set_preferred_sampling_frequency(stream_endpoint, PREFERRED_SAMPLE_RATE);
    local_seid = avdtp_local_seid(stream_endpoint);

    sbc_encoder = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_state);

    memset(a2dp_sdp_record, 0, sizeof(a2dp_sdp_record));
    a2dp_source_create_sdp_record(a2dp_sdp_record, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(a2dp_sdp_record);

    DebugPrintF("a2dp: source ready, seid %u\n", local_seid);
}

/*
 * Which Class of Device is an audio sink.
 *
 * Major device class 4 is Audio/Video, and within it the minor class says what
 * kind: a headset, headphones, a loudspeaker, a car kit. A camcorder is in
 * there too and has no business being sent audio, so the ones that can actually
 * play are named rather than taking the whole major class.
 */
static bool a2dp_probe_classic(uint32_t cod){
    if (((cod >> 8) & 0x1F) != 0x04) return false;

    uint8_t minor = (uint8_t) ((cod >> 2) & 0x3F);
    switch (minor){
        case 0x01:  /* wearable headset */
        case 0x02:  /* hands-free */
        case 0x05:  /* headphones */
        case 0x06:  /* portable audio */
        case 0x07:  /* car audio */
        case 0x0A:  /* loudspeaker */
            return true;
        default:
            return false;
    }
}

static uint8_t a2dp_connect_addr(const bd_addr_t addr){
    uint16_t cid = 0;
    uint8_t status = a2dp_source_establish_stream((uint8_t *) addr, &cid);
    if (status == ERROR_CODE_SUCCESS){
        a2dp_cid = cid;
        memcpy(a2dp_addr, addr, 6);
    }
    return status;
}

static uint8_t a2dp_connect_handle(hci_con_handle_t con_handle){
    /* A2DP is reached by address, like the other Classic profiles */
    UNUSED(con_handle);
    return ERROR_CODE_COMMAND_DISALLOWED;
}

static void a2dp_disconnect(hci_con_handle_t con_handle){
    UNUSED(con_handle);
    audio_timer_stop();
    if (a2dp_cid != 0){
        a2dp_source_disconnect(a2dp_cid);
        a2dp_cid = 0;
    }
}

static bool a2dp_probe_le(const uint8_t * ad_data, uint8_t ad_len){
    /* A2DP is Classic only */
    UNUSED(ad_data);
    UNUSED(ad_len);
    return false;
}

const bt_profile_handler_t bt_handler_a2dp = {
    "a2dp",
    BT_DEVICE_KIND_AUDIO,
    &a2dp_init,
    &a2dp_probe_le,
    &a2dp_connect_handle,
    &a2dp_disconnect,
    &a2dp_probe_classic,
    &a2dp_connect_addr,
};
