# HANDOVER 2026-11-17 — TB365FC 触控输入移植

本文只描述现状：工程结构、构建方法、已证实的事实、当前卡点、要达到的效果。不含后续计划。

---

## 1. 项目概述

- 设备：Lenovo TB365FC（代号 **cava**，MT6835/cava 平台，12.1" 平板）
- 目标系统：UniXNU 方案下启动 prelinked XNU（iPad7,1 / iPadOS 17.7.11 / 21H461 用户态）
- 整体进度：UEFI 引导链、UFS 原生驱动、根卷挂载、launchd 用户态、锁屏 + 桌面均已打通
- 本阶段唯一焦点：**触摸输入**。显示已由 LegacyFramebuffer 后端承担；离线 Home 键注入已可用（ShengHidInput）；只差手指触摸

## 2. 触摸硬件事实（已实测证实）

| 项目 | 值 |
| --- | --- |
| 触摸 IC | **Himax HX83122B**（BOE 屏 SKU 的 TDDI，屏内嵌触控） |
| 判别依据 | 芯片对 Himax 0xF3 协议有响应；IC ID 字节 buf[1]&0xFE==0x2A、buf[2]==0x12、buf[3]==0x83；固件大小 0x3FC00=261120 与 `himax_boe_fw.bin` 一致 |
| 干扰项 | 工厂 DTBO 同节点还列有 Novatek NT36532（Tianma 屏 SKU），本机不响应 Novatek 协议；早期按 Novatek 调试数周，已废弃 |
| 总线引脚 | GPIO68 CLK / 69 CS / 70 MOSI / 71 MISO / 43 RESET（**位带 SPI**，MTK GPIO 基址 0x10005000） |
| MTK GPIO 布局 | MODE=0x300+(pin/8)*0x10 4bit/针；DIR/DO/DI=0x000/0x100/0x200，32 针/库，库距 0x10 |
| TDDI 供电 | pm-enable 34、bias 124/151（runPrep 中已完成 mux+拉高），复位 43 |
| IRQ | GPIO4（未使用，全部采用轮询） |
| 面板字符串 | 固件内 `BLC_BOE12.1` / `P785_CAVA` / `HX83122-B`；.ko 内有 `cava_hx83122b_boe_vdo_cphy_dsc_90hz_lcm_drv` |

## 3. 仓库与关键文件

