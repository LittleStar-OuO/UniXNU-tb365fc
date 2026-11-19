# TB365FC 输入（触摸）适配 — 项目全流程 / 进度 / 卡点 / 工具手册

日期：2026-11-19
适用：Lenovo TB365FC（cava，MT6835，12.1"）+ iPadOS 17.7.11 (21H461) 用户态 + prelinked XNU

---

## 一、项目目标

在 boot_a=UEFI FD、vendor_boot_a=payload 的自制引导链上，让 Himax HX83122B（BOE TDDI，
in-cell）触摸在 iPadOS 里正常工作：手指 → 触摸 IC 报文 → 内核 → HID → 系统手势。

## 二、完整流程（阶段划分）

```
① Boot 链        UEFI(boot_a) → payload FAT(vendor_boot_a: kernel.macho+ADT+trustcache+Data种子) → XNU → launchd
② 触摸供电/复位   TDDI 电源轨 GPIO 34/124/151（runPrep 上电）；RESET 引脚 GPIO43（本 .ko 实际不用它复位）
③ SPI 传输       GPIO 位敲：CLK68 / CS69 / MOSI70 / MISO71，~500kHz
                 寄存器协议: 写 [F2][reg][addr32?][data]；读 [F3][reg][0][..]，读取需按帧位23重对齐
④ 固件下载 ZF    vendor 精确序列（见五），下载 FW 129024B→0x20000000、cfg 27632B→0x10000000，
                 HW CRC 校验，cmd reset，reload_to_active，等 0x100006C0=C0 72（FW 启动握手）
⑤ FW 初始化      FW 信息寄存器：0x100004F4(rx/tx=60/40)、0x100004F8(pt=10)、0x10000488(irq_edge)
⑥ 事件栈读取     pipe 0x30 逐帧读 56B（max_pt=10 + raw_cnt_max=2 公式），count 在 buf[max_pt*4] 低 nibble，0xFF=无触点
⑦ 坐标/发布      原始 16000x25600 → 旋转(touch-rotate) → TouchState → ShengHidInput → IOHIDFamily
⑧ 真机验证       锁屏/主屏实际手指操作
```

## 三、当前进度（截至 2026-11-19 凌晨）

| 阶段 | 状态 | 关键证据 |
|---|---|---|
| ① Boot 链 | ✅ | 锁屏 + 自动 Home 解锁 + 主屏，lfp c=0 cls=IOMobileFramebufferUserClient stg=10 |
| ② 供电/复位 | ✅ | 面板点亮、芯片可读写 |
| ③ SPI 传输 | ✅ | `TSP za m0=0/c33ca55a m1=0 m2=0`（4B/16B/复读全对）；`t4=a5a5c33c` 精确 |
| ④ 固件 ZF | ✅ | `rl=1 c6=72c0 up=1`；a8=05；`ra=101/01ec`（reload_to_active 第 1 次即被接受） |
| ⑤ FW 初始化 | ✅ | `rx=60 tx=40 pt=10 ie=91`（模块 diag307 遥测） |
| ⑥ 事件栈 | 🔶 收尾 | 背靠背连读 200 次有 145 次非零（栈里有数据）；单次间隔读为零。正抓非零帧字节定性（diag313 的 f8/c8/ev） |
| ⑦ 坐标/发布 | ⏸ 代码就绪 | mapPoint/发布管线在案，从未收到真实报文 |
| ⑧ 真机验证 | ⏸ | — |

### 卡点详述（2026-11-19 更新）
- 固件启动成功（0x72C0、a8=05、rx=60 tx=40 pt=10），AHB 寄存器读全部正常。
- **diag311 决定性发现**：对 pipe 0x30 做 200 次背靠背连读，145 次返回非零数据；而间隔 1~8ms
  的单次读恒为零 → 事件栈是 FIFO，数据存在但单点采样总落在空窗；pipe 扫描 0x00-0x7F/0xD0-0xDF
  无其他数据管道（bp=00）。
- 当前收尾工作：抓非零帧的真实字节（diag313）并分类：
  1. 若为触摸报文（有坐标结构）→ 直接解析发布（⑦ 代码已就绪）
  2. 若为全 0xEC/0xED 等异常帧 → 按 vendor 异常恢复流程处理
  3. 若为杂散数据 → 调整读时序（连续连读直到取到有效帧）
- 已在 diag312 起把 UFS 根窗口读失败从 panic 降级为 I/O 错误返回（5 次重试；校验语义不变），
  避免测试中途设备崩溃。

## 四、工具调用手册

### 0. 环境前提
- 所有 WSL 命令必须加 `MSYS_NO_PATHCONV=1`（否则 Git Bash 会把 /mnt/e 改写成 D:/Git/mnt/e）
- 构建树：`/mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree`
  - `tree/kits/tb365fc` → 软链到 `/mnt/e/workspace/Repository/UniXNU-tb365fc-work/package`
    （**touch 代码改 package/ 即被构建吸收；不要去改 tree/kits 里面的文件本体**）
- 暂存/打包目录：`/mnt/e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029`
- 设备源仓库：`E:\workspace\Repository\UniXNU-tb365fc-work`（board.toml/porting.toml/package/drivers）

