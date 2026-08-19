# Realtek Bluetooth firmware

Realtek controllers hold no firmware of their own: the host downloads it at
every startup, and without it the controller answers commands but finds
nothing - it will scan for a minute and report not one device.

These are the files from `linux-firmware`, decompressed and renamed to what
BTstack asks for. Its chipset table names the USB variant of a part, while
linux-firmware usually names the part itself, so `rtl8821cu_fw` here is
linux-firmware's `rtl8821c_fw.bin`.

## Installing

Copy the whole folder to `SYS:Firmware/bt`, which is where the service looks
without needing an assign and whoever starts it:

    SYS:Firmware/bt/rtl8761bu_fw
    SYS:Firmware/bt/rtl8761bu_config

The search order is `-f FOLDER`, then `BT:Firmware`, then `SYS:Firmware/bt`,
then the current directory. The paths actually used are printed at startup, so
if a controller is not coming up, that line says where BTstack looked.

Only the two files for the controller in use are read; the rest cost nothing but
disk space.

## Where each file came from

Named the same in linux-firmware, so these are certain:

    rtl8723a_fw        rtl8723b_fw        rtl8723b_config
    rtl8761bu_fw       rtl8761bu_config   rtl8821a_fw        rtl8821a_config
    rtl8822cu_fw       rtl8822cu_config   rtl8851bu_fw       rtl8851bu_config
    rtl8852au_fw       rtl8852au_config   rtl8852bu_fw       rtl8852bu_config
    rtl8852cu_fw       rtl8852cu_config

Renamed from the part linux-firmware ships, which is the same silicon under a
different name - correct as far as the naming goes, but not verified against
hardware here:

    rtl8723du_fw       <- rtl8723d_fw          rtl8723du_config   <- rtl8723d_config
    rtl8761au_fw       <- rtl8761a_fw          rtl8761aw_fw       <- rtl8761a_fw
    rtl8821cu_fw       <- rtl8821c_fw          rtl8821cu_config   <- rtl8821c_config
    rtl8822bu_fw       <- rtl8822b_fw          rtl8822bu_config   <- rtl8822b_config

## Not here

BTstack knows of these, and linux-firmware does not ship them. A controller
needing one of them cannot be brought up with this folder alone:

    rtl8723a_config    rtl8723fu_fw       rtl8723fu_config
    rtl8725au_fw       rtl8725au_config   rtl8761a_config    rtl8761aw_config
    rtl8761au8192ee_fw rtl8761au8812ae_fw
    rtl8821du_fw       rtl8821du_config   rtl8822eu_fw       rtl8822eu_config
    rtl8851au_fw       rtl8851au_config

## Licence

Realtek firmware, redistributed from `linux-firmware`, whose `WHENCE` file
carries the terms. It is not covered by BTstack's licence and no part of it was
written here.
