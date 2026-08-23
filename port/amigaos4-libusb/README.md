# BTstack port for AmigaOS 4 (libusb-1)

This port targets AmigaOS 4 using the `libusb-1.library` wrapper and the
`clib4` C runtime.

## Requirements

* AmigaOS 4 cross toolchain with `ppc-amigaos-gcc`
* `clib4` (`-mcrt=clib4`)
* AmigaOS 4 `libusb-1.library` SDK / stubs (provided in `libusb-1/`)

## Build

```bash
cd port/amigaos4-libusb
mkdir -p build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=/usr/ppc-amigaos/bin/ppc-amigaos.cmake ..
make -j4
```

The toolchain file already sets `-mcrt=clib4` and the required pthread
flags. The CMake build automatically uses the local `libusb-1` headers
and stubs from the workspace.

## Status

* Cross-compiles successfully for `ppc-amigaos`.
* Uses the synchronous libusb-1.library API (AmigaOS 4 libusb wrapper does
  not expose asynchronous transfers).
* HCI command and ACL data paths are implemented using a run-loop polling
  timer plus a `DATA_SOURCE_CALLBACK_POLL` data source.
* ACL packets are sent with the async API, not `libusb_bulk_transfer()`:
  the synchronous call blocks the run loop, and every other call only
  returns after its full timeout, which added 2 s to every second ACL
  transaction. `usb: async ACL out not available` in the log means the
  library rejected the OUT submit and the sync fallback is in use.
* Uses its own run loop, `btstack_run_loop_amigaos.c`, based on
  `timer.device` + `Wait()`. The posix run loop cannot be used: clib4's
  `select()` does not honour its timeout when there is no valid descriptor
  (and its wake-up pipes do not work either), so it executed a single
  overdue timer and then blocked for ever.
* CTRL-C is handled natively via `SIGBREAKF_CTRL_C` in the run loop: the
  first one powers Bluetooth off and shuts down cleanly, a second one
  forces the run loop to exit.
* Console input (`HAVE_BTSTACK_STDIN`) is provided by
  `btstack_stdin_amigaos.c`: the console is switched to RAW mode and polled
  with `WaitForChar()` from a run loop timer, so the interactive examples
  work. `btstack_stdin_posix.c`, `btstack_run_loop_posix.c` and
  `btstack_signal.c` are excluded from the build.
* SCO / audio-over-HCI is not yet supported.
* USB path selection (`-u`) is not supported; the first matching VID/PID
  from the known-device list is used.
* Network (PAN/BNEP) and audio examples are excluded.

## Testing with Realtek dongles

Realtek controllers need a firmware and a config file to be downloaded at
startup. BTstack looks for **uncompressed** files with the *exact* names used
by the chipset driver (no `.bin`, no `.zst` suffix). For the `0bda:8771`
(RTL8761BU) dongle these are:

```
rtl8761bu_fw
rtl8761bu_config
```

So the Linux files have to be decompressed and renamed, e.g. on Linux:

```bash
zstd -d rtl8761bu_fw.bin.zst -o rtl8761bu_fw
zstd -d rtl8761bu_config.bin.zst -o rtl8761bu_config
```

The `firmware/` folder here already holds them, extracted from `linux-firmware`
and named the way BTstack asks for them - copy it to `SYS:Firmware/bt` and a
Realtek dongle comes up without any of the above. See `firmware/README.md` for
which parts are covered and which are not.

They are searched for in this order, which matters for a service started by
bt.usbfd - its current directory is `C:`, where they certainly are not:

1. `-f FOLDER`, when the user said so
2. `BT:Firmware`, an assign for a self contained installation of the stack
3. `SYS:Firmware/bt`, the system firmware drawer where AmigaOS keeps such files
4. the current directory, which is how the demos were used

Installing them in `SYS:Firmware/bt` is the recommended choice: it needs no
assign and works whoever starts the service. The exact paths used are printed at startup
("Realtek: Using firmware ... and config ...").

## bthid - use a Bluetooth LE mouse as the system mouse

`bthid` is not a BTstack example but an application of this port. It scans for
LE HID devices, pairs with the first one found and feeds its Input Reports
into `input.device` with `IND_WRITEEVENT`, so the pointer moves for the whole
system - Intuition and every application see a normal mouse.

```
bthid -p          (-p is needed for a mouse without LE Secure Connections)
bthid -p -v       also print every report
```

Once bonded the device is stored in the TLV, so the next start reconnects
without scanning. Quit with CTRL-C; held mouse buttons are always released on
the way out, so a button can never stay stuck system-wide.

Reports are decoded with BTstack's HID parser, looking fields up by HID usage
(Desktop X/Y/Wheel, Consumer AC Pan, Button page) instead of by offset, so the
report layout of the device does not matter. Injection itself lives in
`amigaos4_input.c` and is independent of Bluetooth, so it can be reused.

