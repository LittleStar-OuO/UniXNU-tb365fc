# 硬件取证与镜像解包工具

**English edition: [README.en.md](README.en.md)**

这些脚本把 TB365FC 原厂固件转化为设备/固件仓库所需的测量值。除写入本地输出目录外，
对平板本身均为只读操作。

依赖：`bash`、带 USB 调试的 `adb`、`python3`；`dtc`（device-tree-compiler）可选，
仅在反编译 blob 时使用。

## 1. 盘点运行中的平板

```bash
./collect-device-info.sh -o out/inv
```

输出 `build-identity.txt`、过滤后的 `getprop`、`/proc/cmdline`、`/proc/cpuinfo`、
`/proc/meminfo`、`/proc/partitions`、`block-by-name.txt`、`block-queue.txt`
（每个设备的大小与逻辑块大小）、`boot-devices.txt`、`wm-size.txt`、`wm-density.txt`；
有 root 时另存 `/proc/iomem`、live FDT（`fdt.dtb`）与顶层 DT 节点列表。
序列号、IMEI/MEID、MAC 与 Android ID 行会从 `props-filtered.txt` 过滤；
`cmdline.txt`、`bootconfig.txt` 中的标识值与 `cpuinfo.txt` 的 `Serial` 行会被遮蔽；
加 `--dmesg` 还会保存过滤后的内核日志。`fdt.dtb` 仍可能含 MAC/序列号属性，
分享前请人工复核。

用 `build-identity.txt` 确认板型（`ro.product.device`、`ro.board.platform`、
`ro.boot.hardware`、`ro.boot.hardware.sku`）匹配 `cava_prc_consumer_wifi` / MT6835。

## 2. 拉取原厂启动镜像

```bash
./pull-stock-images.sh -o out/stock
# 或指定集合：
./pull-stock-images.sh -o out/stock boot vendor_boot dtbo
```

需要 root。脚本拒绝 `persist`、`nvram`、`modem*`、`fsg`、`fsc`、`radio`、
`metadata`、`userdata` 与 `super`。文件按分区命名（有活动槽位后缀时为
`boot_a.img` 等）。

如果官方固件包是带 `payload.bin` 的 zip，可在主机侧提取：

```bash
# 来自 OTA/工厂包的 payload.bin
payload-dumper-go -o out/stock payload.bin
# 或从 super 镜像：
lpunpack --slot=0 super.img out/stock
```

QFIL/SP Flash Tool 包本身已包含各分区 `.img`。

## 3. 读取 boot 头

```bash
python3 unpack_boot_image.py out/stock/boot_a.img
python3 unpack_boot_image.py out/stock/boot_a.img --json
python3 unpack_boot_image.py out/stock/boot_a.img --extract out/stock/unpacked
python3 unpack_boot_image.py out/stock/vendor_boot_a.img --extract out/stock/unpacked
```

支持：boot 头 v0–v4（`ANDROID!`）与 vendor_boot v3/v4（`VNDRBOOT`）。头部字段去向：

| 字段 | 去向 |
| --- | --- |
| `kernel_addr`、`pagesize`、`header_version` | `Resources/Configs/tb365fc.toml` |
| `tags_addr`、`ramdisk_addr` | `tags_offset`、`ramdisk_offset` |
| `cmdline` | `[boot_image] cmdline` |
| `dtb_size`、`dtb_addr` | `dtb_offset`（v2 镜像） |

v0–v2 的 boot 基址为 `kernel_addr - kernel_offset`；除原厂镜像另有证明，
`kernel_offset` 保持 `0x8000`。注意 `./unixnu bootimg` 只把 `base`、
`kernel_offset`、`tags_offset`、`pagesize`、`header_version`、`ramdisk_offset`、
`second_offset` 与 `cmdline` 传给 mkbootimg，忽略 `dtb`/`dtb_offset`：
它把编译出的 DTB 追加在 gzip 内核之后，blob 取自命令行的 `--dtb`。
`dtb`/`dtb_offset` 仅对独立 `build_uefi.py` 参考路径有意义。
v3/v4 boot 镜像不携带加载地址，无法据此推导 FD 基址：使用 LK/fastboot
日志或 Plan B。

## 4. 提取原厂设备树

```bash
python3 extract_fdt.py out/stock/dtbo_a.img -o out/stock/fdt --dtc
python3 extract_fdt.py out/stock/vendor_boot_a.img -o out/stock/fdt --dtc
python3 extract_fdt.py out/stock/dtb_a.img --raw-scan -o out/stock/fdt --dtc
```

支持单个 DTB、DTBO 表、vendor_boot DTB（含内部 DTBO 容器）以及拼接多棵树的
原始分区。每个 blob 写成 `<name>-NN.dtb`；`--dtc` 同时写出 `<name>-NN.dts`。

与骨架树对比：

```bash
diff -u ../../UniXNU-mt6835-common/dts/mt6835.dtsi out/stock/fdt/vendor_boot_a-00.dts
```

在 `porting.toml`（areas：`cpu`、`memory`、`display`、`storage`、`power`）
记录：文件名、命令、观察到的节点/属性及最终改动；随后更新 DTS 与固件值。

## 5. 记录证据

- `proprietary-files.txt` 每个文件记录一条 SHA-256 与来源分区；**不要**提交镜像本体。
- `porting.toml` 的发现应引用命令与关键头部行，例如
  `unpack_boot_image.py boot_a.img` → `kernel_addr 0x40080000`。
- 在全部 `REQUIRED` 值有测量记录且固件已在平板上启动前，`board.toml` 保持
  `example = true`。

## 隐私

原厂启动镜像含厂商代码；`persist`/`modem*`/`nvram` 含校准数据与标识信息，
均不应进入公开仓库。设备盘点脚本会过滤明显标识，但提交前请复核每个文件。
