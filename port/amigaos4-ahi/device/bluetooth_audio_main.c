/*
 * The AHI sub-driver entry points for bluetooth.audio.
 *
 * Much shorter than the USB Audio driver's equivalent, and for a good reason:
 * that one has a device to find, endpoints to pick, alternate settings to
 * negotiate and a sample rate to talk the hardware into. Here the device is
 * already connected, by the service, before AHI is ever opened - so all that is
 * left is to ask for the ring and start a process that fills it.
 */

#include "bluetooth_audio.h"

#include <devices/ahi.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <string.h>

/*
 * No libc here: the library is linked -nostartfiles, the newlib CRT never runs
 * and INewlib stays NULL, so a call to memset would jump through a null
 * interface and take down ahi.device's unit process. exec and utility provide
 * the equivalents, and they are already open.
 */

extern struct ExecIFace    * IExec;
extern struct DOSIFace     * IDOS;
extern struct UtilityIFace * IUtility;

struct BluetoothAudioIFace;

/*
 * We only offer what the service negotiated with the sink, and A2DP over SBC
 * settles on 44100 in practice. Offering rates we would silently resample is
 * worse than offering one.
 */
#define BT_AUDIO_FREQUENCY   44100

/*
 * The most AHI may mix in one uninterruptible burst.
 *
 * Taken from the USB Audio driver, where the reasoning is written out: this is
 * the longest stall the rest of the pipeline has to absorb, and at 16384 frames
 * a single mix call was long enough to click under load. The ring is twice this
 * for exactly that reason.
 */
#define BT_MAX_PLAY_SAMPLES  4096

/* -------------------------------------------------------------------------- */

/*
 * Ask BluetoothService for the ring.
 *
 * The service owns it, so it survives this driver being opened and closed
 * repeatedly, and a driver that dies without saying so leaves nothing dangling.
 * No service means no audio, and saying so is better than playing into memory
 * nobody reads.
 */
BTAudioRing * bluetooth_audio_ring_open(void){
    struct MsgPort * reply = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (reply == NULL) return NULL;

    BTServiceMsg msg;
    IUtility->ClearMem(&msg, sizeof(msg));
    msg.bsm_Message.mn_Node.ln_Type = NT_MESSAGE;
    msg.bsm_Message.mn_Length       = sizeof(msg);
    msg.bsm_Message.mn_ReplyPort    = reply;
    msg.bsm_Version                 = BLUETOOTH_SERVICE_VERSION;
    msg.bsm_Command                 = BTCMD_AUDIO_OPEN;

    IExec->Forbid();
    struct MsgPort * service = IExec->FindPort(BLUETOOTH_SERVICE_PORT_NAME);
    if (service != NULL){
        IExec->PutMsg(service, &msg.bsm_Message);
    }
    IExec->Permit();

    BTAudioRing * ring = NULL;
    if (service != NULL){
        IExec->WaitPort(reply);
        while (IExec->GetMsg(reply) == NULL){
            IExec->WaitPort(reply);
        }
        if (msg.bsm_Result == BT_RESULT_OK) ring = msg.bsm_AudioRing;
    }

    IExec->FreeSysObject(ASOT_PORT, reply);
    return ring;
}

void bluetooth_audio_ring_close(void){
    struct MsgPort * reply = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (reply == NULL) return;

    BTServiceMsg msg;
    IUtility->ClearMem(&msg, sizeof(msg));
    msg.bsm_Message.mn_Node.ln_Type = NT_MESSAGE;
    msg.bsm_Message.mn_Length       = sizeof(msg);
    msg.bsm_Message.mn_ReplyPort    = reply;
    msg.bsm_Version                 = BLUETOOTH_SERVICE_VERSION;
    msg.bsm_Command                 = BTCMD_AUDIO_CLOSE;

    IExec->Forbid();
    struct MsgPort * service = IExec->FindPort(BLUETOOTH_SERVICE_PORT_NAME);
    if (service != NULL){
        IExec->PutMsg(service, &msg.bsm_Message);
    }
    IExec->Permit();

    if (service != NULL){
        IExec->WaitPort(reply);
        while (IExec->GetMsg(reply) == NULL){
            IExec->WaitPort(reply);
        }
    }
    IExec->FreeSysObject(ASOT_PORT, reply);
}

