# HANDOVER 2026-11-17 — TB365FC 输入、HX83122B ZF 与 diag279/280/281

本文是 `HANDOVER-2026-11-17-TOUCH.md` 的完整后继交接，覆盖：接手后的全部代码改动、HX83122B Zero-Flash 的厂商顺序、diag279/280/281 的构建与烧写记录、控制台拍照限制，以及下一步判据和风险。

原 TOUCH 文档保留为历史快照；两者冲突时以本文为准。

---

## 0. 一句话状态

- 触摸 HID 链路（digitizer 服务、报告描述符、坐标发布）已打通代码路径，可用合成滑动验证。
- 实体按键（Power / Volume Up / Volume Down）已实现，走 report 2。
- HX83122B 的 **真实 ZF 分区表**已解出：表在文件 `0x20500`，pnum=13，FW entity 129024 B 到 `0x20000000`，12 个 DSRAM 段打包成 27632 B 连续镜像一次写入。
- ZF 下载现在按厂商 `himax_mcu_firmware_update_0f` + `power_on_init` 的完整顺序执行：`sense_off` 密码、FW/cfg 的 **HW CRC**、reload enable、reset GPIO 脉冲、`0x72C0` 握手轮询。
- diag279 已烧写并启动，但只看到旧的 `TSP f2 …`（全零），ZF 结果被“只留最后一行”的控制台挤掉。
- diag280 已构建未烧写；**diag281 已烧写并重启**，最后一行 `TSP st ph=… fw=… cf=… rl=… c6=… up=…` 尚未读回。
- 因此：**ZF 是否真正启动、触摸是否出点，仍未在硬件上确认。**

## 1. 设备与目标

- 设备：Lenovo TB365FC（代号 cava，MT6835，12.1" 平板）。
- 目标：UniXNU + prelinked XNU（iPad7,1 / iPadOS 17.7.11 / 21H461 用户态）。
- 已打通：UEFI 引导链、UFS、根卷、launchd、锁屏与桌面、LegacyFramebuffer 显示、离线 Home 注入。
- 本阶段焦点：输入（按键 + 触摸），触摸的源头是 HX83122B 固件未进入运行态。

## 2. 仓库、构建与烧写

### 2.1 仓库

- 设备仓库（源码唯一真源）：`E:\workspace\Repository\UniXNU-tb365fc-work`
- UniXNU 主仓库：`E:\workspace\Repository\UniXNU`
- 产物树：`out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree`
- 镜像/脚本暂存：`out/tb365fc/test-bootloop-20261029`

注意：
- `tree/kits/tb365fc` 是指向 `package/` 的 **WSL 符号链接**；在 Windows 下看到的是 0 字节文件，不要删。
- `tree/scripts/build-display-archive.py` 是**副本**，不是链接。改完仓库的 `scripts/build-display-archive.py` 后，快速编译前要复制到 tree。

### 2.2 快速归档编译（约 1–2 分钟）

```sh
T=/mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree
cp /mnt/e/workspace/Repository/UniXNU-tb365fc-work/scripts/build-display-archive.py $T/scripts/
cd $T && python3 scripts/build-display-archive.py
```

成功标志：`Built TB365FC legacy display archive; key/HID probes linked`。

### 2.3 内核构建（本机环境的实际做法）

正常命令：

```sh
cd /mnt/e/workspace/Repository/UniXNU
./unixnu build kernel -D ../UniXNU-tb365fc-work
```

本机限制：单次工具调用最多 10 分钟，完整 XNU 编译会超时。已验证可行的做法：

1. 先让 `./unixnu build kernel` 跑到 `build-xnu-4k-baseline.sh`（对象会增量缓存）。
2. 归档已构建后，把 `out/tb365fc/ipados-17.7.11-21H461/tb365fc/.config` 的
   `CONFIG_XNU_BUILD_ARCHIVES=y` 临时改成 `n`。
3. 再跑 `./unixnu build kernel`，会跳过归档、只做 XNU 增量链接与 prelink，约 45 秒。
4. **改回 `y`**，否则下次不会重建归档。

产物：`out/.../tb365fc/kernel/kernel.macho`。

### 2.4 打包与烧写

```sh
cd /mnt/e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029
./build-diag281.sh     # 生成 payload281.img / m281.macho / diag281-vendor_boot.img
```

