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

Keyboards are not handled yet. For those, `IECLASS_EXTENDEDRAWKEY` with
`IESUBCLASS_SET1_RAWKEY` accepts PC Set-1 scancodes, which avoids writing a
full USB HID to Amiga rawkey mapping table.

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