/* -------------------------------------------------------------------------- */

/*
 * Trace to serial. AHI reports nothing when it declines a driver, so the only
 * way to tell which call it stops at is to have every entry point say so.
 */
#define BTA_LOG(...) IExec->DebugPrintF("[bluetooth.audio] " __VA_ARGS__)

uint32 _btaudio_AHIsub_AllocAudio(struct BluetoothAudioIFace * Self,
                                             struct TagItem * tagList,
                                             struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;
    (void) tagList;

    /*
     * What AHI is asking for. It decides whether the driver is usable inside
     * AHI_AllocAudioA, after this returns, and says nothing when it declines -
     * so the request itself is the only evidence of what it disliked.
     */
    BTA_LOG("AHIsub_AllocAudio: mixfreq %lu ch %u snd %u flags 0x%lx\n",
            (unsigned long) AudioCtrl->ahiac_MixFreq,
            (unsigned) AudioCtrl->ahiac_Channels,
            (unsigned) AudioCtrl->ahiac_Sounds,
            (unsigned long) AudioCtrl->ahiac_Flags);
    BTA_LOG("  playerfreq %ld (min %ld max %ld) bufftype 0x%lx buffsize %lu\n",
            (long) AudioCtrl->ahiac_PlayerFreq,
            (long) AudioCtrl->ahiac_MinPlayerFreq,
            (long) AudioCtrl->ahiac_MaxPlayerFreq,
            (unsigned long) AudioCtrl->ahiac_BuffType,
            (unsigned long) AudioCtrl->ahiac_BuffSize);
    BTA_LOG("  playerfunc %p mixerfunc %p\n",
            (void *) AudioCtrl->ahiac_PlayerFunc,
            (void *) AudioCtrl->ahiac_MixerFunc);

    struct BluetoothAudioData * dd =
        IExec->AllocVecTags(sizeof(struct BluetoothAudioData),
                            AVT_Type,           MEMF_SHARED,
                            AVT_ClearWithValue, 0,
                            TAG_END);
    if (dd == NULL) return AHISF_ERROR;

    /*
     * The autodocs require the driver to move ahiac_MixFreq to the nearest rate
     * it can actually play. There is only one: the encoder is fixed at 44100
     * and the sink agreed to it. Accepting whatever AHI asked for would mix at
     * one rate and play at another, which is wrong pitch rather than an error.
     */
    if (AudioCtrl->ahiac_MixFreq != BT_AUDIO_FREQUENCY){
        BTA_LOG("snapping mixfreq %lu -> %lu\n",
                (unsigned long) AudioCtrl->ahiac_MixFreq,
                (unsigned long) BT_AUDIO_FREQUENCY);
        AudioCtrl->ahiac_MixFreq = BT_AUDIO_FREQUENCY;
    }

    BTA_LOG("AllocAudio: storing dd %p in AudioCtrl %p\n", (void *) dd, (void *) AudioCtrl);

    dd->AudioCtrl              = AudioCtrl;
    dd->ba_OutputVolume        = 0x10000;   /* unity until AHI says otherwise */
    dd->ba_MonitorVolume       = 0x00000;
    dd->ba_InputGain           = 0x10000;
    AudioCtrl->ahiac_DriverData = dd;

    /*
     * AHISF_TIMING says this driver paces playback itself, which it does: the
     * ring is drained by the service at the rate the stream negotiated, and the
     * play process fills it to match. AHISF_MIXING says AHI should do the
     * mixing, which is the whole point of using its mixer.
     */
    return AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO | AHISF_KNOWHIFI;
}

void _btaudio_AHIsub_FreeAudio(struct BluetoothAudioIFace * Self,
                                          struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    BTA_LOG("AHIsub_FreeAudio\n");

    struct BluetoothAudioData * dd = (struct BluetoothAudioData *) AudioCtrl->ahiac_DriverData;
    if (dd == NULL) return;

    IExec->FreeVec(dd);
    AudioCtrl->ahiac_DriverData = NULL;
}