烧写要求：
- 设备手动进 fastboot；只写 `vendor_boot_a`；脚本带 sha256 + slot 守卫。
- `flash-diagNNN.sh` 使用 `/e/...` 路径，设计为在 MSYS/Git Bash 下运行。
- 不在 fastboot 时无法远程进入；必须人工按键。

## 3. 硬件事实

### 3.1 触摸

| 项目 | 值 |
| --- | --- |
| IC | Himax HX83122B（BOE 屏 TDDI） |
| 总线 | 位带 SPI：GPIO68 CLK / 69 CS / 70 MOSI / 71 MISO |
| 复位 | GPIO43 |
| IRQ | GPIO4（未使用，全部轮询） |
| MTK GPIO 基址 | `0x10005000`；MODE `0x300+(pin/8)*0x10` 4 bit/针；DIR/DO/DI `0x000/0x100/0x200` |
| 面板 | 3048×2032（显示坐标发布范围 30480×20320） |
| 触摸原始范围 | 16000×25600 |
| 固件 | `himax_boe_fw.bin`，261120 B（`0x3FC00`） |

寄存器协议（已实现于 `MtTouchSpi.cpp`）：
- 写：`[F2][reg][addr32 LE（reg!=0xFFFFDAF1）][data…]`
- 读：`[F3][reg][0][rx 数据从 rx[3] 起]`
- 32 位地址读：`0x13←0x31` → `0x0D←(addr!=0x8000002C && len>4)?0x11:0x10` → `0x00←addr32` → `0x0C←0` → 读 `0x08`
- 大块写要自己分块；`hxRegWrite` 内部上限 2048 B，`hxRegReadN` 上限 128 B。

### 3.2 按键

| 键 | 来源 | 判据 |
| --- | --- | --- |
| Volume Up | KPD 矩阵 `0x1C00E000` bit0 | 释放读 1，按下读 0；`KP_MEM1=0x04`、`KP_DEBOUNCE=0x18`、`KP_EN=0x24` |
| Volume Down | MT6377 TOPSTATUS `0x22` bit3（HOME） | 空闲高，按下低 |
| Power | MT6377 TOPSTATUS `0x22` bit1（PWRKEY） | 空闲高，按下低 |

PMIC wrapper：`0x1C804000`，MT6835 用 SPMI/WACS2：
- `0x880` 命令、`0x8A4` 清结果、`0x8A8` FSM/读数据、`0x894` 结果
- slave 4；FSM `(0x8A8>>1)&7`；idle 0、result-ready 6
- 读命令 `0xC0000000 | (4<<24) | addr`；写命令 `0xE0000000 | …`

HID report 2 位定义（`NativeHidInput.cpp`）：
- bit0 Home/Menu，bit1 Power，bit2 Volume Increment，bit3 Volume Decrement

### 3.3 其它

- 显示、电源、UFS 的既有事实见原 `HANDOVER-2026-11-17-TOUCH.md` 与 `porting.toml`。
- PMIC 的 TOPSTATUS 空闲值实测约 `0x0E`（USB 插入时）。

## 4. 接手后的修改

### 4.1 新增文件

| 文件 | 作用 |
| --- | --- |
| `package/MtButtons.cpp` | KPD + MT6377 按键轮询，发布 report 2；`buttons=1`；`native_buttons_set_sink` 真实定义 |
| `package/TouchState.cpp` | `native_touch_set_sink` / `native_touch_published` / `native_touch_publish`；seqlock 快照；`native_touch_sink_ready` |
| `package/TouchSelfTest.cpp` | `touch-selftest=<秒>`：等首帧和 sink 后按 Home、横滑，验证 OS 侧触摸链 |
| `package/himax_fw_data.h` | `gFwImg[261120]`，把 `himax_boe_fw.bin` 嵌进内核，ZF 下载直接从这里取表和数据（664682 B 源码头） |

### 4.2 修改文件