### 1. 快速构建（~2~5 分钟一轮）
```bash
# 1a. 编译 late 显示/触摸归档（含 MtTouchSpi.cpp 等）
MSYS_NO_PATHCONV=1 wsl bash -lc "cd /mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree && python3 scripts/build-display-archive.py"

# 1b. 若改了 drivers/ShengUfs/*（如 NativeUfs.cpp 的 panic 重试修复）：
MSYS_NO_PATHCONV=1 wsl bash -lc "cd .../tree && python3 scripts/build-arm-driver.py ShengUfs"
# 必须再用 llvm-ar 重打 darwin 格式归档（脚本自带的 ar 会产生 ld64 不认的 TOC）：
MSYS_NO_PATHCONV=1 wsl bash -lc "cd .../tree/build/sheng-ufs && /usr/bin/llvm-ar --format=darwin rcs libShengUfs.a NativeUfs.o original-bindings.o"

# 1c. 关归档重建、增量编内核、恢复配置
cd /e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc
sed -i 's/^CONFIG_XNU_BUILD_ARCHIVES=y/CONFIG_XNU_BUILD_ARCHIVES=n/' .config
MSYS_NO_PATHCONV=1 wsl bash -lc "cd /mnt/e/workspace/Repository/UniXNU && export UNIXNU_UEFI=/mnt/e/workspace/Repository/uefi-tb365fc && ./unixnu build kernel -D ../UniXNU-tb365fc-work" > ../../../../test-bootloop-20261029/build-NNN-kernel.log 2>&1
sed -i 's/^CONFIG_XNU_BUILD_ARCHIVES=n/CONFIG_XNU_BUILD_ARCHIVES=y/' .config
# 成功标志：日志尾行 "== kernel: .../kernel.macho sha256 <新sha>"
```

### 2. 打包 payload + vendor_boot（~2 分钟）
```bash
cd /e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029
# 从模板生成（N/M/日志名两处替换）
sed -e 's/^N=299$/N=NNN/' -e 's/^M=m299$/M=mNNN/' -e 's/build-299-payload\.log/build-NNN-payload.log/' build-diag299.sh > build-diagNNN.sh
chmod +x build-diagNNN.sh
MSYS_NO_PATHCONV=1 wsl bash -lc "cd /mnt/e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029 && ./build-diagNNN.sh"
```
- 模板内含：payload build（boot args 集）、entry-probe + bigtext 控制台补丁、mkbootimg vendor_boot v4、
  header/ADT 断言、三件 sha256 输出。boot args 关键位：
  `touch=4 zf=1/0 touch-rotate=1 offline-hid-* buttons=1 sheng-* …`
- 产物：`diagNNN-vendor_boot.img`（61018112B）、`payloadNNN.img`、`mNNN.macho`

### 3. 烧录（设备须在 fastboot；只写 vendor_boot_a）
```bash
cd .../test-bootloop-20261029
# 生成烧录脚本：从上一版复制并更新 want= 为新 sha
sed "s/^want=.*/want=$(sha256sum diagNNN-vendor_boot.img | cut -d' ' -f1)/" flash-diag307.sh > flash-diagNNN.sh
sed -i 's/diag307/diagNNN/g' flash-diagNNN.sh; chmod +x flash-diagNNN.sh
./flash-diagNNN.sh && /e/workspace/msys64/ucrt64/bin/fastboot.exe reboot
```
- flash 脚本自带 sha/尺寸/槽位（必须是 a 槽）三重校验，失败即停，不会误写。

### 4. 时序等待（重要）
- 上电→锁屏 ≈ 3~4 分钟；lateLoop 计时 t 从用户态稳定后开始（≈开机 4~5 分钟处）
- ZF 在 **t=600** 触发 → 实际约**开机后 15~16 分钟**；触摸线程 t=610 起动，
  且先等 `gZfDone`（zf=1 时 ZF 完成即走；zf=0 时白等 120s）
- 抓帧窗口：烧录重启后 **16 分钟起**，每 2~3 分钟抓一轮，直到看到 ZF 完成行与 `TSP tq` 遥测
- 长等待拆分：单次 Bash 命令不要超过 ~9.5 分钟（工具超时 10 分钟）：
  `sleep 570 && echo chunk1` → 再 `sleep 290 && echo chunk2`

### 5. 拍照（Iriun Webcam + ffmpeg，别换方案）
```bash
cd /e/workspace/tmp/cap266
# 连拍 4~6 帧，间隔 5~8 秒（大字体控制台只保留最新 ~24 行，靠连拍命中关键行）
for i in a b c d; do
  /e/workspace/msys64/ucrt64/bin/ffmpeg.exe -hide_banner -loglevel error \
    -f dshow -i video="Iriun Webcam" -frames:v 1 -y capNNN_$i.png
  sleep 6
done
# 旋转（平板竖装，必须 transpose=2）
for i in a b c d; do
  /e/workspace/msys64/ucrt64/bin/ffmpeg.exe -hide_banner -loglevel error \
    -i capNNN_$i.png -vf transpose=2 -y capNNN_${i}_rot.png
done
```
- dshow "real-time buffer too full" 警告可忽略
- 用 Read 工具查看 *_rot.png；反光/过曝就换帧