uint32 _btaudio_AHIsub_Start(struct BluetoothAudioIFace * Self, uint32 Flags,
                                        struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    struct BluetoothAudioData * dd = (struct BluetoothAudioData *) AudioCtrl->ahiac_DriverData;

    /* Trace first, and unconditionally: an entry point that reports only after
     * its guards cannot show a call that the guards reject. */
    BTA_LOG("AHIsub_Start, flags 0x%lx, dd %p\n", (unsigned long) Flags, (void *) dd);

    if (dd == NULL) return AHISF_ERROR;

    /* recording is the other half of the audio work and is not this driver */
    if ((Flags & AHISF_PLAY) == 0) return AHISF_ERROR;

    dd->ba_Ring = bluetooth_audio_ring_open();
    if (dd->ba_Ring == NULL){
        BTA_LOG("no ring: is BluetoothService running?\n");
        return AHISF_ERROR;
    }

    dd->ba_MixBuffer = IExec->AllocVecTags(AudioCtrl->ahiac_BuffSize,
                                           AVT_Type,           MEMF_SHARED,
                                           AVT_ClearWithValue, 0,
                                           TAG_END);
    if (dd->ba_MixBuffer == NULL){
        bluetooth_audio_ring_close();
        dd->ba_Ring = NULL;
        return AHISF_ERROR;
    }

    /*
     * The play process is told where to find everything through the driver
     * data, and signals back when it is running - so a failure to start is a
     * failure of AHIsub_Start rather than silence discovered later.
     */
    dd->ba_Caller      = IExec->FindTask(NULL);
    dd->ba_StartSignal = IExec->AllocSignal(-1);
    dd->ba_PlayStop    = 0;
    dd->ba_PlayRunning = 0;

    dd->ba_PlayProc = IDOS->CreateNewProcTags(
        NP_Entry,      bluetooth_play_proc,
        NP_Name,       "Bluetooth Audio",
        NP_Priority,   50,
        NP_StackSize,  16384,
        NP_Child,      FALSE,
        NP_UserData,   dd,
        TAG_END);

    if (dd->ba_PlayProc == NULL){
        if (dd->ba_StartSignal != -1) IExec->FreeSignal(dd->ba_StartSignal);
        IExec->FreeVec(dd->ba_MixBuffer);
        dd->ba_MixBuffer = NULL;
        bluetooth_audio_ring_close();
        dd->ba_Ring = NULL;
        return AHISF_ERROR;
    }

    if (dd->ba_StartSignal != -1){
        IExec->Wait(1UL << dd->ba_StartSignal);
        IExec->FreeSignal(dd->ba_StartSignal);
        dd->ba_StartSignal = -1;
    }

    return 0;
}

uint32 _btaudio_AHIsub_Stop(struct BluetoothAudioIFace * Self, uint32 Flags,
                                       struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    BTA_LOG("AHIsub_Stop, flags 0x%lx, ctrl %p dd %p\n", (unsigned long) Flags,
            (void *) AudioCtrl,
            (void *) ((AudioCtrl != NULL) ? AudioCtrl->ahiac_DriverData : NULL));
    (void) Flags;

    struct BluetoothAudioData * dd = (struct BluetoothAudioData *) AudioCtrl->ahiac_DriverData;
    if (dd == NULL) return AHISF_ERROR;

    if (dd->ba_PlayProc != NULL){
        /* ask, then wait: the process has a mix in hand and dropping it mid-way
         * would leave AHI's mixer state half advanced */
        dd->ba_PlayStop = 1;
        IExec->Signal((struct Task *) dd->ba_PlayProc, SIGBREAKF_CTRL_C);
        while (dd->ba_PlayRunning != 0){
            IDOS->Delay(1);
        }
        dd->ba_PlayProc = NULL;
    }

    if (dd->ba_MixBuffer != NULL){
        IExec->FreeVec(dd->ba_MixBuffer);
        dd->ba_MixBuffer = NULL;
    }
    if (dd->ba_Ring != NULL){
        bluetooth_audio_ring_close();
        dd->ba_Ring = NULL;
    }
    return 0;
}