| 文件 | 改动 |
| --- | --- |
| `package/MtTouchSpi.cpp` | 本轮的主要工作，见下面的分项 |
| `package/LegacyDisplayProbe.cpp` | 在 `native_hid_input_probe()` 后调用 `tb365fc_buttons_start()` 和 `tb365fc_touch_selftest_start()` |
| `scripts/build-display-archive.py` | 编译并归档 `TouchState.o` / `MtButtons.o` / `TouchSelfTest.o`；删除 `native_touch_*` / `native_buttons_set_sink` 空桩；更新 report_sources |
| `porting.toml` | input 区更新为 `implemented`，补入按键/ZF/HW CRC/事件栈事实、启动参数、限制与下一步；修掉一个 `,,` 语法错误和 GPU 区非法的 `in-progress` 状态 |
| `HANDOVER-2026-11-17-TOUCH.md` | 追加第 10 节“后续进展（输入与 ZF）” |
| `HANDOVER-2026-11-17-INPUT.md` | 本文，随每轮结果更新 |
| `out/.../test-bootloop-20261029/build-diag279.sh` `build-diag280.sh` `build-diag281.sh` | payload + 控制台补丁 + vendor_boot 打包；281 是当前版本 |
| `out/.../test-bootloop-20261029/flash-diag279.sh` `flash-diag280.sh` `flash-diag281.sh` | 带 sha256 / 尺寸 / slot 守卫的烧写脚本（设计在 MSYS bash 下运行；在 WSL 下因 `/e/...` 路径不可用，需直接用 fastboot.exe） |

`package/MtTouchSpi.cpp` 的分项改动：

1. **ZF 分区表**：`zfWord()` 从 `gFwImg + 0x20500` 读 16 字节行；`runZfDownload()` 解析 pnum、FW entity（129024 B → `0x20000000`）和 12 个 DSRAM 段（`0x10000000..0x10006BE8`）。
2. **打包写入**：12 段按最小 SRAM 地址拼成 27632 B 零初始化镜像，一次写 `0x10000000`；不再逐段用 `0x10000000 | offset`。
3. **`hxSramCrc()`**：完整实现厂商 `himax_mcu_calc_crc_by_hw`：写 `0x80050020 = 起始地址`，写 `0x80050028 = words | 0x99<<16`，轮询 `0x80050000` bit0 落下，读 `0x80050018`；返回 0 表示一致，`0xFFFFFFFF` 表示 100 次轮询无响应。FW entity 和 cfg 各最多重写 3 次；CRC 无响应时自动回退到 64 B 采样回读。
4. **厂商前置序列**：dummy AHB 读唤醒总线 → `0x13←0x31`、`0x0D←0x10` → `0x90000018←0x55`（系统复位）→ `sense_off` 密码 `pipe 0x31 ← 0x27,0x95` → 回读 `0x900000A8`（CS central state，期望 `0x0C`）。
5. **厂商尾部序列**：`0x10000100←0`（reload enable）、`0x100006EC←0`（rawout sel）、`0x10000104←0`（sorting mode）、`0x10000694←1`（N frame）、`0x100006C0←0`（清 reload done）、`0x9000005C←0`（ctrl_fw）、拉低/释放 reset GPIO 43（`HX_RST_PIN_FUNC` 的 `ic_reset(0)`）。
6. **握手**：reset 后轮询 `0x100006C0` 直到四字节等于 `0x000072C0`（60 次 × 10 ms）；成功后读 `0x10007000`、`0x100004F4`、`0x100004F8`。
7. **阶段记录**：`gZfPhase` 记录最后完成的阶段（1 总线唤醒、2 sense_off、3 FW 写完/CRC、4 cfg 写完/CRC、5 尾部寄存器、6 reset、7 握手），任何提前返回都会保留阶段号。
8. **触摸线程**：改为等 `0x100006C0 == 0x72C0` 才启动（不再等 `0xA33A`）；`max_pt` 仍从 `0x100004F8` 读。
9. **报文解析**：按厂商 `himax_parse_report_data`：`buf[max_pt*4]` 的低 4 位是触点数，`0xFF` 表示无触点；x/y 在 `i*4`，大端；`x==0 || y==0` 视为空槽；`touch-rotate` 映射到 30480×20320。
10. **摘要行**：`printSummary()` 的最后一行为 `TSP st ph=… fw=… cf=… rl=… c6=… up=… st=… mp=… tx=…`，覆盖 ZF 阶段、两个 CRC 残差、握手次数、寄存器值、FW 状态和事件栈计数。

### 4.3 启动参数

HID/按键/自检：

```
offline-hid-input=1 sheng-touch-hid=1 buttons=1
touch-selftest=<秒>          # 可选，合成滑动
```

真实触摸/ZF 阶段：

```
touch=4 touch-rotate=1
```