Note on writing events: `IND_WRITEEVENT` must be sent with `SendIO`, not
`DoIO`. The event travels through the whole input handler chain including
Intuition, and `DoIO` blocks until that is finished - dragging a window
border is enough to stall the caller, which then stops reading the USB
transfers and the mouse appears to freeze. `amigaos4_input.c` therefore uses
a pool of requests, each with its own event buffer (the device may still
reference the event after the call returns), and the replies are collected by
`amigaos4_input_poll()` from the run loop. Events that find no free request
are queued, with movement coalescing into the last queued movement so a long
stall costs one entry and no distance is lost.

Every `IECLASS_RAWMOUSE` event carries a real `GetSysTime()` time stamp -
Intuition derives double click from the interval between button events - and
the `IEQUALIFIER_LEFTBUTTON`/`RBUTTON`/`MIDBUTTON` bits for the buttons
currently held, which is how click and drag state is tracked.

Both Bluetooth LE and Classic are handled, and they are genuinely different
searches: an LE device is found by scanning for its advertisements, a Classic
one only by inquiry. A Classic keyboard never advertises, so it stays invisible
to an LE scan however long it runs - which is why the service does both at once
(`gap_start_scan()` plus a `gap_inquiry_start()` that restarts itself, since
inquiry is bounded rather than a state one leaves on). LE devices are matched on
their advertisement, Classic ones on their Class of Device.

Everything downstream is shared: `bt_hid_report.c` decodes the report and
injects the events, and neither it nor the keymap knows whether the report came
over GATT from an LE mouse or over L2CAP from a Classic keyboard.

Keyboards work too. HID usages are mapped to Amiga raw key codes in
`bt_hid_keymap.h`, positionally: usage 0x14 is "the key where Q is on a US
keyboard" and RAWKEY 0x10 is that same physical key, so what it produces is
decided by the keymap the user has chosen - an Italian or German layout comes
out right without anything here knowing about it.

A HID keyboard reports the set of keys held right now, so presses and releases
are derived by comparing consecutive reports. Modifiers are sent before the
keys they apply to, and every key event carries the two previously pressed keys
in `ie_dead`, which is how `keymap.library` composes dead keys - without them
accented characters do not work.

Events are written with `IND_ADDEVENT`, not `IND_WRITEEVENT`: on AmigaOS 4 that
is what a driver feeding the input stream uses - the boot mouse, boot keyboard
and HID drivers all do, and their history files say `IND_WRITEEVENT` is the OS3
way, needing an input handler.

## BluetoothGUI - managing devices

A ReAction front end for the service. It holds no Bluetooth state and links
against no part of BTstack: everything it shows arrives over the public message
port and everything it does is a command sent there, so the service can be
started, stopped or restarted underneath it.

```
BluetoothService        (start it first, or from bt.usbfd at boot)
BluetoothGUI
```

Two lists, because they answer different questions. **Nearby** is what is being
heard from right now, built from the sighting events the service sends rather
than asked for - the service keeps no table entry for a device no handler can
drive, since LE privacy addresses rotate and the supply is endless, but it
announces every one. **Known devices** is what has been bonded, which persists
and is what there is to manage: connect, disconnect, or forget.

Scan also restarts the Classic inquiry, which the service stops by itself once
everything it knows is connected - so it is the way to add a second keyboard.

The service itself is started and stopped from here, and the button says which
of the two it will do. Whether it is running is checked every couple of seconds
rather than assumed: the service is not the GUI's to own - bt.usbfd starts it
when a controller is plugged in, it can be stopped from a Shell, and it can exit
by itself - and there is nothing to be notified by when the thing that would
send the notification is the thing that has gone.

With no service running the lists are emptied and every other button is
disabled, including Scan. Everything shown belongs to the service, and listing a
device as connected by a service that is not there would be worse than showing
nothing at all.

Buttons that act on a selection are disabled while there is none.

It is localised. `BluetoothGUI.cd` describes the strings for translators and
`bluetooth_gui_cat.h` holds the IDs with their English text in the shape CatComp
produces - written by hand because CatComp is an Amiga tool and this cross builds
on a host without one, but laid out so a real CatComp run over the `.cd` drops
straight in. The built-in text is the fallback handed to `GetCatalogStr()`, so
everything reads correctly with no catalog installed, with one that does not
cover a string, or with no locale.library at all.

## Receiving files

Object Push is offered over both RFCOMM and L2CAP. A 9 MB file arrives in about
a hundred seconds - roughly 87 kB/s - and getting there took four things, each
of which was worth a fifth or more.

**The L2CAP bearer has to open.** GOEP 2.0 is OBEX over L2CAP, and Single
Response Mode is a GOEP 2.0 feature: over RFCOMM the sender never asks for it,
waits for a response to every packet, and a transfer runs at 17 kB/s. That is
the single biggest factor.

