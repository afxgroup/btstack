/**
 * @title A2DP Source - sending audio to headphones and speakers
 *
 * Drives a Bluetooth audio sink: the Amiga is the source, the headphones or
 * speaker are the sink. Audio is encoded to SBC, which every A2DP device is
 * required to accept, and paced by a timer rather than by the sink - A2DP has
 * no flow control worth the name, so the sender is responsible for producing
 * samples at the rate the stream was configured for and no faster.
 *
 * Where the samples come from is deliberately behind one function. A tone is
 * built in so the whole chain can be proved before anything else depends on it;
 * feeding it from AHI, so that Bluetooth headphones become a system audio
 * device, is the next thing and does not change anything below.
 */

#ifndef BT_HANDLER_A2DP_H
#define BT_HANDLER_A2DP_H

#include <stdbool.h>
#include <stdint.h>
#include "bt_profile_handler.h"

#if defined __cplusplus
extern "C" {
#endif

extern const bt_profile_handler_t bt_handler_a2dp;

/**
 * @brief Where the audio comes from.
 *
 * Called to fill a buffer with interleaved 16 bit stereo samples at the rate
 * the stream negotiated. Returning fewer frames than asked for is silence, not
 * an error - a source with nothing to say is normal.
 *
 * @param buffer     num_frames * channels signed 16 bit samples, interleaved
 * @param num_frames how many frames are wanted
 * @return           how many were provided
 */
typedef uint16_t (*bt_audio_source_t)(int16_t * buffer, uint16_t num_frames);

/** @brief Use this source instead of the built-in tone. NULL restores it. */
void bt_handler_a2dp_set_source(bt_audio_source_t source);

/** @brief Sample rate the stream settled on, 0 when not streaming */
uint32_t bt_handler_a2dp_sample_rate(void);

/** @brief Log the stream setup and each timing correction */
void bt_handler_a2dp_set_verbose(bool enabled);

#if defined __cplusplus
}
#endif

#endif // BT_HANDLER_A2DP_H