说明：
- `offline-hid-input=1` 创建 `NativeHidInput`（IOHIDDevice）。
- `sheng-touch-hid=1` 给键盘设备加十分指 digitizer 描述符并注册 touch sink。
- `touch-split=1 touch-hid=1` 可额外创建独立 digitizer 服务（本阶段未用）。
- `touch-rotate`：0 竖屏、1 顺时针 90°（默认）、2 倒置、3 逆时针 90°。
- `touch=3` 仍是原来的诊断时间线；`touch=4` 是新的生产路径。

### 4.4 构建集成

`build-display-archive.py` 现在编译并归档：

```
TouchState.o  MtButtons.o  TouchSelfTest.o
```

并删除了 `tb365fc-display-stubs.c` 里原来的：

```
native_touch_seq/count/hid_reports/xs/ys/ids
native_touch_set_sink
native_buttons_set_sink
```

否则会与真实定义重复。

## 5. HX83122B ZF 逆向结论（关键）

### 5.1 描述符与表地址

固件文件偏移 0 的 16 字节表是 **Flash 镜像表**，不是 ZF 分区表。厂商描述符在文件 `0x60` 行：

- `map_code = TP_CONFIG_TABLE = 0x00000A00`
- `flash_addr = 0x20500`

所以 **ZF 分区表在 `0x20500`**。该处 `+12` 字节为 `pnum = 13`。

### 5.2 分区表内容

| 行 | 目的 SRAM | 大小 | 文件偏移 |
| --- | --- | --- | --- |
| 0 | `0x20000000` | 129024 | `0x500` |
| 1 | `0x10000000` | 256 | `0x400` |
| 2 | `0x10003740` | 4804 | `0x22100` |
| 3 | `0x10000400` | 120 | `0x21500` |
| 4 | `0x10000484` | 528 | `0x215FE` |
| 5 | `0x10000700` | 180 | `0x21F00` |
| 6 | `0x100006F0` | 16 | `0x21ED0` |
| 7 | `0x100007F0` | 32 | `0x21EE0` |
| 8 | `0x10004A08` | 2168 | `0x23600` |
| 9 | `0x10005280` | 1088 | `0x23E78` |
| 10 | `0x100059E0` | 216 | `0x24F00` |
| 11 | `0x10005AB8` | 4400 | `0x26000` |
| 12 | `0x100056C0` | 800 | `0x24B00` |

- 全部 12 个 DSRAM 段没有 overlay 标记。
- 最低地址 `0x10000000`，最高结束 `0x10006BE8`。
- 打包镜像大小 `27632`（16 字节对齐），中间空洞补 0；面板字符串落在镜像 `0x407/0x414/0x444`（SRAM `0x10000407/414/444`）。

### 5.3 正确写法

厂商 `himax_mcu_firmware_update_0f()` + `hx_0f_op_file_dirly()` 的完整顺序（本轮已在 `runZfDownload()` 中实现）：

1. `interface_on`：先做一次 dummy AHB 读，再 `0x13←0x31`、`0x0D←0x10`（大块读写时 `0x0D←0x11`）。
2. 写系统复位 `0x90000018←0x55`，停掉正在跑的 FW。
3. `_sense_off(false)`：`pipe 0x31 ← 0x27, 0x95`；回读 `0x900000A8`（CS central state），`0x0C` 表示已进 safe mode。
4. 把 row 0 的 FW entity 写到 `0x20000000`，随后做 HW CRC。
5. 把 12 个段按 `addr - 0x10000000` 拷进一块 27632 B 的零初始化缓冲，一次写 `0x10000000`，再做 HW CRC。
6. `_reload_disable(0)` → `0x10000100←0`（HX83122 的 `_addr_chk_fw_reload` 就是 `0x10000100`，不是 83102e 的 `0x10007F00`）。
7. `power_on_init`：
   - `0x100006EC←0`（rawout sel）
   - `0x10000104←0`（sorting mode，写后回读校验）
   - `0x10000694←1`（N frame）
   - `0x100006C0←0`（清 reload done）
   - `sense_on(0)`：`interface_on` → `0x9000005C←0` → `ic_reset(0)`；本板有 reset GPIO，走 `HX_RST_PIN_FUNC` 的 `pin_reset(3)`，即 GPIO43 拉低约 10 ms 再释放。
8. 轮询 `0x100006C0` 期望四字节 `0x000072C0`；这一步才是 FW 真正起来的证据。

