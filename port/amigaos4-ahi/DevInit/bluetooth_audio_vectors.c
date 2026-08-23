/*
 * Interface vector table for bluetooth.audio.
 *
 * The order is AHI's and not ours to choose: the sub-driver interface is
 * positional, so an entry in the wrong place is called for the wrong reason.
 * Taken from the layout idltool generates.
 */

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_EXEC_H
#include <exec/exec.h>
#endif
#ifndef EXEC_INTERFACES_H
#include <exec/interfaces.h>
#endif
#ifndef LIBRARIES_AHI_SUB_H
#include <libraries/ahi_sub.h>
#endif

struct BluetoothAudioIFace;

extern uint32 _btaudio_Obtain(struct BluetoothAudioIFace *);
extern uint32 _btaudio_Release(struct BluetoothAudioIFace *);
extern struct BluetoothAudioIFace * _btaudio_Clone(struct BluetoothAudioIFace *);
extern uint32 _btaudio_AHIsub_AllocAudio(struct BluetoothAudioIFace *, struct TagItem *tagList, struct AHIAudioCtrlDrv *AudioCtrl);
extern void   _btaudio_AHIsub_FreeAudio(struct BluetoothAudioIFace *, struct AHIAudioCtrlDrv *AudioCtrl);
extern void   _btaudio_AHIsub_Disable(struct BluetoothAudioIFace *, struct AHIAudioCtrlDrv *AudioCtrl);
extern void   _btaudio_AHIsub_Enable(struct BluetoothAudioIFace *, struct AHIAudioCtrlDrv *AudioCtrl);
extern uint32 _btaudio_AHIsub_Start(struct BluetoothAudioIFace *, uint32 Flags, struct AHIAudioCtrlDrv *AudioCtrl);
extern uint32 _btaudio_AHIsub_Update(struct BluetoothAudioIFace *, uint32 Flags, struct AHIAudioCtrlDrv *AudioCtrl);
extern uint32 _btaudio_AHIsub_Stop(struct BluetoothAudioIFace *, uint32 Flags, struct AHIAudioCtrlDrv *AudioCtrl);
extern uint32 _btaudio_AHIsub_SetVol(struct BluetoothAudioIFace *, uint16 Channel, Fixed Volume, sposition Pan, struct AHIAudioCtrlDrv *AudioCtrl, uint32 Flags);
extern uint32 _btaudio_AHIsub_SetFreq(struct BluetoothAudioIFace *, uint16 Channel, uint32 Freq, struct AHIAudioCtrlDrv *AudioCtrl, uint32 Flags);
extern uint32 _btaudio_AHIsub_SetSound(struct BluetoothAudioIFace *, uint16 Channel, uint16 Sound, uint32 Offset, int32 Length, struct AHIAudioCtrlDrv *AudioCtrl, uint32 Flags);
extern uint32 _btaudio_AHIsub_SetEffect(struct BluetoothAudioIFace *, APTR Effect, struct AHIAudioCtrlDrv *AudioCtrl);
extern uint32 _btaudio_AHIsub_LoadSound(struct BluetoothAudioIFace *, uint16 Sound, uint32 Type, APTR Info, struct AHIAudioCtrlDrv *AudioCtrl);
extern uint32 _btaudio_AHIsub_UnloadSound(struct BluetoothAudioIFace *, uint16 Sound, struct AHIAudioCtrlDrv *AudioCtrl);
extern int32  _btaudio_AHIsub_GetAttr(struct BluetoothAudioIFace *, uint32 Attribute, int32 Argument, int32 DefValue, struct TagItem *tagList, struct AHIAudioCtrlDrv *AudioCtrl);
extern int32  _btaudio_AHIsub_HardwareControl(struct BluetoothAudioIFace *, uint32 Attribute, int32 Argument, struct AHIAudioCtrlDrv *AudioCtrl);

STATIC CONST APTR main_vectors[] =
{
    _btaudio_Obtain,
    _btaudio_Release,
    NULL,
    _btaudio_Clone,
    _btaudio_AHIsub_AllocAudio,
    _btaudio_AHIsub_FreeAudio,
    _btaudio_AHIsub_Disable,
    _btaudio_AHIsub_Enable,
    _btaudio_AHIsub_Start,
    _btaudio_AHIsub_Update,
    _btaudio_AHIsub_Stop,
    _btaudio_AHIsub_SetVol,
    _btaudio_AHIsub_SetFreq,
    _btaudio_AHIsub_SetSound,
    _btaudio_AHIsub_SetEffect,
    _btaudio_AHIsub_LoadSound,
    _btaudio_AHIsub_UnloadSound,
    _btaudio_AHIsub_GetAttr,
    _btaudio_AHIsub_HardwareControl,
    (APTR)-1
};
