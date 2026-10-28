# UniXNU-tb365fc

UniXNU device repository for the **Lenovo Xiaoxin Pad 12.1 (2025) / Lenovo TB365FC**,
board `cava_prc_consumer_wifi`, MediaTek MT6835 (Dimensity 6300 family), Android 15
stock firmware.

## Status

The UEFI firmware in `uefi-tb365fc` builds and boots on the tablet: the stock LK
loads the 4 MiB FD from `boot_a` (Android boot v4, gzip kernel), the Project
Silicium logo appears, then the UniXNU menu at 1600x2560 (Return to firmware /
Restart / Power off / Tools). Tools → Advanced Logger renders the in-RAM log as
QR pages with five speed gears; Volume Up/Down change the gear and Power or
Enter exit. UART0 mirrors the same log.

Measured and recorded in `porting.toml`: 8 GiB DRAM, UFS 2.2 (4096-byte logical
sectors, 71-entry GPT), BOE HX83122B 1600x2560 @90 Hz panel, MT6631 combo,
MT6377 PMIC keys and the `userdata` LBA range. Still `REQUIRED`: the APFS Data
volume UUID and the unique chip ID that the payload needs; no XNU image has been
built or booted yet. `example = true` makes `build` and `bootimg` refuse the
repository until those inputs are real.

## Layout

| Path | Content |
| --- | --- |
| `board.toml` | Format 2 board configuration and payload inputs |
| `feeds.toml` | MT6835 SoC source pinned to the published repository |
| `configs/tb365fc_defconfig` | Bootstrap kernel gates |
| `dts/tb365fc.dts` | Board memory map and peripheral wiring; includes `mt6835.dtsi` |
| `patches/xnu/series` | Board XNU patches, empty until verified |
| `proprietary-files.txt` | Stock boot-chain hashes and their consumers |
| `porting.toml` | Maintainer record rendered by the documentation site |
| `package/` | Board package, appears as `kits/tb365fc` in the build tree |
| `tools/` | Device inventory, stock partition pull, boot image and FDT unpacking |
| `local.toml.example` | Copy to the ignored `local.toml` for local source overrides |
| `unixnu.lock` | Resolved source commits written by `unixnu fetch` |

The UEFI firmware lives in a separate `uefi-tb365fc` checkout. The UniXNU
framework changes the firmware UI needs are kept in a `UniXNU` fork on the
`tb365fc` branch.

## Prerequisites

- A UniXNU checkout with `./unixnu` and Python 3 on the build host.
- The `UniXNU-mt6835-common` commit pinned in `feeds.toml`, or a local checkout
  pointed to by `local.toml`.
- Stock IPSW `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` (the OS profile fetches it).
- Stock TB365FC firmware package and a rooted device for partition extraction.

## Bring-up order

1. (done) Collect stock inputs with `tools/`: device inventory, boot/vendor_boot/
   dtbo unpack and stock DTB to DTS.
2. (done) Confirm the measured values on the tablet: UFS with 4096-byte logical
   sectors, 8 GiB DRAM, `userdata` start/end, 1600x2560 BOE HX83122B panel,
   MT6631 combo and MT6377 keys.
3. (done) Build the UEFI firmware from `uefi-tb365fc`, flash the v4 boot image to
   `boot_a` and confirm the UniXNU menu and the Advanced Logger viewer.
4. (next) Build the XNU kernel and payload, generate the ADT and record each
   hardware checkpoint in `porting.toml`.
5. (next) Measure the APFS Data volume UUID and the unique chip ID, set
   `example = false` and refresh the source pins.

## Commands

Run from the UniXNU checkout:

```sh
./unixnu defconfig tb365fc_defconfig -D ../UniXNU-tb365fc
./unixnu fetch -D ../UniXNU-tb365fc
./unixnu status --json -D ../UniXNU-tb365fc
./unixnu tree -D ../UniXNU-tb365fc
./unixnu firmware --uefi ../uefi-tb365fc -D ../UniXNU-tb365fc
python3 scripts/porting-record.py check --device ../UniXNU-tb365fc
```

The firmware build applies the Mu base-core patches listed in `board.toml
[firmware]` to the shared edk2 checkout; `uefi-tb365fc/README.md` documents the
manual build and the boot image layout.

See [Device repository](https://unixnu.com/zh/porting/device-repository),
[Device tree](https://unixnu.com/zh/porting/device-tree) and
[Bring-up](https://unixnu.com/zh/porting/bring-up).

## Privacy

The device information dump and everything under `out/` are for local use. Do not
commit serial numbers, Wi-Fi/Bluetooth MAC addresses, FRP data or extracted firmware
blobs; `proprietary-files.txt` records hashes only.
