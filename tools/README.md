# Hardware collection and image unpacking

These scripts turn the TB365FC stock firmware into the values the device and
firmware repositories still mark `REQUIRED`. They are read-only with respect
to the tablet, apart from writing to the local output directory.

Requirements: `bash`, `adb` with USB debugging, `python3`; `dtc`
(`device-tree-compiler`) is optional and only used to decompile blobs.

## 1. Inventory the running tablet

```bash
./collect-device-info.sh -o out/inv
```

Writes `build-identity.txt`, filtered `getprop`, `/proc/cmdline`,
`/proc/cpuinfo`, `/proc/meminfo`, `/proc/partitions`, `block-by-name.txt`,
`block-queue.txt` (size and logical block size per device), `boot-devices.txt`,
`wm-size.txt`, `wm-density.txt` and, with root, `/proc/iomem`, the live FDT
(`fdt.dtb`) and the top-level DT node list. Serial, IMEI/MEID, MAC and Android ID lines are filtered from
`props-filtered.txt`; identifier values in `cmdline.txt` and `bootconfig.txt`
and the `Serial` line of `cpuinfo.txt` are redacted; `--dmesg` additionally
saves a filtered kernel log. `fdt.dtb` can still embed MAC or serial
properties, so review it before sharing.

Use `build-identity.txt` to confirm the board (`ro.product.device`,
`ro.board.platform`, `ro.boot.hardware`, `ro.boot.hardware.sku`) matches
`cava_prc_consumer_wifi` / MT6835.

## 2. Pull the stock boot images

```bash
./pull-stock-images.sh -o out/stock
# or a specific set:
./pull-stock-images.sh -o out/stock boot vendor_boot dtbo
```

Root is required. The script refuses `persist`, `nvram`, `modem*`, `fsg`,
`fsc`, `radio`, `metadata`, `userdata` and `super`. Files are named after the
partition (`boot_a.img` when a slot suffix is active).

If the official firmware package is a zip with `payload.bin`, extract the
same images on a host instead:

```bash
# payload.bin from the OTA/factory package
payload-dumper-go -o out/stock payload.bin
# or, from a super image
lpunpack --slot=0 super.img out/stock
```

A QFIL/SP Flash Tool package already contains the partition `.img` files.

## 3. Read the boot header

```bash
python3 unpack_boot_image.py out/stock/boot_a.img
python3 unpack_boot_image.py out/stock/boot_a.img --json
python3 unpack_boot_image.py out/stock/boot_a.img --extract out/stock/unpacked
python3 unpack_boot_image.py out/stock/vendor_boot_a.img --extract out/stock/unpacked
```

Supported: boot header v0-v4 (`ANDROID!`) and vendor_boot v3/v4
(`VNDRBOOT`). The header dump gives:

| Field | Goes to |
| --- | --- |
| `kernel_addr`, `pagesize`, `header_version` | `Resources/Configs/tb365fc.toml` |
| `tags_addr`, `ramdisk_addr` | `tags_offset`, `ramdisk_offset` |
| `cmdline` | `[boot_image] cmdline` |
| `dtb_size`, `dtb_addr` | `dtb_offset` (v2 images) |

For v0-v2 the boot image base is `kernel_addr - kernel_offset`; keep
`kernel_offset` at `0x8000` unless the stock image proves a different value.
Note that `./unixnu bootimg` passes only `base`, `kernel_offset`, `tags_offset`,
`pagesize`, `header_version`, `ramdisk_offset`, `second_offset` and `cmdline`
to mkbootimg and ignores `dtb`/`dtb_offset`: it appends the compiled DTB to the
gzipped kernel and takes the blob from `--dtb` on the command line. `dtb` and
`dtb_offset` only matter to the standalone `build_uefi.py` reference path.
v3/v4 boot images carry no load addresses, so the FD base cannot be derived
from them: use the LK/fastboot log or Plan B.

## 4. Extract the stock device tree

```bash
python3 extract_fdt.py out/stock/dtbo_a.img -o out/stock/fdt --dtc
python3 extract_fdt.py out/stock/vendor_boot_a.img -o out/stock/fdt --dtc
python3 extract_fdt.py out/stock/dtb_a.img --raw-scan -o out/stock/fdt --dtc
```

Handles a single DTB, a DTBO table, a vendor_boot DTB (including a DTBO
container inside it) and raw partitions with concatenated trees. Every blob
is written as `<name>-NN.dtb`; `--dtc` also writes `<name>-NN.dts`.

Diff the decompiled trees against the skeletons:

```bash
diff -u ../../UniXNU-mt6835-common/dts/mt6835.dtsi out/stock/fdt/vendor_boot_a-00.dts
```

Record in `porting.toml` (areas `cpu`, `memory`, `display`, `storage`,
`power`): the file name, the command, the observed node/property and the
resulting change; then update the DTS and the firmware values.

## 5. Record the evidence

- `proprietary-files.txt` gets one SHA-256 and source partition per file;
  never commit the image itself.
- `porting.toml` findings should quote the command and the relevant header
  line, e.g. `unpack_boot_image.py boot_a.img` → `kernel_addr 0x40080000`.
- Keep `example = true` in `board.toml` until every `REQUIRED` value has a
  recorded measurement and the firmware has booted on the tablet.

## Privacy

Stock boot images contain vendor code; `persist`/`modem*`/`nvram` contain
calibration and identifiers. Neither belongs in a public repository. The
device inventory scripts filter obvious identifiers, but review every file
before committing it.
