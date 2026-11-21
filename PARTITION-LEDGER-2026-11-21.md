# TB365FC 分区与镜像台账（2026-11-21）

> **还原点（RESTORE POINT，2026-11-21 用户回安卓前状态）**
> - boot_a        = diag131-boot.img             sha256 bbe66952f493aac77a72c01d47388aed8a16edc86606c256b540134013109b74
> - vendor_boot_a = diag335-vendor_boot.img      sha256 9cc8c7b9caae443479ea9a017143e2785e47fed6debd49705ef0c434dd66db0c
> - userdata      = userdata-root.img (8.74 GiB) sha256 fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093 (首页4KiB bc908a6c…)
> - slot a, bootloader unlocked（永不回锁）；init_boot 为 SukiSU root，项目从未刷写
> 复原命令见第 5 节；路径 A（回安卓）见第 4 节。


本文件记录本项目（UniXNU/TB365FC）对设备分区的全部刷写历史、当前内容与本地镜像位置，
用于"回安卓"与"复原 UniXNU 状态"两条路径的可逆恢复。设备：Lenovo TB365FC
(MT6835)，当前 slot a，bootloader unlocked（**任何操作都不允许回锁**）。

## 1. 设备当前状态（2026-11-21 09:30 之后）

| 分区 | 当前内容 | 大小 | sha256 | 本地镜像 |
|---|---|---|---|---|
| boot_a | UniXNU UEFI FD（diag131） | 651,264 B | `bbe66952f493aac77a72c01d47388aed8a16edc86606c256b540134013109b74` | `out/tb365fc/test-bootloop-20261029/diag131-boot.img` |
| vendor_boot_a | diag335 payload（SPI3 bring-up probe） | 61,018,112 B | `9cc8c7b9caae443479ea9a017143e2785e47fed6debd49705ef0c434dd66db0c` | `.../diag335-vendor_boot.img` |
| userdata | iPadOS 根镜像（SHNGROOT manifest + OS/SystemOS 窗口） | 9,382,658,048 B (8.74 GiB) | `fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093`（首页 4 KiB `bc908a6c…`） | `out/tb365fc/ipados-17.7.11-21H461/tb365fc/userdata/userdata-root.img` |
| init_boot_a | SukiSU Ultra root（早于项目，**项目从未刷写**） | — | 与专有清单 pin 不一致（已知） | 无（设备唯一） |
| dtbo/vbmeta/super/lk/preloader 等 | 原厂，项目从未刷写 | — | — | 无（需时用 stock 包） |

## 2. 刷写历史汇总

| 分区 | 刷写次数与来源 |
|---|---|
| boot_a | 10 个早期脚本 diag54/56/57/58/59/60/61/62/63/64 → diag131（任务 #87，此后未变）。2026-11-21 为 M0 实验临时刷入 stock `boot.img`，同会话恢复为 diag131。 |
| vendor_boot_a | **156 次**：diag74 起每个版本（diag74…diag335）。2026-11-21 临时刷 stock `vendor_boot.img`，同会话恢复为 diag335。 |
| userdata | **1 次**：diag82（2026-10-31），按绝对偏移分段写入 `userdata-root.img`（`flash-userdata-sparse.py`，fastboot 单包 >max-download-size 问题绕行）。这次写入是 Android /data 被代换的直接原因。 |
| 其它 | 无。init_boot/dtbo/vbmeta/super/misc 从未刷写；现有工具链无 slot b 操作（早期 UEFI 菜单期曾有一次 boot_b 实验记录，已恢复，非当前工具）。 |

## 3. 关键本地镜像（全部存在，已验证）