**ERTM cannot be mandatory.** BTstack's GOEP server sets `ertm_mandatory`, and
l2cap then closes the channel the moment a peer's configure request arrives with
no Retransmission and Flow Control option - which is how Basic Mode is proposed,
and what Linux proposes here. The sender sees the connection reset.
`GOEP_SERVER_ERTM_MANDATORY` is 0 in `btstack_config.h` for that reason. The
mode does not decide the speed: SRM is negotiated in OBEX headers, so a Basic
Mode channel that opens beats an ERTM channel that does not.

**The radio has to be left alone.** Paging a sleeping keyboard costs a five
second page timeout each time, and the LE scan runs a quarter of the time
permanently. Both were happening throughout every transfer, and both are now
suspended while one is running. Together they were about a third of the
throughput.

**Enough reads in flight.** With one ACL read outstanding the controller has
nowhere to put the next packet until the last has been processed, so throughput
becomes one packet per round trip. There are sixteen.

If it ever needs looking at again, the numbers to reach for are in the log at
the end of each transfer - bytes, packets, elapsed time - and `-l FILE` writes a
packet log that shows the L2CAP configuration exchange, which is where three
different wrong guesses were finally settled.

## Bluetooth Classic keyboards

Four things are needed to keep one connected, and none of them is obvious.

**Page scan has to be on.** BTstack builds the scan enable value as
`(connectable << 1) | discoverable`, and `connectable` defaults to off, so
asking only for `gap_discoverable_control(1)` leaves page scan disabled: we can
find devices and dial them, and nothing can dial us. A bonded keyboard does not
wait to be dialled - it sleeps, and on a keypress it pages its host. Without
`gap_connectable_control(1)` those pages go nowhere and the keyboard can only be
reached while it is in pairing mode and answering inquiry, which is why it used
to work for exactly as long as its pairing light blinked.

**Sniff mode has to be allowed.** The default link policy settings are zero,
which disallows everything, so the controller refuses the sniff request a
battery keyboard makes. Refused, it does not stay awake - it goes quiet, and the
link dies of supervision timeout (reason 0x08) about twenty seconds later.

**The radio has to be left alone.** Scanning with interval equal to window is a
100% duty cycle, and an inquiry running five seconds out of every five hops the
inquiry sequence while established links take what is left. A keyboard in sniff
mode has sparse anchor points by design and gets none. Inquiry is for finding a
device we do not have; a bonded one is reached by paging it or by it paging us,
so inquiry stops once a Classic device is bonded and a client has to ask for it
to run again.

**A stale link key is worse than none.** A device held in pairing mode is
waiting to create a new bond. Authenticating it with the key from an old one
succeeds - "asked for its link key", authentication and encryption all report
status 0 - but its own pairing never completes, and it drops the link when its
pairing window closes. `bonded, link key stored` in the log is the line that
says a real bond was made; if it is missing, no pairing happened. `--forget-all`
drops the link keys and the device list so everything can be paired afresh.

Reports arrive as the whole L2CAP payload, which over Classic begins with a
one byte HID transaction header (0xa1, DATA/Input) before the report itself.
It is stripped in the Classic handler, not in the shared decoder, because
reports arriving over LE as GATT notifications have no such byte.

Note that a modifier can be latched in the keyboard itself - if every report
carries the same modifier bit, including the ones where no key is down, that is
the keyboard's own state and pressing and releasing that modifier clears it. The
descriptor is printed under `-v` and each report with it, so where a field
actually sits can be checked rather than guessed.

## Pairing with devices without LE Secure Connections

`sm_init()` enables LE Secure Connections *Only* mode whenever
`ENABLE_LE_SECURE_CONNECTIONS` is configured, so pairing with a device that
only supports LE Legacy Pairing is rejected with
`SM_REASON_AUTHENTHICATION_REQUIREMENTS` ("Pairing failed, reason = 3").
Many BLE mice and older peripherals are legacy-only. Pass `-p` to accept
legacy pairing:

```
hog_host_demo -p
```

The main executable registers the known Realtek USB controllers
automatically, so a Realtek-based dongle should be detected without
extra command-line options. Run an example such as:

```
gap_inquiry
```

or, with logging enabled:

```
gap_inquiry -l T:hci_dump.pklg
```

## Notes

* Temporary/TLV files are written to `T:` instead of `/tmp`.
* The wrapper header `include/libusb.h` maps btstack's expected
  `<libusb.h>` to the AmigaOS 4 SDK's `<libusb-1.h>`.
* The AmigaOS 4 SDK defines `UNUSED` as `__attribute__((unused))`, which
  conflicts with btstack's definition; the transport source undefines it
  before including btstack headers.
