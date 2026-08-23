# bluetooth.audio - Bluetooth headphones as a system audio device

An AHI sub-driver that plays through whatever A2DP sink BluetoothService has
connected. With it installed, Bluetooth headphones appear in AHI Prefs as an
audio mode and every program that plays through AHI comes out of them.

Built from the shape of Andrea's USB Audio driver, because an AHI sub-driver has
one right shape: a play process calls AHI's PlayerFunc and MixerFunc and hands
the result somewhere. Only the somewhere differs.

## Building and installing

```
make
make install        # bluetooth.audio to DEVS:AHI/, BLUETOOTH to DEVS:AudioModes/
```

The mode file is what makes AHI aware of the driver at all: AHI reads
`DEVS:AudioModes` and a driver without an entry there is on disk and invisible.

## How the audio gets out

The driver and the stack are separate processes, so PCM crosses a boundary every
few milliseconds. It goes through a ring in shared memory that the service owns
and hands out on request - one producer, one consumer, each touching only its
own index, so neither needs a lock. A message per buffer would have been a
hundred messages a second and would have added its own latency to a path that
already has a codec and a radio in it.

The ring is sized to hold two of AHI's largest mix bursts. That is headroom
rather than latency: the service drains it at exactly the sample rate, so in the
steady state it sits nearly empty however large it is. What it has to absorb is
one uninterruptible mix plus a late run loop turn.

One thing is simpler here than on USB. AHI mixes 16 bit in host byte order and
the SBC encoder wants the same, so on this machine the samples are copied
untouched where the USB driver has to byte-swap every one.

## What it does not do

Recording. A Bluetooth headset's microphone is HFP rather than A2DP, a different
profile with its own codec and its own connection, and it belongs in the input
half of this work.

Rate conversion. A2DP over SBC settles on 44100 in practice and that is the only
rate offered, because offering rates that would be silently resampled is worse
than offering one.

## When nothing comes out

The service must be running and a sink connected - the driver asks it for the
ring by message port, and no service means no audio rather than silence played
into memory nobody reads.

With a sink connected and nothing playing, the service emits a tone. That is
deliberate: it means the chain is working and waiting for something to play,
which is a more useful thing to hear than silence.