注意：
- `0x90000048←0xEC` / 回读 `0x01EC` 是 `reload_to_active`（resume 路径）的命令，不是 boot 流程的必要步骤。
- `0xA33A@0x10007000` 不是可靠的上电判据；diag279/280 的旧代码用它，现已改为 `0x72C0`。
- `hw CRC` 寄存器：`0x80050000` 状态、`0x80050018` 结果、`0x80050020` 起始地址、`0x80050028` 命令；结果为 0 才是一致。

diag275 的失败原因：把每个段写到 `0x10000000 | offset`，后写的段覆盖先写的段。diag279 起改为打包镜像；diag281 起补齐了上面的厂商顺序。

### 5.4 事件栈与报文

- 事件栈读命令：pipe `0x30`，固定 128 B，SPI 帧 `[F3][0x30][0]`，数据从 `rx[3]` 起。
- `max_pt`：`0x100004F8` 的 byte0，默认 10。
- 厂商 `touch_info_size = (max_pt + raw_cnt_max + 1 或 2) * 4`；`raw_cnt_max` 是 IC 配置里的原始数据上限，不是 `max_pt/4`，不要用它算触点数。
- **触点数**：`buf[max_pt*4]` 的低 4 位；`0xFF` 表示无触点（厂商 `himax_parse_report_data`）。厂商另有 `coord_buf[touch_info_size-4] & 0x0F` 的 `finger_num`，但需要知道 `raw_cnt_max`。
- **坐标**：每个触点 4 字节，大端 x/y，位于 `i*4`；宽度字节在 `max_pt*4 + i`（与触点数共用首字节，厂商实现如此）。
- 原始范围：x 0..16000、y 0..25600；`x==0 || y==0` 视为空槽。
- 旧代码按 `touch_info_size - 4` 取触点数，`raw_cnt_max` 猜成 `max_pt/4`，已废弃。

## 6. 本轮烧写测试记录（diag279）

### 6.1 镜像与烧写

- 镜像：`out/tb365fc/test-bootloop-20261029/diag279-vendor_boot.img`
- 大小：61018112 B
- sha256：`1012447bf5b23ca051dc4d0be06e609eceab6d5967dc838d9e38d95bf12f468b`
- 内核：`kernel.macho` `8e7ee698bfd883e19192c6edc42e3dc3f261e1fa91691385cedbd7dfd0ef3fb5`
- 烧写：`fastboot flash vendor_boot_a`，slot a，`has-slot:vendor_boot=yes`，写入成功。
- 启动参数已核对：`offline-hid-input=1 sheng-touch-hid=1 buttons=1 touch=4 touch-rotate=1 …`。

### 6.2 观察

- 启动约 8 分钟：屏幕仅顶部两行 `dmc1t: fairyload_sl …` / `dmc1t: ClassPreviewPro …`。
- 再等 5 分钟（约 13 分钟）：屏幕仅一行：

```
TSP f2 a=00 b=00 c=00 d=00 w=00000000 c6=00000000
```

- 证据照片：`out/tb365fc/test-bootloop-20261029/cap279b_rot.png`、`cap279b_top2.png`。

### 6.3 解释

- `TSP f2` 是 `printSummary()` 的**最后一行**；diag279 里 `TSP zf2` 在它之前，被只保留最新一行的大字号控制台挤掉了。
- `TSP f2` 的 `a/b/c/d/w/c6` 来自 `gDw[0..5]`，在 stage 4 中不由 ZF 路径设置，所以全零是**预期**，不能据此判断 ZF 失败。
- `printSummary()` 能在 t≥60 打印，说明 `runZfDownload()` 已返回（没有卡在总线/轮询里）。
- 仍然**没有看到** `TSP zf2`、`TSP tx`、`btn` 行，所以：
  - ZF 是否成功未知；
  - 触摸是否出点未知；
  - 按键是否上报未知。

### 6.4 新事实

大字号控制台在实机上只保留**最新一行**（至少当前 patch 如此）。以后所有需要拍照的关键结果必须压缩到一行、并且放在最后打印。

拍照方向同样关键：
- 平板**竖放**时，50 多字符的整行落在照片的长轴上，字高约 20 px，可读（diag279 的 `cap279b_top2.png` 就是这样读出来的）。
- 平板**横放**时，同一行只剩约 22 个字符可见，字被压到约 10 px，再叠加相机对蓝色亮屏的曝光，会完全糊成色块（`cap281d_*`、`cap281g_*`）。
- 结论：**读控制台前先确认平板竖放**，否则拍多少张都没用。