uint32 _btaudio_AHIsub_Update(struct BluetoothAudioIFace * Self, uint32 Flags,
                                         struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    BTA_LOG("AHIsub_Update\n"); (void) Flags; (void) AudioCtrl;
    return 0;
}

void _btaudio_AHIsub_Disable(struct BluetoothAudioIFace * Self,
                                        struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    BTA_LOG("AHIsub_Disable\n"); (void) AudioCtrl;
    IExec->Forbid();
}

void _btaudio_AHIsub_Enable(struct BluetoothAudioIFace * Self,
                                       struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    BTA_LOG("AHIsub_Enable\n"); (void) AudioCtrl;
    IExec->Permit();
}

/*
 * SetVol, SetFreq, SetSound, SetEffect, LoadSound and UnloadSound.
 *
 * A driver that sets AHISB_MIXING gets these first, and ahi.device only handles
 * them itself if the driver answers AHIS_UNKNOWN. Any other answer claims the
 * work - so returning an error code here does not report a failure, it asserts
 * that the driver took care of it. This driver does none of it: AHI mixes.
 */
/*
 * The channel calls belong to a driver that mixes in hardware. AHI does the
 * mixing here, so it never asks - and answering "not supported" is the correct
 * answer rather than a gap.
 */
uint32 _btaudio_AHIsub_SetVol(struct BluetoothAudioIFace * Self, uint16 Channel,
                                         Fixed Volume, sposition Pan,
                                         struct AHIAudioCtrlDrv * AudioCtrl, uint32 Flags){
    (void) Self; (void) Channel; (void) Volume; (void) Pan; (void) AudioCtrl; (void) Flags;
    return AHIS_UNKNOWN;
}

uint32 _btaudio_AHIsub_SetFreq(struct BluetoothAudioIFace * Self, uint16 Channel,
                                          uint32 Freq, struct AHIAudioCtrlDrv * AudioCtrl,
                                          uint32 Flags){
    (void) Self; (void) Channel; (void) Freq; (void) AudioCtrl; (void) Flags;
    return AHIS_UNKNOWN;
}

uint32 _btaudio_AHIsub_SetSound(struct BluetoothAudioIFace * Self, uint16 Channel,
                                           uint16 Sound, uint32 Offset, int32 Length,
                                           struct AHIAudioCtrlDrv * AudioCtrl, uint32 Flags){
    (void) Self; (void) Channel; (void) Sound; (void) Offset; (void) Length;
    (void) AudioCtrl; (void) Flags;
    return AHIS_UNKNOWN;
}

uint32 _btaudio_AHIsub_SetEffect(struct BluetoothAudioIFace * Self, APTR Effect,
                                            struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Effect; (void) AudioCtrl;
    return AHIS_UNKNOWN;
}

uint32 _btaudio_AHIsub_LoadSound(struct BluetoothAudioIFace * Self, uint16 Sound,
                                            uint32 Type, APTR Info,
                                            struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Sound; (void) Type; (void) Info; (void) AudioCtrl;
    return AHIS_UNKNOWN;
}

uint32 _btaudio_AHIsub_UnloadSound(struct BluetoothAudioIFace * Self, uint16 Sound,
                                              struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Sound; (void) AudioCtrl;
    return AHIS_UNKNOWN;
}

