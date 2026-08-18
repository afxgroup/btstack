# bt.usbfd - Bluetooth hotplug for AmigaOS 4

Makes a Bluetooth USB dongle work like any other peripheral: plug it in, and
paired devices start working. This folder holds the USB function driver; the
service and the GUI are separate programs that live next to it.

## The pieces

```
  dongle plugged in
        |
        v
  [ USB stack ]  matches class 0xE0 / subclass 0x01 / protocol 0x01
        |
        v
  [ bt.usbfd ]   this folder - starts the service, then gets out of the way
        |
        | PutMsg to "bluetooth.service"
        v
  [ BluetoothService ]  one process, owns BTstack and the controller
        |                 - keeps bonding keys, reconnects known devices
        |                 - dispatches each connected device to a handler
        |
        +--> [ HID handler ]     mouse/keyboard -> input.device
        +--> [ audio handler ]   later
        +--> [ serial handler ]  later
        ^
        | BTServiceMsg on a public MsgPort
        |
  [ GUI ]  later - scan, list, pair, connect, disconnect
```

`bluetooth_service.h` defines the protocol between all of them, and is the
only thing they share.

## Why bt.usbfd does so little

A function driver runs inside the USB stack's context and its lifetime is tied
to the device. A Bluetooth stack needs the opposite: it owns a run loop, keeps
bonding keys across re-plugs, holds connections, and talks to `input.device`.
So the driver only ensures the service is running and notifies it - the same
shape as `usbaudio.usbfd`, which just triggers a refresh.

Two constraints worth writing down, because getting them wrong is expensive:

- **The driver must not claim the controller's interfaces.** The service
  reaches the dongle through `libusb-1.library`, which claims interface 0
  itself. `usbaudio.usbfd` claims an interface to get detach notifications;
  doing the same here has to be checked against `USBA_SeeClaimed` first, or the
  service is locked out of its own dongle. Until that is verified, detach is
  noticed by the service through its USB errors. See the TODO in `fdmain.c`.
- **The service must never call anything that needs Intuition.** It injects
  input events, and Intuition can be blocked waiting for those - dragging a
  window is enough. A `printf()` to a console window from the service
  deadlocks the machine: the console handler waits for Intuition, Intuition
  waits for the button release, and only the service can deliver it. This is
  not theoretical, it cost several debugging rounds in `port/amigaos4-libusb`.
  Diagnostics go to the serial debug output.

## The service

Not written yet. It is `port/amigaos4-libusb` turned into a daemon:

- BTstack with the AmigaOS run loop (`btstack_run_loop_amigaos.c`) and the
  libusb transport, both already working
- a public `MsgPort` named `bluetooth.service`; the run loop already supports
  waiting on extra ports through `btstack_run_loop_amigaos_add_signal_mask()`,
  so serving clients is a data source like any other
- bonding keys in TLV under `ENVARC:Bluetooth/`, so pairings survive a reboot
- a table of profile handlers with a small vtable (probe / connect /
  disconnect / report), so adding audio or serial later does not touch the
  core. `bthid.c` is the first handler, already written and working.

Suggested startup order for a user who has none of this running: plugging the
dongle in starts the service through `bt.usbfd`. A `SYS:WBStartup` entry is a
reasonable alternative for a dongle that is never unplugged, and the service
must handle both, hence `BTCMD_PING` and the "is it already running" check.

## Stopping the service

The service handles `SIGBREAKF_CTRL_C`, so it stops from Ranger or any task
tool with a Break/CTRL-C, and from a shell with CTRL-C. The first break powers
Bluetooth off cleanly - handlers release their devices, held mouse buttons are
released - and the process exits when the controller reports itself off; a
second break forces the run loop out. `BTCMD_SHUTDOWN` on the public port does
the same thing and is what the GUI will use.

## Build

```bash
make
make install     # copies to DEVS:USB/fdclasses/
```

`bt.fdclass` is the descriptor the USB stack reads to know which driver to load
for a device; both files belong in `DEVS:USB/fdclasses/`.
