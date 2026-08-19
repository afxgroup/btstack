#!/bin/sh
#
# Assemble the installable archive.
#
# The layout below is what Install expects to find beside it, so this and the
# script have to agree - which is why it is a script rather than instructions
# that drift.
#
# libusb-1.library is not built here and is not in this repository. Point
# LIBUSB at a built one, or the archive is made without it and the installer
# will fail on that step.

set -e

PORT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$PORT/../.." && pwd)"
OUT="${1:-$PORT/build/BluetoothStack}"
LIBUSB="${LIBUSB:-}"

rm -rf "$OUT"
mkdir -p "$OUT/C" "$OUT/Libs" "$OUT/Devs/USB/fd" "$OUT/Firmware" "$OUT/Documentation"

cp "$PORT/install/Install"                    "$OUT/"
cp "$PORT/build/BluetoothGUI"                 "$OUT/"
cp "$PORT/build/BluetoothService"             "$OUT/C/"
cp "$PORT/../amigaos4-usbfd/bt.usbfd"         "$OUT/Devs/USB/fd/"
cp "$PORT/firmware/"rtl*                      "$OUT/Firmware/"
cp "$PORT/install/Documentation/"*            "$OUT/Documentation/"
cp "$PORT/README.md"                          "$OUT/"
cp "$ROOT/LICENSE"                            "$OUT/"

if [ -n "$LIBUSB" ] && [ -f "$LIBUSB" ]; then
    cp "$LIBUSB" "$OUT/Libs/libusb-1.library"
else
    echo "warning: no libusb-1.library in the archive - set LIBUSB to one" >&2
fi

echo "archive laid out in $OUT"
find "$OUT" -type f | sed "s|$OUT/||" | sort