| 镜像 | 路径 | sha256 |
|---|---|---|
| 当前 boot_a（UEFI） | `test-bootloop-20261029/diag131-boot.img` | `bbe66952…`（见上） |
| 当前 vendor_boot_a | `test-bootloop-20261029/diag335-vendor_boot.img` | `9cc8c7b9…`（见上） |
| 早期带 userdata 的 vendor_boot | `test-bootloop-20261029/diag82-vendor_boot.img` | `bac697f30decc7a56aa7d550c6703d7428f4540929d0d1573c384113d4d1616c` |
| iPadOS userdata 镜像 | `ipados-17.7.11-21H461/tb365fc/userdata/userdata-root.img` | `fd43ee70…`（见上） |
| stock boot | `device-data/tb365fc-bootloader-extract-20261029/stock/boot.img` | `52fd4ff2e328888e23190135a921fb7beae1bb088a6e7ac51d1301099484a416` |
| stock vendor_boot | 同上目录 `vendor_boot.img` | `e361c9eae8289ff84ddb84b64e90a6e3b9e83eadea72ab658ddafbc2b4713291` |
| diag74–diag335 全部 vendor_boot | `test-bootloop-20261029/diag*-vendor_boot.img` | 各版本自带 sha 守卫（flash-diagNNN.sh 内 want=） |
| diag50–diag64 boot_a 历史 | `test-bootloop-20261029/diag*-boot.img` | — |

辅助脚本：
- `test-bootloop-20261029/flash-diagNNN.sh`（每个都含 self-sha 守卫 + 槽位检查 + 只写 vendor_boot_a）
- `test-bootloop-20261029/flash-userdata-sparse.py`（userdata 分段写入；含 manifest 首页校验）

## 4. 路径 A：刷回安卓（保留可复原性）

前提：接受 **userdata 被 Android 重新格式化**（当前 iPadOS 窗口将被清除；镜像本地在，可随时重放）。

1. 设备进 fastboot（slot a，unlocked 保持）：
   ```
   fastboot flash boot_a        <stock>/boot.img          # sha 52fd4ff2…
   fastboot flash vendor_boot_a <stock>/vendor_boot.img   # sha e361c9ea…
   ```
2. 进入原厂 recovery 执行 **Wipe data/factory reset**（格式化 userdata，重建 FBE）。
   若 fastboot 支持，也可先试 `fastboot -w`；两条路都不通时再用 SP Flash Tool 全量
   下载（注意：SPFT 会按出厂 scatter 重建 GPT，属最后手段，且**仍然不会回锁**）。
3. Android 首次启动后：
   - **不要**开启系统 OTA 自动更新（避免 A/B 槽位切换写坏我们的 boot_a 布局）。
   - bootloader 保持 unlocked，不做任何 lock/relock 操作。
   - 附带收益：原厂 himax 驱动会按完整时序初始化触摸 IC，**大概率能治愈当前挂死状态**。

## 5. 路径 B：复原 UniXNU 现状（继续项目）

1. fastboot 刷回：
   ```
   fastboot flash boot_a        diag131-boot.img            # bbe66952…
   fastboot flash vendor_boot_a diag335-vendor_boot.img     # 或任意历史版本
   ```
2. 重放 userdata（8.74 GiB，分段写入）：
   ```
   python3 test-bootloop-20261029/flash-userdata-sparse.py
   ```
   脚本会校验 `userdata-root.img` 尺寸与首页 manifest sha；写入完成后 Android 不可用，
   UniXNU 链完整恢复（OS/SystemOS 窗口与原状态一致）。
3. 设备冷启动（拔 USB，长按电源 20 s，等 20 s 再开机），触摸 IC 若仍挂死按项目文档的
   PMIC `mt6377_vtp` 方案处理。

## 6. 风险与注意事项

- **绝不回锁 bootloader**：不运行任何 `fastboot flashing lock` / `oem lock`。
- userdata 镜像 8.74 GiB，本地为唯一副本，做任何 userdata 操作前先确认该文件与 sha。
- Android 运行期会写 slot metadata/misc 等元数据分区（正常行为，不影响恢复路径）。
- 恢复链依赖本台账中的镜像路径；如移动/清理磁盘，先迁移上述文件并更新本文件。
