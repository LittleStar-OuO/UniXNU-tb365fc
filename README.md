# UniXNU-tb365fc

UniXNU device repository for the **Lenovo Xiaoxin Pad 12.1 (2025) / Lenovo TB365FC**,
board `cava_prc_consumer_wifi`, MediaTek MT6835 (Dimensity 6300 family), Android 15
stock firmware.

## Status

The stock ZUXOS 1.1.10.122 package has been unpacked: the boot chain geometry,
CPU topology, GIC and timer, the UFS/MSDC host inventory, the partition download
map and the board peripherals (panel, touch, PMIC, audio, connsys) are recorded in
`porting.toml`. Still `REQUIRED`: the storage type (UFS vs eMMC), the DRAM size,
the real `userdata` start/end and logical block size, the panel resolution, the
exact Wi-Fi/BT die (MT6631-class per the stock firmware) and the LK's acceptance
of the self-built image. No XNU image has been
built or booted on this board yet. `example = true` makes `build` and `bootimg`
refuse the repository until the inputs are real.

## Layout

| Path | Content |
| --- | --- |
| `board.toml` | Format 2 board configuration and payload inputs |
| `feeds.toml` | MT6835 SoC source; uses the local checkout during bring-up |
| `configs/tb365fc_defconfig` | Bootstrap kernel gates |
| `dts/tb365fc.dts` | Board memory map and peripheral wiring; includes `mt6835.dtsi` |
| `patches/xnu/series` | Board XNU patches, empty until verified |
| `proprietary-files.txt` | Stock boot-chain hashes and their consumers |
| `porting.toml` | Maintainer record rendered by the documentation site |
| `package/` | Board package, appears as `kits/tb365fc` in the build tree |
| `tools/` | Device inventory, stock partition pull, boot image and FDT unpacking |

The UEFI firmware lives in a separate `uefi-tb365fc` checkout.

## Prerequisites

- A UniXNU checkout with `./unixnu` and Python 3 on the build host.
- The sibling `UniXNU-mt6835-common` checkout (or replace the `local` entry in
  `feeds.toml` with a pinned commit before publishing).
- Stock IPSW `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` (the OS profile fetches it).
- Stock TB365FC firmware package and a rooted device for partition extraction.

## Bring-up order

1. Collect inputs with the tools in `tools/` (device inventory, boot/vendor_boot/dtbo
   unpack, stock DTB to DTS).
2. Confirm the remaining measured values on the tablet: storage type (UFS vs eMMC),
   DRAM size, `userdata` start/end and logical block size, panel resolution and the
   active touch/panel variant.
3. Build the UEFI firmware from `uefi-tb365fc` and confirm the payload menu.
4. Generate the ADT, build the payload and boot image, then record each hardware
   checkpoint in `porting.toml`.
5. Set `example = false`, pin the SoC commit and publish with the `unixnu` topic.

## Commands

Run from the UniXNU checkout:

```sh
./unixnu defconfig tb365fc_defconfig -D ../UniXNU-tb365fc
./unixnu status --json -D ../UniXNU-tb365fc
./unixnu tree -D ../UniXNU-tb365fc
python3 scripts/porting-record.py check --device ../UniXNU-tb365fc
```

See [Device repository](https://unixnu.com/zh/porting/device-repository),
[Device tree](https://unixnu.com/zh/porting/device-tree) and
[Bring-up](https://unixnu.com/zh/porting/bring-up).

## Privacy

The device information dump and everything under `out/` are for local use. Do not
commit serial numbers, Wi-Fi/Bluetooth MAC addresses, FRP data or extracted firmware
blobs; `proprietary-files.txt` records hashes only.