### 6. 控制台判读图例（bigtext，每 10s 打一轮 summary）
```
TSP za  m0/m1/m2 = 4B/16B/复读 写读比对失配数 + 首 dword（0/c33ca55a=读通路好）
TSP zf2 pn/fs/cs/fw/cf/rl/c6/up/ra   （rl=1 c6=72c0 up=1 = FW 已启动；ra=xxx/01ec = reload 接受）
TSP z3  t4 fw0 cf0 c0(=0x100006C0) a8 hd w12/w14
TSP tq  f cb sm a8 kh a8h rx tx pt i1 s4   （触摸线程遥测：cb=触点计数，sm=事栈校验和，
        a8=芯片状态，kh=自愈结果，a8h=a8历史，rx/tx=FW通道，pt=最大触点数，i1=INT电平，s4=4B读）
TSP st  ph fw cf rl c6 up st mp tx=帧/报 n=触点 s=hdmi? stg meta init nub ...：全局状态
lfp     每 20s 心跳：显示客户端 + zf dump（zph/fw/cf/rl/c6/up/w12..w15）
```
- 控制台只留最新行：**关键信息必须放在打印序列最后**（当前顺序：z3,z4,z5,za,zf2,tq,st）
- 需要一次性关键行（如 `TSP zr`）时，要卡在 ZF 结束后的 10 秒窗口内连拍

### 7. 常见坑
- Windows 侧 python heredoc 会静默失败（exit 49）：**用 Write 写 .py 补丁文件，再 WSL python3 跑**
- ld64 报 "archive has no table of contents"：见 1b 的 llvm-ar --format=darwin
- kernel.macho 尺寸变化 ~百 KB 属正常（归档内嵌）
- UFS 单次读失败曾在 NativeUfs.cpp:989 触发保守 panic（屏幕 panic 页）：
  diag302 起已加“窗口读失败整体重试一次”，此后再未复现
- 设备 panic 页时需强制重启再进 fastboot

## 五、vendor 精确 ZF 序列（当前实现，MtTouchSpi.cpp runZfDownload）
```
interface_on(0x13←0x31, 0x0D←0x10) → 0x90000018←0x55(system_reset)
→ ctrl_fw 0x9000005C←0xA5(DATA_FW_STOP) → 0x31←{0x27,0x95},20ms,0x31←{0},10ms,
  读 0x900000A8 直到 0x0C（safe mode，最多6次，失败则 0x80020004←0 复位TCON）
→ FW 写 0x20000000(129024B) + HW CRC(0x80050020/28/00/18) → cfg 写 0x10000000(27632B)+CRC
→ 0x10000100←0x9AA9(ZF reload disable) → rawout 0x100006EC←0 / sorting 0x10000104←0 /
  frame 0x10000694←1 / chk2 0x100006C0←0 → ctrl_fw 0x9000005C←0
→ cmd reset: 0x31←{0x27,0x95},20ms,0x31←{0},10ms, 读 0x900000E4 等 00 02（≤6次）
→ interface_on → reload_to_active: 0x90000048←0xEC 轮询回读 EC 01（≤20次）
→ 轮询 0x100006C0 等 C0 72 00 00（30×10ms）
```
- 关键寄存器（本 SKU .ko 表实测）：event_stack=0x30, chk2=0x100006C0, chk_reload=0x10000100,
  reload_to_active=0x90000048, ctrl_fw=0x9000005C, flag_reset_event=0x900000E4,
  cs_central_state=0x900000A8, rawout=0x100006EC, set_frame=0x10000694,
  info_ch_num=0x100004F4, info_max_pt=0x100004F8, irq_edge=0x10000488
- vendor 源码：WSL `/tmp/hxsrc`（yamikiri/hxchipset_240802 经 gh-proxy 克隆）；
  .ko 反汇编：`/usr/bin/llvm-objdump -d -r --disassemble-symbols=<sym> hxchipset.ko`

## 六、流程约束（务必遵守）
- 只烧 `vendor_boot_a`，绝不碰 boot_a/其它分区；fastboot 只读槽位校验通过才写
- 不触碰 userdata/真实用户数据；不删除 `E:\workspace - 副本`
- 推送只允许 fork `LittleStar-OuO/UniXNU` 的 `tb365fc-gpu-ci` 分支；提交身份
  `LittleStar-OuO <LittleStar-OuO@users.noreply.github.com>`，不改 git config；推送前征求确认
- 拍照设备与命令保持 Iriun Webcam + ffmpeg dshow + transpose=2，不更换

## 七、当前进行中
- diag313 已打包待烧（sha `a0f483e0…`）：快速连读抓第一个非零帧字节（f8=）与首个计数位非零帧
  字节（c8=），并检测全同字节异常帧（ev=）。据此决定第 ⑦ 步（解析发布）或异常恢复路径。
- 判读图例行：`TSP tq f=… cb=… a8=… fb=非零次数/最大和 ev=异常帧字节 f8=…, … c8=…, …`
