/*
 * The play process for bluetooth.audio.
 *
 * One mix cycle is the same three steps as in the USB Audio driver: advance
 * AHI's playback position, ask AHI to mix, then hand the samples on. Only the
 * last step differs, and it is simpler here - AHI mixes 16 bit in host byte
 * order and that is exactly what the SBC encoder on the other side wants, so
 * the samples are copied rather than converted.
 *
 * It runs as its own process because a mix burst is long and must not be done
 * on somebody else's time. Priority 50 keeps it ahead of ordinary work without
 * competing with the input handlers, which is where a stutter would be heard.
 */

#include "bluetooth_audio.h"

#include <devices/ahi.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

extern struct ExecIFace    * IExec;
extern struct DOSIFace     * IDOS;
extern struct UtilityIFace * IUtility;

/*
 * How much audio to keep ahead of the consumer.
 *
 * The service drains the ring at exactly the sample rate, so anything beyond
 * what covers the jitter is latency for its own sake. A quarter of the ring is
 * enough to survive a late run loop turn and still leave three quarters of the
 * buffer as headroom for a long mix burst.
 */
#define TARGET_FILL_FRAMES  (BT_AUDIO_RING_FRAMES / 4)

/*
 * How long to wait when the ring is full enough.
 *
 * Sleeping on the timer rather than spinning: the process wakes, tops the ring
 * up and goes back to sleep, so a machine doing nothing else is not warmed up
 * for no reason.
 */
#define IDLE_DELAY_TICKS    1     /* one tick is 20 ms */

/* -------------------------------------------------------------------------- */

/*
 * Room in the ring, in frames.
 *
 * One slot is deliberately never used: with a wholly full ring, write and read
 * are equal and full is indistinguishable from empty. Giving up one frame is
 * cheaper than a flag that both processes would have to agree about.
 */
static uint32 ring_space(BTAudioRing * ring){
    uint32 used = ring->bar_Write - ring->bar_Read;
    if (used >= BT_AUDIO_RING_FRAMES) return 0;
    return (BT_AUDIO_RING_FRAMES - 1) - used;
}

static uint32 ring_used(BTAudioRing * ring){
    uint32 used = ring->bar_Write - ring->bar_Read;
    if (used > BT_AUDIO_RING_FRAMES) used = BT_AUDIO_RING_FRAMES;
    return used;
}

/*
 * Apply the master volume AHI set through AHIsub_HardwareControl. Fixed
 * arithmetic: 0x10000 is unity, and that case is left alone so the ordinary
 * path stays a plain copy.
 */
static inline int16 scale(int16 sample, Fixed vol){
    if (vol == 0x10000) return sample;
    return (int16) (((int32) sample * (int32) vol) >> 16);
}

/*
 * One mix cycle into the ring.
 *
 * Returns how many frames were written, which is zero when there was no room -
 * the caller then sleeps rather than mixing audio that has nowhere to go and
 * would have to be thrown away.
 */
static uint32 mix_into_ring(struct BluetoothAudioData * dd){
    struct AHIAudioCtrlDrv * AudioCtrl = dd->AudioCtrl;
    BTAudioRing * ring = dd->ba_Ring;

    uint32 frames = AudioCtrl->ahiac_BuffSamples;
    if (frames == 0) return 0;
    if (ring_space(ring) < frames) return 0;

    /* advance AHI's idea of where playback is */
    IUtility->CallHookPkt(AudioCtrl->ahiac_PlayerFunc, (APTR) AudioCtrl, NULL);

    /* and let it mix into our buffer */
    IUtility->CallHookPkt(AudioCtrl->ahiac_MixerFunc, (APTR) AudioCtrl, dd->ba_MixBuffer);

    uint32 write = ring->bar_Write;
    uint32 i;

    /*
     * The master volume AHI handed us through AHIsub_HardwareControl. It is
     * Fixed, so 0x10000 is unity and the common case costs nothing.
     */
    Fixed vol = dd->ba_OutputVolume;

    if (AudioCtrl->ahiac_Flags & AHIACF_HIFI){
        /*
         * HiFi mixing is 32 bit; the ring is 16. Taking the top half is what
         * the USB driver does and is the same arithmetic, minus its byte swap.
         */
        int32 * src = (int32 *) dd->ba_MixBuffer;
        if (AudioCtrl->ahiac_Flags & AHIACF_STEREO){
            for (i = 0; i < frames; i++){
                uint32 slot = (write + i) % BT_AUDIO_RING_FRAMES;
                ring->bar_Samples[slot * 2]     = scale((int16) (src[i * 2]     >> 16), vol);
                ring->bar_Samples[slot * 2 + 1] = scale((int16) (src[i * 2 + 1] >> 16), vol);
            }
        } else {
            for (i = 0; i < frames; i++){
                uint32 slot = (write + i) % BT_AUDIO_RING_FRAMES;
                int16 sample = scale((int16) (src[i] >> 16), vol);
                ring->bar_Samples[slot * 2]     = sample;
                ring->bar_Samples[slot * 2 + 1] = sample;   /* mono goes to both */
            }
        }
    } else {
        int16 * src = (int16 *) dd->ba_MixBuffer;
        if (AudioCtrl->ahiac_Flags & AHIACF_STEREO){
            for (i = 0; i < frames; i++){
                uint32 slot = (write + i) % BT_AUDIO_RING_FRAMES;
                ring->bar_Samples[slot * 2]     = scale(src[i * 2],     vol);
                ring->bar_Samples[slot * 2 + 1] = scale(src[i * 2 + 1], vol);
            }
        } else {
            for (i = 0; i < frames; i++){
                uint32 slot = (write + i) % BT_AUDIO_RING_FRAMES;
                int16 sample = scale(src[i], vol);
                ring->bar_Samples[slot * 2]     = sample;
                ring->bar_Samples[slot * 2 + 1] = sample;
            }
        }
    }

    /*
     * The samples are in place before the index says so.
     *
     * The consumer reads bar_Write to decide how much it may take, so if that
     * store were seen before the samples themselves it would read whatever the
     * buffer held a moment ago. This is the one ordering the whole lock-free
     * arrangement rests on, which is why it is a barrier and not CacheClearU -
     * the need is for the stores to be ordered, not for a cache to be flushed.
     */
    __sync_synchronize();
    ring->bar_Write = write + frames;

    return frames;
}

/* -------------------------------------------------------------------------- */

void bluetooth_play_proc(void){

    struct Process * self = (struct Process *) IExec->FindTask(NULL);
    struct BluetoothAudioData * dd = (struct BluetoothAudioData *) self->pr_Task.tc_UserData;

    if ((dd == NULL) || (dd->ba_Ring == NULL) || (dd->ba_MixBuffer == NULL)) return;

    dd->ba_PlayRunning = 1;

    /* let AHIsub_Start know it is up, so a failure is a failure of Start */
    if ((dd->ba_Caller != NULL) && (dd->ba_StartSignal != -1)){
        IExec->Signal(dd->ba_Caller, 1UL << dd->ba_StartSignal);
    }

    while (dd->ba_PlayStop == 0){

        uint32 signals = IExec->SetSignal(0, 0);
        if (signals & SIGBREAKF_CTRL_C) break;

        if (ring_used(dd->ba_Ring) >= TARGET_FILL_FRAMES){
            /* far enough ahead: sleep rather than mix audio with nowhere to go */
            IDOS->Delay(IDLE_DELAY_TICKS);
            continue;
        }

        if (mix_into_ring(dd) == 0){
            IDOS->Delay(IDLE_DELAY_TICKS);
        }
    }

    dd->ba_PlayRunning = 0;
}