### 6.5 diag281 启动观察（2026-11-17）

- 烧写成功、重启后，控制台在早期启动阶段会显示**系统自带的 verbose 文本**（`FIPSPOST KEXT … PASSED`、`Skylark: sk_copy4_64 … PASSED`、`tsp enter found=1 stage=4`、`tsp clk=6`、`tsp pp d0=1 d21=1 …`），这些在照片里是**可读的**。
- 随后画面切到蓝色/空白的 framebuffer，只剩右边缘的大字号 HUD 一行，UI 元素（图标、时钟、Dock）不可见。
- 对同一位置每 20 s 采样 8 帧，文件大小在 685–846 KB 之间波动，说明屏幕内容在“verbose 文本”和“空白蓝屏”之间反复；**疑似重启循环**，但尚未抓到 panic 行或完整的 `TSP st`。
- 已用两帧相隔 12 s 的差分确认 HUD 区域有 38% 像素变化，即内核仍在持续打印，不是完全死机。
- 下一步先解决“读回一行”的问题：竖放拍照，或在早期 verbose 阶段直接读 `tsp …` 行；如果确认循环，需要用 `--skip-panic-log` 之外的串口/panic 输出定位。

## 7. diag281（已烧写，待读结果）

diag280 没有烧写；本轮直接把它升级成 diag281，按厂商 `himax_mcu_firmware_update_0f` + `himax_mcu_power_on_init` 的顺序重写了 ZF 路径。逐项改动见 4.2，这里只记 diag281 的要点、产物与判读。

1. `runZfDownload()` 现在分 7 个阶段，`gZfPhase` 记录最后完成的阶段：
   - 1：`interface_on`（先做一次 dummy AHB 读，再 `0x13<-0x31`、`0x0D<-0x10`），然后 `0x90000018 <- 0x55` 停 FW
   - 2：`sense_off` 密码 `pipe 0x31 <- 0x27, 0x95`，回读 `0x900000A8`（期望 `0x0C`）
   - 3：写 FW entity（129024 B 到 `0x20000000`），做 HW CRC（最多 3 次）
   - 4：把 12 个 DSRAM 段打包成 27632 B 镜像写到 `0x10000000`，做 HW CRC
   - 5：`0x10000100<-0`（reload enable）、`0x100006EC<-0`、`0x10000104<-0`、`0x10000694<-1`、`0x100006C0<-0`
   - 6：`0x9000005C<-0`，然后拉低再释放 reset GPIO 43（厂商 `HX_RST_PIN_FUNC` 的 `ic_reset(0)`）
   - 7：轮询 `0x100006C0` 直到 `0x72C0`；成功后读 `0x100004F4/F8` 和 `0x10007000`
2. 新增 `hxSramCrc()`：完整实现厂商 `himax_mcu_calc_crc_by_hw` 的协议（`0x80050020` 起始地址、`0x80050028` 写 `words | 0x99<<16`、轮询 `0x80050000` bit0、读 `0x80050018`，0 = 一致）。
3. 触摸线程改为用 `0x100006C0 == 0x72C0` 判断 FW 起来（不再用 `0xA33A`），并按厂商规则解析触点：`buf[max_pt*4]` 低 4 位是触点数，`0xFF` 表示无触点。
4. `printSummary()` 最后一行改成：

```
TSP st ph=7 fw=00000000 cf=00000000 rl=1 c6=72c0 up=1 st=0000 mp=10 tx=125/125 n=1 s=1
```

字段：`ph` 最后完成阶段、`fw`/`cf` FW 与 cfg 的 HW CRC 残差（0 最好，`ffffffff` 表示 CRC 引擎无响应）、`rl` 握手轮询次数、`c6` 最后读到的 `0x100006C0`、`up` FW 是否起来、`st` `0x10007000`、`mp` max_pt、其余为事件栈计数。

产物：

- 镜像：`out/tb365fc/test-bootloop-20261029/diag281-vendor_boot.img`
- 大小：61018112 B
- sha256：`f4d886dcfb1b6029f321bba15e26ca6c7d6d20653d26990cb146084cba74b3be`
- prelink 内核：`kernel.macho` sha256 `8508aa6f71292adf5db5682893de612632dbefbab558df28f146b6db006ccd59`
- 控制台补丁后 payload 内核：`m281.macho` sha256 `5e791c4c9eb791f911359a838857d7b08c8463e2de7c02f4fb01fe6e9662e998`
- 启动参数含 `touch=4 touch-rotate=1 buttons=1 sheng-touch-hid=1 offline-hid-input=1`