- 设备仓库（源码唯一真源）：`E:\workspace\Repository\UniXNU-tb365fc-work`
  - `package/MtTouchSpi.cpp` —— 全部触摸移植代码（~1200 行），编译进内核
  - `package/himax_fw_data.h` —— `himax_boe_fw.bin` 的 C 数组（261120B，生成文件）
  - `package/nt36532/NvtProtocol.c/.h` —— Novatek 协议（已确认无用，保留在构建中）
  - `scripts/build-display-archive.py` —— 编译 package/*.cpp 并归档进内核（tree/kits/tb365fc 与 package/ 是硬链接，改 package 即生效）
- UniXNU 主仓库：`E:\workspace\Repository\UniXNU`；产物树 `out/tb365fc/ipados-17.7.11-21H461/tb365fc`
- 镜像/脚本暂存目录：`E:\workspace\Repository\UniXNU\out\tb365fc\test-bootloop-20261029`
  - `build-diagNNN.sh` / `flash-diagNNN.sh` / `diagNNN-vendor_boot.img`（当前最新 diag278）
- 厂商材料（WSL /tmp，重启会丢，注意备份）：
  - `/tmp/hxsrc` —— **官方 hxchipset 驱动源码**（github `yamikiri/hxchipset_240802`，经 `https://v6.gh-proxy.org/` 克隆）
  - `/tmp/vbr/lib/modules/hxchipset.ko` —— 本机厂商驱动（1.2MB，全部分析基于它）
  - `/tmp/vbr/lib/firmware/himax_boe_fw.bin` —— 固件 261120B
  - `/tmp/fwupd.dis`、`/tmp/hx.dis`、`/tmp/prog.dis`、`/tmp/waitwip.dis` —— 关键函数反汇编
  - `/tmp/lk0.dts` —— LK 内嵌基础 DTS（topckgen@0x10000000、infracfg_ao@0x10001000 等）

## 4. 构建与烧写方法

### 4.1 内核重建（约 15–20 分钟，WSL）
```sh
cd /mnt/e/workspace/Repository/UniXNU
./unixnu build kernel -D ../UniXNU-tb365fc-work
# 产物: out/tb365fc/ipados-17.7.11-21H461/tb365fc/kernel/kernel.macho
```
快速编译校验（1 分钟，不产出镜像）：
```sh
T=/mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree
cd $T && python3 scripts/build-display-archive.py
```

### 4.2 打包 diag 镜像
```sh
cd /mnt/e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029
./build-diagNNN.sh    # 生成 payload/m.macho/diagNNN-vendor_boot.img，内含入口补丁+大字号控制台
```

### 4.3 烧写
- 设备手动重启进 fastboot（USB 序列号 GSN01234，slot a）
- `./flash-diagNNN.sh`（带 sha256 守卫，只会烧 vendor_boot_a）
- `fastboot reboot`
- 约 6–8 分钟后用 Iriun Webcam 通过 ffmpeg dshow 拍照，旋转 90° 读取持久摘要行（TSP0/TSP1/... /TSP ff/TSP f2 等）
```sh
ffmpeg -f dshow -i video="Iriun Webcam" -frames:v 1 cap.png
ffmpeg -i cap.png -vf transpose=2 cap_rot.png
```

### 4.4 启动参数要点
touch=3（启用触摸探针线程）；调试控制台 `-v serial=3 debug=0x14e -enable_kprintf_spam`；离线 Home 注入 offline-hid-* 一组。lateLoop 时间线：t=40s 执行当轮主实验，t≥390s 每 10s 重印摘要。

## 5. 已证实的厂商驱动知识

### 5.1 寄存器访问协议（位带 SPI 之上）
- 写：`[F2][reg][(addr32 LE 若 reg!=0xFFFFDAF1)][数据…]`
- 读：`[F3][reg][0][rx 数据从 rx[3] 起]`
- 32 位地址空间读：`写0x13←0x31` → `写0x0D←(addr!=0x8000002C && len>4)?0x11:0x10` → `写0x00←addr32` → `写0x0C←0` → `读0x08`
- 我们实现：`hxCmd / hxAddr / hxRegRead / hxRegReadN / hxRegWrite / hxRegWriteU32 / hxReadU32`
- SRAM 大块写入已验证可行（见 6.4）

### 5.2 固件文件格式（himax_boe_fw.bin，261120B）
- 偏移 0 起为 16 字节行表，行格式：`{u16 目的偏移, pad, u32 fw文件偏移, u32 大小, pad, u8 0x56, u8 校验}`，16 字节和 &0xFF==0
- 行 0..15：flash 镜像段（0x0200 cfg/面板串 0x3D00B；**0x0300 = 主代码 120832B**；0x0400/0x0600/0x0700/0x0900/0x0A00/0x0A01/0x0D00..03/0x0F00 等）
- 行 16..21：完整 SRAM 地址参数行（0x10000000/0x10000100/0x10000200/0x10000300/0x10000600 → 分别指向 BLC/面板ID/HX83122-B 字符串偏移），带校验
- 文件是**连续打包**：0x2100→0x1F900→0x23600→…→0x3F000
- 固件加载方式：厂商 .ko 的 himax_boot_upgrade 调用 `himax_mcu_firmware_update_0f`（Zero-Flash 路径）；另有完整 flash 烧写函数（fts_ctpm_fw_upgrade / flash_programming / chip_erase / block_erase）

### 5.3 SPI200 flash 控制器（0x800000xx）
| 寄存器 | 用途（源码实证） |
| --- | --- |
| 0x80000010 | 传输格式 `_data_flash_spi200_trans_fmt = 0x00020780` |
| 0x80000020 | 传输控制：WREN 前 0x47000000；WEL 门控读 0x42000000；wait_wip 用 0x42000003；块擦 0x67000000；页编程 0x610FF000 |
| 0x80000024 | 命令寄存器：WREN=0x06、RDSR=0x05、PP=0x02、块擦=0xD8、整片擦=0xC7、读=0x3B |
| 0x80000028 | 地址（页编程写字节地址，块擦写块字节地址） |
| 0x8000002C | 数据口（读回/写入），超过 4 字节仍用 0x0D=0x10 |
| 0x80000040 | 写前使能 0x201 |
- 厂商 WEL 门控：写 WREN 后 RDSR 读 0x2C，要求 byte0 & 0x02 置位
- 厂商 wait_wip：0x42000003 + RDSR，要求 byte0 & 0x01 == 0

### 5.4 其它已解码行为
- `0x90000048 ← 0xEC` 是 **resume/reload-to-active**（挂起恢复）语义，不是冷启动固件；本机轮询从未出现 0x01EC
- power_on_init 序列：`0x100006EC←0`、`0x10000694←1`、`0x100006C0←0`，轮询 0x100006C0 期待 0x72C0
- FW 运行握手候选：0x10007000 出现 `0xA3 0x3A`（从未观察到）
- 固件版本寄存器：0x10000404/0x10000484/0x10000400/0x10000408/0x10000414
- 系统复位：0x90000018←0x55（软复位，ZF 入口使用）
- 软复位备用：0x9000005C←含 0x9527 的 8 字节

## 6. 实测证据台账（diag 编号即镜像编号）

| diag | 实验 | 关键读数 | 结论 |
| --- | --- | --- | --- |
| 265 | 首次 flash 窗口读 | 0x00000000 处读出工厂头 `00…00 04 00 00 00 00 56 A6`（与固件文件头一致） | 窗口读可用；当时 flash 内容完好 |
| 266 | reload-to-active | 写 0x90000048=0xEC 读回 `ec 00 00 00`，byte1 恒 0；FW 状态区垃圾 | reload 是 resume 语义，不能启动固件 |
| 267 | 整片擦 C7 + 全量页编程 | 页门控 739/1020 失败；窗口回读全部失配 | 编程未生效 |
| 268 | 厂商块擦 D8(0x67…) | 擦除"成功"（e=1）；页门控 505 失败；窗口仍全零 | 状态轮询不可信；窗口读读 0 |
| 269/270 | 单页试验 + WREN 重试 + 窗口回读 | 回读全片 261120 字节皆为 0x00；门控状态字节首错 0xF1/0x00 | flash 区（或窗口映射）读不出任何数据 |
| 271/274 | 纯探针 | 0x80000010 默认 0x000E0401；0x24/0x2C 默认 0；写 0x10/0x40 后读回固定值；窗口全零 | 控制寄存器像只读镜像/只写寄存器 |
| 274 | **SRAM 段写读回** | 固件段 0x0F00（4096B@0x25000）写入 SRAM 0x10000F00，回读 **4091/4096 一致**（首差 0xFFB） | **SRAM 写通道完全可用**（这是目前最强的"活"证据） |
| 275 | 9 段 SRAM 载入 + power_on_init | by=40032；0x0F00 回读失配涨到 454（段间覆盖）；0x72C0 未出现；无 0xA33A | 段地址假设错误：0x0D00 段写到了 0x0F00 区域 |
| 276 | 5 个全地址参数行写入 | 0x10000300 回读 16/16 不符；0x10000000 读出 0x00112C94 | 这些 0x1000xxxx 参数寄存器在 FW 未运行时写不进去（或读路径不同） |
| 277 | 厂商精确 flash 流程 | WREN 后状态读（0x42000000 与 0x42000003 两种模式）均 0x00；0xFF 页试验后窗口仍全 0；镜像页 16B 仅 7 失配（其余为 0）；无 0xA33A | flash 命令通道在 BL 阶段完全静默 |
| 278 | 命令通道定位（四组对照） | a/b/c/d 全部 0x00（0x24 入口、重写格式字、0x28 入口、先电源序列）；窗口头 0x00000000；0x100006C0 读 0 | 排除了入口/时序差异；见 §7 |

## 7. 当前卡点（具体、可复述）

**一句话**：触摸芯片的固件跑不起来，输入链路从源头断掉。

拆开来说：

1. **传输层没问题**：位带 SPI + Himax 寄存器协议可用（IC 有应答、SRAM 读写在 4KB 尺度上验证一致）。
2. **固件执行未发生**：三种启动证据（0x10007000 的 0xA33A 握手、0x100006C0 的 0x72C0、FW 版本寄存器）在任何一轮 diag 中都未出现，FW 域寄存器始终读到"总线无驱动"式垃圾值。
3. **flash 写入通道静默**：按官方源码逐行复刻（格式字、WREN、RDSR、PP、块擦、wait_wip；命令字与源码逐字节一致；字节序已对照 `hx_parse_assign_cmd` 源码排除）后：
   - WREN 后 RDSR 恒为 0x00（WEL 从不置位）
   - 0xFF 页试验后窗口读仍为 0x00
   - 0x24（命令）与 0x2C（数据口）默认值恒为 0，0x10 只有固定的 0x000E0401
   - 先执行厂商 power_on_init 序列再测，也无变化
4. **Zero-Flash（SRAM）路径未闭环**：厂商 .ko 开机实际调用的是 ZF 路径（`himax_mcu_firmware_update_0f`），SRAM 写通道本身已验证可用，但：
   - 段→SRAM 地址的布局规则尚未确定（diag275 的简单"0x10000000|段偏移"造成 0x0D00 段覆盖 0x0F00 段；大段 0x0300 有 120832B，超过 DSRAM 合理容量，说明存在尚未解出的分组/重定位规则）
   - 全地址参数行（0x10000000/0x10000100/0x10000200/0x10000300/0x10000600）在 FW 未运行时写入不生效（diag276 回读全不符）
   - .ko 的 `firmware_update_0f`（6604B）尚有部分分支未解：段重叠覆盖规则、cfg 打包细节、"大段去哪"、启动收尾（turn_on_mp_func / reload_disable / power_on_init 的确切寄存器动作）

**要达到的效果**：芯片内部固件进入运行态（FW 域寄存器开始返回有效值、0xA33A 握手出现）→ 能通过既有 Himax 协议读到触摸报文（sense_on 后的事件栈/中断管道）→ 报文解析为触点事件 → 经仓库内 ShengHidInput 注入 IOHID → iPadOS 锁屏/桌面上手指点击与滑动生效。

**已知风险 / 注意**
- 本机触摸 flash 的工厂固件已在 diag267–270 的擦除实验中被动过（窗口读原为工厂头，现为全零）。原厂固件文件 `himax_boe_fw.bin` 完好在手，随时可回写（一旦写入通道恢复）。
- 若最终确认该 SKU 为 Zero-Flash（无内部 flash），上述破坏则无关紧要，问题全部集中在 ZF 段布局。
- 参考固件：`himax_boe_8M_fw.bin` 与本固件同构不同内容，别混用。

## 8. 安全与流程约束（沿用项目既有约定）

- 只可向 fork `LittleStar-OuO/UniXNU` 的专用分支 `tb365fc-gpu-ci` 推送，**严禁 main**；提交身份 `LittleStar-OuO <LittleStar-OuO@users.noreply.github.com>`（不改 git config）；共享/危险动作（刷机、推送）先确认
- 不删除 `E:\workspace - 副本`；不触碰用户数据/userdata
- 烧写只动 vendor_boot_a（脚本带 sha256 守卫 + slot 检查）；boot_a 的 UEFI FD 已固化，不再动
- 每次烧写前设备需手动进 fastboot；拍照相机固定用 Iriun Webcam（ffmpeg dshow）

## 9. 常用摘要行图例（拍照读取时对照）

- `TSP0/TSP1 ic=… tg=… rl=… fw=…`：两种 SPI 相位下的 IC ID/触发/FW 区样本
- `TSP bl …`：0x80000010 / 0x24 / 0x2C / 0x90000018 读值
- `TSP fl / f2`：窗口读与 0x3B 读法读到的 flash 头
- `TSP eq n=… v=…`：reload 命令轮询结果
- `TSP dl …` / `TSP ff …`：下载/闪存流程台账（字段含义见对应 diag 源码注释）
- `TSP f2 a= b= c= d= w= c6=`：diag278 命令通道四组对照

## 10. 2026-11-17 后续进展（输入与 ZF）

### 10.1 已实现的输入链路
- `package/MtButtons.cpp`：KPD `0x1C00E000` bit0 = Volume Up，MT6377 TOPSTATUS `0x22` bit1/bit3 = Power/Volume Down，经 PMIC wrapper `0x1C804000` 轮询，`buttons=1` 发布 report 2 bit1/2/3。
- `package/TouchState.cpp`：真实 `native_touch_set_sink` / `native_touch_published` / `native_touch_publish`，替换归档空桩。
- `package/TouchSelfTest.cpp`：`touch-selftest=<秒>` 等首帧和 sink 就绪后发 Home + 横滑，验证 OS 侧触摸链。
- `package/LegacyDisplayProbe.cpp` 在 `native_hid_input_probe()` 后启动上述两项。

### 10.2 ZF 分区表的新事实
- 厂商描述符在文件 `0x60` 行：map code `TP_CONFIG_TABLE=0x00000A00`，`flash_addr=0x20500`。
- `0x20500+12 = 13`（pnum）。行 0：FW entity `0x20000000`，129024 B，文件 `0x500`；行 1..12：DSRAM 段 `0x10000000..0x10006BE8`。
- 正确写法是把所有非 overlay 段按最低 SRAM 地址**打包成一块 27632 B 的连续镜像**再写；diag275 的逐段 `0x10000000|offset` 写法会让后段覆盖前段。
- 事件栈读法是 pipe `0x30`，固定 128 B，数据从 `rx[3]` 起；`max_pt` 在 `0x100004F8`。
- `ADDR_CHK_FW_RELOAD2` 在本 SKU 为 `0x100006C0`（期望 `0x72C0`）；reload 期望回读 `0x01EC`。
- 已实现 `runZfDownload()`（MtTouchSpi.cpp）和 stage 4：`touch=4` 在 t=40 跑 ZF，t=50 起事件栈线程，`touch-rotate` 选方向（默认 1 = 顺时针 90°）。

### 10.3 diag279
- `out/tb365fc/test-bootloop-20261029/diag279-vendor_boot.img`，61018112 B，sha256 `1012447bf5b23ca051dc4d0be06e609eceab6d5967dc838d9e38d95bf12f468b`。
- kernel.macho sha256 `8e7ee698bfd883e19192c6edc42e3dc3f261e1fa91691385cedbd7dfd0ef3fb5`，内核已含 `tb365fc_buttons_start` / `native_touch_publish` / `TSP zf2` / `TSP tx` 符号与字符串。
- 启动参数：`offline-hid-input=1 sheng-touch-hid=1 buttons=1 touch=4 touch-rotate=1` + 既有显示/aks 参数。
- 待验证：`TSP zf2 pn=13 … rl>0 c6=72c0 a>0`，以及手指触摸后 `TSP tx` 出现 `n>0`。