int32 _btaudio_AHIsub_HardwareControl(struct BluetoothAudioIFace * Self,
                                                 uint32 Attribute, int32 Argument,
                                                 struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    struct BluetoothAudioData * dd =
        (AudioCtrl != NULL) ? (struct BluetoothAudioData *) AudioCtrl->ahiac_DriverData : NULL;

    BTA_LOG("AHIsub_HardwareControl 0x%lx (arg %ld)\n",
            (unsigned long) Attribute, (long) Argument);

    if (dd == NULL) return FALSE;

    /*
     * A set answers TRUE, a query answers the value. Answering FALSE to a set
     * tells AHI the control failed, and AHI then gives up on the driver
     * altogether - which is why every one of these has to be handled, even the
     * ones that have nothing behind them.
     */
    switch (Attribute){
        case AHIC_MixFreq_Query:        return (int32) AudioCtrl->ahiac_MixFreq;

        case AHIC_OutputVolume:         dd->ba_OutputVolume  = (Fixed) Argument; return TRUE;
        case AHIC_OutputVolume_Query:   return (int32) dd->ba_OutputVolume;

        case AHIC_MonitorVolume:        dd->ba_MonitorVolume = (Fixed) Argument; return TRUE;
        case AHIC_MonitorVolume_Query:  return (int32) dd->ba_MonitorVolume;

        case AHIC_InputGain:            dd->ba_InputGain     = (Fixed) Argument; return TRUE;
        case AHIC_InputGain_Query:      return (int32) dd->ba_InputGain;

        case AHIC_Input:                dd->ba_Input  = (ULONG) Argument; return TRUE;
        case AHIC_Input_Query:          return (int32) dd->ba_Input;

        case AHIC_Output:               dd->ba_Output = (ULONG) Argument; return TRUE;
        case AHIC_Output_Query:         return (int32) dd->ba_Output;

        default:                        return FALSE;
    }
}

int32 _btaudio_AHIsub_GetAttr(struct BluetoothAudioIFace * Self, uint32 Attribute,
                                         int32 Argument, int32 DefValue,
                                         struct TagItem * tagList,
                                         struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) tagList; (void) AudioCtrl;

    BTA_LOG("AHIsub_GetAttr 0x%lx (arg %ld)\n",
            (unsigned long) Attribute, (long) Argument);

    switch (Attribute){
        case AHIDB_Bits:            return 16;

        /*
         * One frequency only: the SBC encoder is configured for 44100 and the
         * sink agreed to it, so there is nothing to choose between.
         */
        case AHIDB_Frequencies:     return 1;
        case AHIDB_MinMixFreq:      return BT_AUDIO_FREQUENCY;
        case AHIDB_MaxMixFreq:      return BT_AUDIO_FREQUENCY;
        case AHIDB_Frequency:       return BT_AUDIO_FREQUENCY;
        case AHIDB_Index:           return 0;    /* one frequency, so always it */
        case AHIDB_Annotation:      return (int32) "A2DP audio over Bluetooth";
        case AHIDB_Author:          return (int32) "Andrea Palmate";
        case AHIDB_Copyright:       return (int32) "GPL";
        case AHIDB_Version:         return (int32) VSTRING;
        case AHIDB_Record:          return FALSE;
        case AHIDB_FullDuplex:      return FALSE;
        case AHIDB_Realtime:        return TRUE;
        case AHIDB_MaxPlaySamples:  return BT_MAX_PLAY_SAMPLES;
        case AHIDB_MaxChannels:     return DefValue;   /* AHI mixes: its call */
        case AHIDB_Stereo:          return TRUE;
        case AHIDB_Panning:         return TRUE;
        case AHIDB_Volume:          return TRUE;
        case AHIDB_HiFi:            return TRUE;
        case AHIDB_MultiChannel:    return FALSE;

        /*
         * Outputs must be at least one. AHI reads the count first and then asks
         * for each name as a string, so leaving either to DefValue hands it a
         * zero where it expects a pointer.
         */
        case AHIDB_Outputs:         return 1;
        case AHIDB_Output:          return (int32) "Bluetooth";
        case AHIDB_Inputs:          return 0;    /* playback only, for now */

        /* Volume ranges are Fixed: 0x10000 is unity gain. */
        case AHIDB_MinOutputVolume:  return 0x00000;
        case AHIDB_MaxOutputVolume:  return 0x10000;
        case AHIDB_MinMonitorVolume: return 0x00000;
        case AHIDB_MaxMonitorVolume: return 0x00000;   /* nothing to monitor */
        case AHIDB_MinInputGain:     return 0x10000;
        case AHIDB_MaxInputGain:     return 0x10000;
        default:
            (void) Argument;
            return DefValue;
    }
}