已烧写到 `vendor_boot_a` 并重启（2026-11-17）。diag280 的镜像与哈希保留在目录里作历史对照（sha256 `e8f9149b…`）。


## 8. 已知风险与限制

- HX83122B 的工厂 flash 已在早期 diag267–270 的擦除实验中被改动；如果该 SKU 不是 Zero-Flash，恢复依赖 flash 写入通道，而该通道目前仍静默。
- ZF 路径现在按厂商顺序做了 `sense_off`、HW CRC、reload enable 和 reset 脉冲；尚未在硬件上确认 `0x80050018` 在 BL 阶段可读（若 `fw/cf = ffffffff` 就是不可读）。
- 触摸坐标方向未用真实手指校准；`touch-rotate` 只是可调参数。
- 按键未做真实按压测试；PMIC 读取失败会退化为仅 Volume Up。
- pogo 键盘/触控笔未实现，协议未记录。
- 大字号控制台只保留最新一行，调试输出要按“最后一行”设计。
- **拍照方向很重要**：平板竖放（portrait）时 50 多字符的整行都能落在照片里，字高约 20 px，可读；横放（landscape）时同一行只剩约 22 个字符可见，而且字被压到约 10 px，相机景深/曝光一叠加就完全糊成色块。以后读控制台务必让平板竖放。

## 9. 下一步（按优先级）

1. **读 diag281 最后一行 `TSP st`**（平板竖放拍照）：
   - `ph<7`：看停在哪一阶段；`ph=2` 且 `cs!=0c` 说明没进 safe mode。
   - `fw/cf = ffffffff`：HW CRC 引擎在 BL 阶段不可读，退回采样回读（代码已自动回退）。
   - `fw/cf 非 0`：SRAM 写入或分块有问题，先解决再谈启动。
   - `rl=0`：reset 后没有 reload，检查 reload enable（`0x10000100`）是否在 reset 前写入、reset 脉宽是否足够。
   - `rl>0` 但 `up=0`：`0x72C0` 出现了但后续寄存器读不到，检查 `sense_on` 的 `0x9000005C` 顺序。
   - `up=1`：FW 已跑，继续看 `tx/n/s`；`tx>0` 但 `n=0` 时换 `touch-rotate` 并检查原始坐标范围。
2. 如果 `up=1` 且有帧：用手指点按，确认 `n` 变化、`tx` 报告数增长，并在 iPadOS 桌面验证点击/滑动。
3. 在 `buttons=1` 下按 Power/Volume，确认 `btn` 行和系统音量/锁屏行为。
4. pogo 键盘/触控笔：单独采集协议后再做。

## 10. 安全与流程约束

- 只向 fork `LittleStar-OuO/UniXNU` 的专用分支 `tb365fc-gpu-ci` 推送，**严禁 main**；提交身份 `LittleStar-OuO <LittleStar-OuO@users.noreply.github.com>`，不改 git config；推送/刷机前先确认。
- 不删除 `E:\workspace - 副本`；不触碰用户数据/userdata。
- 烧写只动 `vendor_boot_a`；`boot_a` 的 UEFI FD 已固化，不再动。
- 每次烧写前设备需手动进 fastboot；拍照相机固定 Iriun Webcam（ffmpeg dshow）。
- 设备序列号、MAC、FRP 等标识不要写进仓库或文档。

## 11. 文件与哈希清单

### 源码

- `package/MtButtons.cpp`（新）
- `package/TouchState.cpp`（新）
- `package/TouchSelfTest.cpp`（新）
- `package/himax_fw_data.h`（新，固件数组）
- `package/MtTouchSpi.cpp`（改）
- `package/LegacyDisplayProbe.cpp`（改）
- `scripts/build-display-archive.py`（改）
- `porting.toml`（改）
- `HANDOVER-2026-11-17-TOUCH.md`（追加）
- `HANDOVER-2026-11-17-INPUT.md`（本文）

### 关键符号/行号

