# UniXNU-tb365fc

**中文版: [README.md](README.md)**

UniXNU device repository for the **Lenovo Xiaoxin Pad 12.1 (2025) / Lenovo TB365FC**
(board `cava_prc_consumer_wifi`, MediaTek MT6835, Android 15 stock firmware).

## Status (2026-11-21)

Milestones reached:

- The stock ZUXOS 1.1.10.122 package is fully unpacked; boot-chain geometry,
  CPU/GIC/timers, the UFS host, the partition layout and the board peripherals
  (panel, touch, PMIC, audio, connsys) are recorded in `porting.toml`.
- **UEFI boot chain**: `boot_a` carries the UEFI FD (diag131); the stock LK
  accepts the self-built image and reaches the UniXNU menu.
- **Payload transport**: a 58 MiB FAT payload in `vendor_boot_a` carries the
  kernel, ADT and trust cache.
- **XNU 8796 boots into iPadOS 17.7.11 userland**: launchd -> SpringBoard,
  reaching the lock screen and home screen; the native UFS driver mounts the
  iPadOS root volume, SystemOS and Data (Data is a RAM seed, not persistent).
- **Display**: the legacy IOMobileFramebuffer path scans real frames onto the
  BOE panel.
- **GPU**: the Mali-G57 (Valhall JM) power sequence and the first executed job
  are verified from XNU.
- **Input (partial)**: ShengHidInput is integrated; hardware keys work and the
  lock screen can be unlocked offline.

**Touch bring-up in progress** (BOE HX83122B TDDI):

- Done: bit-banged SPI transport, ZF firmware download (project P785, `72C0`
  handshake), INT edge activity confirmed under real touches.
- In progress: the event stack (`0x30`, 68 B) still returns all-zero frames;
  lengths 56/68, IRQ gating, DSRAM mirrors and runtime re-arms are ruled out.
  The transport is moving to the **MTK SPI3 hardware bus** (following
  `ruby-ios-bringup`'s MTK approach) combined with `UniXNU-tb128fu`'s Himax
  event-loop ideas (three-channel INT, idle reads, publish de-duplication).
- Known issue: heavy continuous SPI reads can wedge the HX83122B slave; a full
  power cycle (unplug USB, hold power) or the stock driver's power sequencing
  revives it.

**Partitions and restore point**: the current versions, local images, sha256
values and restore commands for `boot_a`, `vendor_boot_a` and `userdata` are in
[`PARTITION-LEDGER-2026-11-21.md`](PARTITION-LEDGER-2026-11-21.md).

## Layout

| Path | Content |
| --- | --- |
| `board.toml` | Format 2 board configuration and payload inputs (userdata windows, ADT, kernel args) |
| `feeds.toml` | MT6835 SoC source; local checkout during bring-up |
| `configs/tb365fc_defconfig` | Bootstrap kernel gates |
| `dts/tb365fc.dts` | Board memory map and peripheral wiring; includes `mt6835.dtsi` |
| `patches/xnu/series` | Board XNU patch series |
| `proprietary-files.txt` | Stock boot-chain hashes and their consumers |
| `porting.toml` | Maintainer record rendered by the documentation site |
| `package/` | Board package (`kits/tb365fc` in the build tree): display, touch, HID and GPU probes |
| `tools/` | Device inventory, stock partition pull, boot-image and FDT unpacking |
| `PARTITION-LEDGER-2026-11-21.md` | Partition/image ledger and both restore paths |

The UEFI firmware lives in a separate `uefi-tb365fc` checkout.

## Prerequisites

- A UniXNU checkout with `./unixnu` and Python 3 on the build host.
- The sibling `UniXNU-mt6835-common` checkout (replace the `local` entry in
  `feeds.toml` with a pinned commit before publishing).
- Stock IPSW `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` (fetched by the OS profile).
- The stock TB365FC firmware package; partition forensics need a rooted Android
  (on this unit userdata now holds the iPadOS root image; see the ledger).

## Build and flash

From the UniXNU checkout:

```sh
./unixnu defconfig tb365fc_defconfig -D ../UniXNU-tb365fc-work
./unixnu build kernel -D ../UniXNU-tb365fc-work
./unixnu status --json -D ../UniXNU-tb365fc-work
```

Flashing discipline (see the ledger):

- Only `vendor_boot_a` is written routinely (each `flash-diagNNN.sh` carries a
  sha256 guard and a slot check).
- `boot_a` changes only when the UEFI FD is upgraded (currently diag131).
- **Never re-lock the bootloader**; `init_boot`, `dtbo`, `vbmeta` and `super`
  are left untouched.
- `userdata` is written only when replaying the iPadOS root image with the
  sparse-piece helper.

See [Device repository](https://unixnu.com/porting/device-repository),
[Device tree](https://unixnu.com/porting/device-tree) and
[Bring-up](https://unixnu.com/porting/bring-up).

## Touch references

- `lky005/ruby-ios-bringup` (Redmi Note 12 Pro 5G / MT6877): MTK SPI3 at
  0x11013000, EINT child controller, persistent-log reader methods.
- `xVanTuring/UniXNU-tb128fu` (Lenovo Xiaoxin Pad 2022 / HX83102E): Himax ZF
  protocol, three-channel INT event loop, host-side unit tests.

## Privacy

The device information dump and everything under `out/` are for local use. Do
not commit serial numbers, Wi-Fi/Bluetooth MAC addresses, FRP data or extracted
firmware blobs; `proprietary-files.txt` records hashes only.
