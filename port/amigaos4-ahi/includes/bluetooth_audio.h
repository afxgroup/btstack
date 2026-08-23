/*
 * bluetooth.audio - AHI sub-driver that plays through a Bluetooth sink
 *
 * The Amiga side of A2DP output. AHI mixes into a buffer as it does for any
 * other driver; this one hands the result to BluetoothService, which encodes it
 * to SBC and sends it to whatever headphones or speaker is connected.
 *
 * Built from the shape of the USB Audio driver, because the shape is the same:
 * a play process calls AHI's PlayerFunc and MixerFunc and then gives the
 * samples to something. Only the something differs, and here it is a ring in
 * shared memory instead of an isochronous USB pipe.
 *
 * One thing that gets simpler. AHI mixes 16 bit big-endian and the SBC encoder
 * wants host order, so on this machine the samples are handed over untouched -
 * where the USB driver has to byte-swap every one of them.
 */

#ifndef BLUETOOTH_AUDIO_H
#define BLUETOOTH_AUDIO_H

#include <exec/types.h>
#include <exec/exec.h>
#include <exec/interfaces.h>
#include <libraries/ahi_sub.h>
#include <dos/dos.h>

#include "bluetooth.audio_rev.h"

/* the protocol shared with the service, and the ring itself */
#include "bluetooth_service.h"

struct BluetoothAudioBase
{
    struct Library     libNode;
    BPTR               segList;
    struct ExecIFace * iexec;
};

/*
 * Per-AllocAudio state.
 *
 * Hung off ahiac_DriverData, which AHI keeps for the driver's own use.
 */
struct BluetoothAudioData
{
    struct AHIAudioCtrlDrv * AudioCtrl;

    struct Process * ba_PlayProc;
    struct Task    * ba_Caller;      /* who to signal when the process is up */
    ULONG            ba_StartSignal;
    volatile LONG    ba_PlayRunning;
    volatile LONG    ba_PlayStop;

    APTR             ba_MixBuffer;   /* AHI mixes here */
    BTAudioRing    * ba_Ring;        /* and it ends up here */
};

/* the play process, in hw/ */
extern void bluetooth_play_proc(void);

/* asking the service for the ring, in device/ */
BTAudioRing * bluetooth_audio_ring_open(void);
void          bluetooth_audio_ring_close(void);

#endif /* BLUETOOTH_AUDIO_H */