- `MtButtons.cpp:169` `buttonsThread()`
- `MtButtons.cpp:229` `native_buttons_set_sink()`
- `MtButtons.cpp:241` `tb365fc_buttons_start()`
- `TouchState.cpp:51` `native_touch_publish()`
- `TouchSelfTest.cpp:54` `selftestThread()`
- `TouchSelfTest.cpp:89` `tb365fc_touch_selftest_start()`
- `MtTouchSpi.cpp:1001` `hxSramCrc()`
- `MtTouchSpi.cpp:1019` `runZfDownload()`
- `MtTouchSpi.cpp:1195` `hxPipeRead()`
- `MtTouchSpi.cpp:1215` `mapPoint()`
- `MtTouchSpi.cpp:1229` `himaxTouchThread()`
- `MtTouchSpi.cpp:2088` `printSummary()`
- `MtTouchSpi.cpp:2221` `native_touch_spi_probe()`

### 镜像

| 镜像 | 大小 | sha256 |
| --- | --- | --- |
| diag279-vendor_boot.img | 61018112 | `1012447bf5b23ca051dc4d0be06e609eceab6d5967dc838d9e38d95bf12f468b` |
| diag280-vendor_boot.img | 61018112 | `e8f9149b677e9c8ea78a369c66d980c4f70aeb155eb7ec70c5b8b59cc3505f1a` |
| **diag281-vendor_boot.img（当前）** | 61018112 | `f4d886dcfb1b6029f321bba15e26ca6c7d6d20653d26990cb146084cba74b3be` |
| m279.macho（控制台补丁后） | 51736576 | `68ae7cb917fe3e78c11b0b66ca22a975e19c39437242c17f7619567a11df48a4` |
| m280.macho（控制台补丁后） | 51736576 | `11770ddb651d1b84067cd8a3cd023d2893d495555ebbb588c266873be248335d` |
| m281.macho（控制台补丁后） | 51736576 | `5e791c4c9eb791f911359a838857d7b08c8463e2de7c02f4fb01fe6e9662e998` |
| kernel.macho（diag281 prelink） | 51736576 | `8508aa6f71292adf5db5682893de612632dbefbab558df28f146b6db006ccd59` |
| payload281.img | 60817408 | `d99d36c4838a1ffc9b6cdcb363bc62016ac6db4fe541b432947e05bf5288a03c` |

## 12. 复现命令

```sh
# 1. 快速归档
cd /mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree
python3 scripts/build-display-archive.py

# 2. 内核（归档已构建时，可临时 CONFIG_XNU_BUILD_ARCHIVES=n 加速）
cd /mnt/e/workspace/Repository/UniXNU
./unixnu build kernel -D ../UniXNU-tb365fc-work

# 3. 镜像
cd out/tb365fc/test-bootloop-20261029
./build-diag281.sh

# 4. 烧写（设备手动进 fastboot）
./flash-diag281.sh        # MSYS bash；WSL 下改用 fastboot.exe 直接执行同样的守卫
```

启动参数（diag281）：

```
-v serial=3 debug=0x14e -enable_kprintf_spam
sheng-pmgr=2 native-ufs-root=1 native-ufs-diagnostics=1 native-4k-trust-cache=1
offline-mobile-graphics-load=1 offline-mobile-graphics-client=1
offline-legacy-register=1 offline-legacy-phase=4 offline-legacy-gop=1
offline-legacy-unit-scale=2 offline-legacy-pm=1 offline-legacy-validate=1
offline-legacy-transfer=1 offline-legacy-cpu-present=1 sheng-log=1
native-mali-probe=1
offline-hid-input=1 sheng-touch-hid=1 buttons=1
offline-hid-home=1 offline-hid-auto-home=480
offline-hid-auto-home-count=4 offline-hid-auto-home-interval=60
offline-key-events=1 aks=1 ml-timeout-lock-panic-scale=40
touch=4 touch-rotate=1
sheng-no-lc=1 sheng-disable-watchdogd=1
sheng-disable-jobs=com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd,com.apple.PerfPowerServices,com.apple.iomfb_fdr_loader
```

## 13. 一句话给下一位

diag281 已烧写，先把平板**竖放**并读回最后一行 `TSP st`：`ph=7` 说明七步都走完；`fw=0`、`cf=0` 说明两段 SRAM 的 HW CRC 都对上；`rl>0`、`c6=72c0`、`up=1` 才说明固件真的跑起来了；`tx/n/s` 才说明触摸进系统。任何一个不成立，按第 9 节对应分支处理；如果屏幕在 verbose 文本和空白蓝屏之间循环，先确认是不是 panic 重启，再继续调 ZF。
