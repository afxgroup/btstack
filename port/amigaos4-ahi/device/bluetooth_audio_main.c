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
    memset(&msg, 0, sizeof(msg));
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
    memset(&msg, 0, sizeof(msg));
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

uint32 VARARGS68K _btaudio_AHIsub_AllocAudio(struct BluetoothAudioIFace * Self,
                                             struct TagItem * tagList,
                                             struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;
    (void) tagList;

    struct BluetoothAudioData * dd =
        IExec->AllocVecTags(sizeof(struct BluetoothAudioData),
                            AVT_Type,           MEMF_SHARED,
                            AVT_ClearWithValue, 0,
                            TAG_END);
    if (dd == NULL) return AHISF_ERROR;

    dd->AudioCtrl              = AudioCtrl;
    AudioCtrl->ahiac_DriverData = dd;

    /*
     * AHISF_TIMING says this driver paces playback itself, which it does: the
     * ring is drained by the service at the rate the stream negotiated, and the
     * play process fills it to match. AHISF_MIXING says AHI should do the
     * mixing, which is the whole point of using its mixer.
     */
    return AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO;
}

void VARARGS68K _btaudio_AHIsub_FreeAudio(struct BluetoothAudioIFace * Self,
                                          struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    struct BluetoothAudioData * dd = (struct BluetoothAudioData *) AudioCtrl->ahiac_DriverData;
    if (dd == NULL) return;

    IExec->FreeVec(dd);
    AudioCtrl->ahiac_DriverData = NULL;
}

uint32 VARARGS68K _btaudio_AHIsub_Start(struct BluetoothAudioIFace * Self, uint32 Flags,
                                        struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;

    struct BluetoothAudioData * dd = (struct BluetoothAudioData *) AudioCtrl->ahiac_DriverData;
    if (dd == NULL) return AHISF_ERROR;

    /* recording is the other half of the audio work and is not this driver */
    if ((Flags & AHISF_PLAY) == 0) return AHISF_ERROR;

    dd->ba_Ring = bluetooth_audio_ring_open();
    if (dd->ba_Ring == NULL) return AHISF_ERROR;

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

uint32 VARARGS68K _btaudio_AHIsub_Stop(struct BluetoothAudioIFace * Self, uint32 Flags,
                                       struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self;
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

uint32 VARARGS68K _btaudio_AHIsub_Update(struct BluetoothAudioIFace * Self, uint32 Flags,
                                         struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Flags; (void) AudioCtrl;
    return 0;
}

void VARARGS68K _btaudio_AHIsub_Disable(struct BluetoothAudioIFace * Self,
                                        struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) AudioCtrl;
    IExec->Forbid();
}

void VARARGS68K _btaudio_AHIsub_Enable(struct BluetoothAudioIFace * Self,
                                       struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) AudioCtrl;
    IExec->Permit();
}

/*
 * The channel calls belong to a driver that mixes in hardware. AHI does the
 * mixing here, so it never asks - and answering "not supported" is the correct
 * answer rather than a gap.
 */
uint32 VARARGS68K _btaudio_AHIsub_SetVol(struct BluetoothAudioIFace * Self, uint16 Channel,
                                         Fixed Volume, sposition Pan,
                                         struct AHIAudioCtrlDrv * AudioCtrl, uint32 Flags){
    (void) Self; (void) Channel; (void) Volume; (void) Pan; (void) AudioCtrl; (void) Flags;
    return AHISF_ERROR;
}

uint32 VARARGS68K _btaudio_AHIsub_SetFreq(struct BluetoothAudioIFace * Self, uint16 Channel,
                                          uint32 Freq, struct AHIAudioCtrlDrv * AudioCtrl,
                                          uint32 Flags){
    (void) Self; (void) Channel; (void) Freq; (void) AudioCtrl; (void) Flags;
    return AHISF_ERROR;
}

uint32 VARARGS68K _btaudio_AHIsub_SetSound(struct BluetoothAudioIFace * Self, uint16 Channel,
                                           uint16 Sound, uint32 Offset, int32 Length,
                                           struct AHIAudioCtrlDrv * AudioCtrl, uint32 Flags){
    (void) Self; (void) Channel; (void) Sound; (void) Offset; (void) Length;
    (void) AudioCtrl; (void) Flags;
    return AHISF_ERROR;
}

uint32 VARARGS68K _btaudio_AHIsub_SetEffect(struct BluetoothAudioIFace * Self, APTR Effect,
                                            struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Effect; (void) AudioCtrl;
    return AHISF_ERROR;
}

uint32 VARARGS68K _btaudio_AHIsub_LoadSound(struct BluetoothAudioIFace * Self, uint16 Sound,
                                            uint32 Type, APTR Info,
                                            struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Sound; (void) Type; (void) Info; (void) AudioCtrl;
    return AHISF_ERROR;
}

uint32 VARARGS68K _btaudio_AHIsub_UnloadSound(struct BluetoothAudioIFace * Self, uint16 Sound,
                                              struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Sound; (void) AudioCtrl;
    return AHISF_ERROR;
}

int32 VARARGS68K _btaudio_AHIsub_HardwareControl(struct BluetoothAudioIFace * Self,
                                                 uint32 Attribute, int32 Argument,
                                                 struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) Attribute; (void) Argument; (void) AudioCtrl;
    return 0;
}

int32 VARARGS68K _btaudio_AHIsub_GetAttr(struct BluetoothAudioIFace * Self, uint32 Attribute,
                                         int32 Argument, int32 DefValue,
                                         struct TagItem * tagList,
                                         struct AHIAudioCtrlDrv * AudioCtrl){
    (void) Self; (void) tagList; (void) AudioCtrl;

    switch (Attribute){
        case AHIDB_Bits:            return 16;
        case AHIDB_Frequencies:     return 1;
        case AHIDB_Frequency:       return BT_AUDIO_FREQUENCY;
        case AHIDB_Index:           return 0;    /* one frequency, so always it */
        case AHIDB_Author:          return (int32) "Andrea Palmate";
        case AHIDB_Copyright:       return (int32) "GPL";
        case AHIDB_Version:         return (int32) VSTRING;
        case AHIDB_Record:          return FALSE;
        case AHIDB_FullDuplex:      return FALSE;
        case AHIDB_Realtime:        return TRUE;
        case AHIDB_MaxPlaySamples:  return BT_MAX_PLAY_SAMPLES;
        case AHIDB_MaxChannels:     return 2;
        case AHIDB_Stereo:          return TRUE;
        case AHIDB_Panning:         return TRUE;
        case AHIDB_Volume:          return TRUE;
        case AHIDB_HiFi:            return TRUE;
        default:
            (void) Argument;
            return DefValue;
    }
}
