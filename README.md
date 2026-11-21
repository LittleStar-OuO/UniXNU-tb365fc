# UniXNU-tb365fc

**English edition: [README.en.md](README.en.md)**

面向 **联想小新 Pad 12.1 (2025) / Lenovo TB365FC**（board `cava_prc_consumer_wifi`，
MediaTek MT6835，Android 15 原厂系统）的 UniXNU 设备仓库。

## 项目状态（2026-11-21）

已打通的主要里程碑：

- 原厂 ZUXOS 1.1.10.122 包完整解包；启动链几何、CPU/GIC/定时器、UFS 主控、
  分区布局、面板/触摸/PMIC/音频/connsys 等外设均记录在 `porting.toml`
- **UEFI 启动链**：`boot_a` = UEFI FD（diag131），自建镜像已被原厂 LK 接受并进入
  UniXNU 菜单
- **Payload 传输**：`vendor_boot_a` FAT payload（58 MiB），内核+ADT+trustcache 随包加载
- **XNU 8796 启动并进入 iPadOS 17.7.11 用户态**：launchd → SpringBoard，
  可到达解锁屏与主屏；UFS 原生驱动挂载 iPadOS 根卷/SystemOS/Data（Data 为 RAM 种子，
  失电不持久）
- **显示**：legacy IOMobileFramebuffer 路径把真实帧送上 BOE 面板
- **GPU**：Mali-G57（Valhall JM）电源序列与首个作业执行已在 XNU 内验证
- **输入（部分）**：ShengHidInput 集成，按键可用、锁屏可离线解锁

**触摸适配进行中**（BOE HX83122B TDDI）：

- 已完成：SPI 位脉冲通道、ZF 固件下载（项目 P785，`72C0` 握手）、INT 边沿活动验证
- 进行中：事件栈（`0x30`/68B）仍返回空帧；已排除帧长 56/68、IRQ 门控、
  DSRAM 镜像、运行期重武装等路径，正在把传输切到 **MTK SPI3 硬件总线**
  （参照 `ruby-ios-bringup` 的 MTK 平台做法），并吸收 `UniXNU-tb128fu`
  的 Himax 事件循环思路（三通道 INT、空闲读、发布去重）
- 已知问题：长时间猛读 SPI 会使 HX83122B 从机挂死；需完整断电（拔 USB 后长按电源）
  或由原厂驱动重新上电恢复

**分区与还原点**：`boot_a`、`vendor_boot_a`、`userdata` 的当前版本与本地镜像、
sha256 及恢复命令见 [`PARTITION-LEDGER-2026-11-21.md`](PARTITION-LEDGER-2026-11-21.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `board.toml` | Format 2 板级配置与 payload 输入（userdata 窗口、ADT、内核参数） |
| `feeds.toml` | MT6835 SoC 源；bring-up 期指向本地检出 |
| `configs/tb365fc_defconfig` | 引导期内核开关 |
| `dts/tb365fc.dts` | 板级内存映射与外设接线；包含 `mt6835.dtsi` |
| `patches/xnu/series` | 板级 XNU 补丁序列 |
| `proprietary-files.txt` | 原厂启动链文件哈希及其用途 |
| `porting.toml` | 维护者记录（文档站渲染，含各领域验证证据） |
| `package/` | 板级包（构建树中呈现为 `kits/tb365fc`），含显示/触摸/HID/GPU 探针 |
| `tools/` | 设备盘点、原厂分区拉取、boot 镜像与 FDT 解包工具 |
| `PARTITION-LEDGER-2026-11-21.md` | 分区/镜像台账与两条恢复路径 |

UEFI 固件位于独立的 `uefi-tb365fc` 检出。

## 前置条件

- 一份带 `./unixnu` 与 Python 3 的 UniXNU 构建环境
- 同级 `UniXNU-mt6835-common` 检出（发布前应将 `feeds.toml` 的 `local` 换成固定 commit）
- 原厂 IPSW `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`（OS profile 自动获取）
- 原厂 TB365FC 固件包；分区取证需要已 root 的 Android（本机 userdata 已被 iPadOS
  根镜像占用，取整链路见台账）

## 构建与刷写

在 UniXNU 检出中运行：

```sh
./unixnu defconfig tb365fc_defconfig -D ../UniXNU-tb365fc-work
./unixnu build kernel -D ../UniXNU-tb365fc-work
./unixnu status --json -D ../UniXNU-tb365fc-work
```

刷写纪律（详见台账）：

- 只写 `vendor_boot_a`（每个 `flash-diagNNN.sh` 自带 sha 守卫与槽位检查）
- `boot_a` 仅在升级 UEFI FD 时更新（当前 diag131）
- **绝不回锁 bootloader**；不动 `init_boot`/`dtbo`/`vbmeta`/`super`
- userdata 仅在重放 iPadOS 根镜像时按分段脚本写入

参考文档：[设备仓库](https://unixnu.com/zh/porting/device-repository)、
[设备树](https://unixnu.com/zh/porting/device-tree)、
[启动验证](https://unixnu.com/zh/porting/bring-up)。

## 触摸适配参考

- `lky005/ruby-ios-bringup`（红米 Note 12 Pro 5G / MT6877）：MTK SPI3 0x11013000
  硬件总线、EINT 子控制器、持久化日志读取方法
- `xVanTuring/UniXNU-tb128fu`（联想小新 Pad 2022 / HX83102E）：Himax ZF 协议、
  三通道 INT 事件循环、主机侧单测

## 隐私

设备信息转储与 `out/` 下的内容仅供本地使用。不要提交序列号、Wi-Fi/蓝牙 MAC、
FRP 数据或提取的固件 blob；`proprietary-files.txt` 只记录哈希。
