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
  timer.
* SCO / audio-over-HCI is not yet supported.
* USB path selection (`-u`) is not supported; the first matching VID/PID
  from the known-device list is used.
* Network (PAN/BNEP) and audio examples are excluded.

## Testing with Realtek dongles

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
