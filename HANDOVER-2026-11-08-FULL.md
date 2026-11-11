# TB365FC（联想平板，MT6835 / Mali-G57）UniXNU 项目交接文档

> 写给接手的人：这份文档尽量用大白话解释“我们在做什么、做到哪、卡在哪、下一步怎么走”。
> 最后更新：2026-11-08 深夜（diag184 验证完成后）。

## 0. 2026-11-09 重要更正（先看这里）

后来拿到长录制的控制台画面后，§4 里“launchd 卡死”的结论**已被推翻**，请按本节理解：

- `dstall/dlastsys` 打印（如 `sys=92`、`sys=396`）只是“pid 1 最后一次进入的 syscall 号和
  距今秒数”。396 号是 `read_nocancel`——**一次大文件读在 ~1MB/s 的验签 UFS 上跑 60 秒以上
  完全正常**；这不是死锁，是误报（看门狗阈值太小）。
- 后续控制台画面里出现了大量 `dexec /usr/libexec/xpcproxy pid=43…65` 以及
  `dexit mergeprops pid=75 st=0`、`dexit fairplayd.R2 pid=76 st=0`——**launchd 一直在正常
  拉起用户态进程**，只是整体极慢；之后屏幕上出现 Apple logo 和会自动旋转的加载轮，也证明
  用户态是活的。
- 真正结论：**系统没卡死，只是慢**（冷 RAM Data + 验签 UFS ~1MB/s）。两份文档里所有
  “launchd 卡死”的说法都应按此更正。
- 但“iPadOS 17 的 launchd 不认 overlay plist、不枚举新 label”这条仍然成立
  （Sheng 源码注释 + 我们 diag178/179/180 的实测），所以 GPU 探针仍然要靠**内核态
  posix_spawn**（见 §10.1）来启动，不能等 launchd。
- 顺带确认了一个真 bug：owner cache 环境原先用的 scratch 0x400/0x500/0x600 与 addon
  三个 union 挂载（0x400/0x600/0x800，每个占 0x100，data 在 +0x100）**重叠**，
  diag177 之后 launchd 的 `DYLD_SHARED_CACHE_DIR` 环境会被覆盖。后来已迁移到
  0xd00/0xe00/0xf00（见 HANDOVER-2026-11-09-GPU-PROBE.md 的改动，已核实）。
- diag185（2026-11-09）：把 `package/TB365FCProbeLaunch.c` 编进显示归档（9 个成员，
  导出 `_tb365fc_mali_probe_spawn`），内核已重建/prelink（sha256 `3ac94dbe…`），
  payload 启动参数加了 `native-mali-probe-spawn=1`，等待刷机验证。
- diag185 设备结果：archive/spawn 代码在，但 `#if defined(XNU_NATIVE_LEGACY_DISPLAY)`
  这个门控在 kern_exec.c 里没有定义（该宏只给 bsd_init.o/IOUserClient.cpo），
  整段 spawn 被编掉——没有 spawn 输出。
- diag186（2026-11-09）：门控改为 kern_exec 里确实定义的 `XNU_OWNER_DATA_MOUNT=1`
  （补丁 `tb365fc-mali-probe-launch.patch` 同步修改），并用
  `llvm-nm --undefined-only kern_exec.o` 确认 `_tb365fc_mali_probe_spawn` 引用真的编入。
  **设备验证成功**：
  ```
  tb365fc: mali-vk-probe spawning env=owner-cache
  tb365fc: mali-vk-probe spawn 0 (pid 2)
  dexec /private/var/ngpu/bin/mali-vk-probe pid=2
  ```
  探针作为 pid 2 在 launchd（pid 1）之前被拉起并完成 exec；
  同期 `dfcntl enter/leave err=0` 也证明 fcntl 无阻塞。
- 现状：pid 2 已进入用户态，但长时间没有任何新 syscall 入口、无输出、无退出——
  判断为 dyld/共享缓存缺页装载（~1MB/s 根卷）。diag187 把 lastsys 观察扩展到
  pid≤2（`dlastsys pid=2 sys=… age=…`），已确认探针至少执行过 fcntl。
  下一步：给足时间（40 分钟低频录制）观察探针 main 输出；若仍无进展，
  可做一个最小 hello 探针做对照，区分“dyld 慢”与“探针自身卡住”。
- diag187 结果（长录制 + OCR）：确认 `dexit mali-vk-probe pid=2 st=256`
  ——**探针跑了并退出，exit code 1**（早期失败分支）；探针从 spawn 到退出约 1.5 小时，
  再次说明慢在 dyld/共享缓存装载。探针自己的 `/dev/console` 输出走小字体控制台，
  大字号补丁看不到，所以屏幕无输出。
- diag188（构建中）：①探针把日志改写到 `/private/var/ngpu/mali-vk-probe.log`，
  并用退出码编码失败阶段（11=dlopen 失败、12=缺 vk_icdGetInstanceProcAddr、
  13=无 vkCreateInstance、14=createInstance 失败、15=缺实例入口、16=enumerate 失败、
  17=enumerate list 失败、18=createDevice 失败、19=缺设备入口、20=createBuffer、
  21=allocateMemory、22=bindBuffer、23=mapMemory、24=回读失败；dexit 的 st=code<<8）；
  ②`TB365FCProbeLaunch.c` 增加内核日志观察线程：每 30 秒查一次日志文件，
  内容变化就 `kprintf("plog| ...")` 打到**大字号控制台**（最多 4 次）。
  新探针 cdhash `f0c1a880…` 已重建 owner trust bundle 和 addon。


---

## 1. 这个项目在干什么

目标：在一台联想 TB365FC 平板（联发科 MT6835，GPU 是 ARM Mali-G57，即 Valhall JM 架构）上，
让 **UniXNU**（一个开源的“把苹果 XNU 内核搬到非苹果设备”的项目）把 iPadOS 17.7.11 的
预链接内核（prelinked XNU）跑起来，进入用户态（launchd → SpringBoard），
并最终把 **GPU/Metal** 打通（目前卡在这一步的前置条件上）。

一句话现状：**内核和存储都通了，根文件系统能挂载、用户态能启动；但 launchd 在启动早期
卡死在一个 fcntl 系统调用里，导致它不启动任何子进程。GPU 用户态探针因此一直没机会跑。**

---

## 2. 启动链路（从上电到用户态）

```
UEFI 固件（我们自建的 UniXNU UEFI app + DarwinBoot 加载器，刷在 boot_a）
  └─ 读 vendor_boot 里的 payload（FAT 镜像，含：kernel.macho、ADT、trustcache、
     可选 GPU addon、data 种子 scratch-data.img）
       └─ DarwinBoot 校验/重定位内核到自有 512MiB 窗口，跳到 XNU 入口
            └─ XNU 早期：start_first_cpu → arm_init → arm_vm_init → 串口/控制台
                 └─ bsd_init → IOKit 匹配根设备
                      └─ ShengUfs 驱动（我们自己写的 MTK UFS 驱动）：握手 → 读根布局 →
                         校验 owner 策略 → 发布 OS / SystemOS 两个 IOMedia
                           └─ load_init_program：挂 SystemOS 缓存卷、挂 Data（RAM 种子）、
                              安装 GPU addon、union 挂载 LaunchDaemons，
                              然后 exec /bin/launchd（pid 1）
                                └─ launchd 拉守护进程 → SpringBoard → 界面
```

几个名词：
- **ADT**：相当于给 XNU 的设备树（Apple 格式），payload 构建时生成。
- **payload**：DarwinBoot 读取的 FAT 镜像，内核、信任缓存、addon、RAM 种子都装在里面。
- **prelink**：把苹果原厂 kernelcache 里的 kext 和我们编译的内核拼成最终 kernel.macho 的步骤。
- **DBRDISK1 / data 种子**：一种把 APFS 镜像按“零页压缩”打包的格式，见 §7。
- **addon**：我们打包给内核的“GPU 用户态文件包”（NGPUADD2 格式），内核会把它写进
  `/private/var/ngpu`，然后用 bindfs union 挂到系统目录上。
- **trust cache**：XNU 的代码信任机制，addon 里的二进制用 ad-hoc 签名，cdhash 在 owner
  trust bundle 里，内核启动时加载。

---

## 3. 当前进度总表

| 模块 | 状态 | 证据 |
|---|---|---|
| UEFI / DarwinBoot / payload 链 | ✅ 稳定很久 | 各 diag 都能到内核 |
| 新树内核早期启动（smp 回归） | ✅ 已修复 | diag174aj |
| 根设备等待 600 秒（恢复 diag141 行为） | ✅ 已修复 | diag175：挂根 + 用户态启动 |
| UFS 驱动 + owner 验签 + 根窗口发布 | ✅ | `d114 layout/policy/pub` 全过 |
| GPU 内核后端（MFG 电源/时钟、Mali LPAE MMU、JM 作业链） | ✅ 真机验证 | diag155–173（详见 §6） |
| GPU addon 安装 + LaunchDaemons union 挂载 | ✅ | diag177 起：`gpuaddon inst w=9 … m2=4/2` |
| GPU 用户态探针（PanVK）执行 | ❌ 没跑起来 | 原因见 §4：launchd 卡死 + iPadOS 17 的 launchd 机制 |
| launchd 启动 | ⚠️ 启动了但卡死 | diag182 只有一条 exec；diag183 无退出；diag184 抓到卡在 fcntl |
| SpringBoard / Metal 前端（NativeMaliMetal） | ❌ 未开始 | 任务 #102 |

---

## 4. 当前唯一阻塞点：launchd 卡死（以及一个大发现）

### 4.1 我们是怎么确认的
- diag182 在内核里加了 exec 观察（`dexec <path> pid=<n>`，打印前 64 次用户态 exec）。
  结果整个启动只有一条：
  `dexec /bin/launchd pid=1` —— 之后 15+ 分钟没有任何进程被 exec。
- diag183 又加了退出观察（`dexit <comm> pid=<n> st=<n>`），**没有** `dexit launchd`，
  说明 launchd 没死，是卡住了。
- diag184 加了“pid 1 系统调用看门狗”：记录 launchd 当前进入的 syscall 号和进入时间，
  内核线程每 5 秒检查，同一 syscall 60 秒没更新就打印 `dstall sys=<编号> secs=<秒数>`。
  真机结果：**`dstall sys=92 secs=62`**。
- 查 `bsd/kern/syscalls.master`：**92 号 = `fcntl`**。也就是 launchd 进了 fcntl 就没再返回。
  此时系统里没有其他进程能持有文件锁，所以更像是 fcntl 内部（很可能是参数 copyin
  触发的缺页读到了我们的 UFS/APFS 读路径）卡死，而不是普通锁等待。

### 4.2 大发现：iPadOS 17 的 launchd 根本不看我们的 overlay plist（Sheng 源码注释）
Sheng/Adreno 参考实现文件：
`E:\workspace\Repository\UniXNU-a04br3\package\A04BR3ProbeLaunch.c`，开头注释写得非常清楚：

> "iPadOS 17's launchd takes its jobs from the signed compiled cache
> (/System/Library/xpc/launchd.plist), ignores the ProgramArguments of an overlay plist
> and never enumerates a new label (UniXNU-tb128fu …: the original iomfb_fdr_loader ran
> although the GPU addon's plist of that name was visible …)."

翻译：**iPadOS 17 的 launchd 从签名的编译缓存里取任务表，忽略 overlay plist 里的
ProgramArguments，也不会枚举新标签。** 这和我们 diag178/179/180 的实测完全吻合
（新增名字不跑、顶替已有名字也不跑）。

所以他们根本不是靠 launchd 拉起 GPU 探针，而是：**在 kernel 的 `load_init_program` 里、
等 root/SystemOS/Data 挂载完成之后、exec launchd 之前，用 `posix_spawn` 直接从
pid 1（init）上下文把探针拉起来**。相关函数：`unixnu_board_launch()` →
`a04br3_powervrprobed_spawn()`：
- 启动参数开关：`a04br3-powervrprobed=1`；
- 探针路径：`/private/var/ngpu/bin/powervrprobed`（在 Data 卷上，由 addon 写入）；
- 用 init 进程 map 里的 scratch 页（0xc00..0xcbf）放路径/argv/envp，环境变量为空；
- 每次启动只拉起一次，失败只打印 errno 不致命；
- 输出：`a04br3: powervrprobed spawn %d (pid %d)`；
- 二进制通过 owner trust bundle 信任；如果 SystemOS 有 union，dyld 共享缓存就在标准路径，
  子进程不需要环境变量。

**这就是我们下一步应该照抄的方案。**（见 §10.1）

---

## 5. 代码/构建现状（重要：本机有很多“现场改动”，接手必须知道）

### 5.1 工作目录
- 设备包仓库（板级配置、GPU 探针/插件、addon 构建脚本）：
  `E:\workspace\Repository\UniXNU-tb365fc-work`
- XNU 源码树（真正的编译树）：
  `E:\workspace\Repository\UniXNU\out\tb365fc\ipados-17.7.11-21H461\tb365fc\tree`
  其中源码在 `tree\.cache\xnu-work`（git 仓库，HEAD 是 apple 的 d8b8029）。
- 生成的内核：`…\tb365fc\kernel\kernel.macho`（旁边有各 diag 的备份 `.diag175.bak` 等）。
- 测试镜像目录：`E:\workspace\Repository\UniXNU\out\tb365fc\test-bootloop-20261029`。
- 构建脚本：`E:\workspace\tmp\build-t*.sh`、`build-diag17x.sh`、`build-diag18x.sh`。
- 构建/刷机日志：`E:\workspace\log\`；拍照录像：`E:\workspace\Pictures\dXXX\`。
- **旧工作树备份（曾达到 launchd 状态的参考）**：
  `E:\workspace - 副本\Repository\UniXNU\out\tb365fc\ipados-17.7.11-21H461\tb365fc\tree\.cache\xnu-work`
  （2026-11-02 快照，别删！做行为对比很有用）

### 5.2 当前 XNU 树里“手工”做的改动（都是必要的，别随手还原）
1. **根等待 600 秒**（否则新内核 60 秒就 panic）：
   `iokit/bsddev/IOKitBSDInit.cpp` 非 DEBUG 分支 `ROOTDEVICETIMEOUT` 60→600。
   已固化成补丁：`patches/xnu/offline-owner-root-wait-timeout.patch`
   （UniXNU 主仓库和 tb365fc-work 仓库各一份，并已追加到 `/tmp/patch_order.txt`）。
2. **回退 `native-smp-secondary.patch`**（否则新树早期 arm_vm_init 直接 panic）：
   效果是 `pexpert/.../board_config.h` MAX_CPUS 回到 4、`osfmk/arm64/start.s`
   回到 `#if HAS_CLUSTER`。做法：`git apply --reverse <tree>/patches/xnu/native-smp-secondary.patch`。
3. **CRLF 坑 + GPU addon 挂载补丁**：
   树里 `bsd/kern/kern_exec.c`、`bsd/miscfs/bindfs/{bind_vfsops.c,bind_vnops.c,bindfs.h}`
   原来是 CRLF 行尾，而补丁是 LF，导致 `git apply` 一直失败（建树时被静默跳过，
   diag176 就是因此没有 addon 安装代码）。解决：先 `sed -i 's/\r$//'` 转 LF，
   再应用 `patches/xnu/zzy-native-gpu-addon-mount.patch`。
4. **zzyc-owner-skip-launch-constraints.patch 已应用**（security/mac_process.c）。
5. **没应用的 late 补丁**：`zzy-staged-system-apps.patch`、`zzyb-native-cryptex-os-view.patch`
   （与当前 kern_exec.c 上下文冲突，先跳过）；`zzz-boot-progress.patch` 也没打
   （构建脚本 `scripts/build-boot-progress.py` 在重新 clone 时丢了）。
6. **我们加的诊断代码**（都是临时的，接手可保留或删除）：
   - `bsd/kern/kern_exec.c`：
     - `native_gpu_addon_prepare()` 末尾打印
       `gpuaddon inst w=%d s=%d f=%d e=%d m1=%d/%d m2=%d/%d m3=%d/%d`
       （addon 写入数量 + 三个 union 挂载的 stage/status）；
     - `exec_activate_image()` 里 `exec_check_permissions` 之后打印
       `dexec <excpath> pid=<n>`（前 64 次；`excpath` 是内核里的路径副本，安全）。
       **注意：不要在 execve() 入口用 copyinstr 读用户指针——diag181 就是这样把内核
       弄崩的（Kernel data abort，pc 在 copyinstr）。**
     - `native_stall_watchdog()` 内核线程 + 在 `load_init_program()` 里启动；
       配合下面的 systemcalls.c 全局变量，检测 pid 1 卡住的 syscall。
   - `bsd/kern/kern_exit.c`：`exit_with_reason()` 开头打印
     `dexit <p_comm> pid=<n> st=<n>`（前 64 次）。
   - `bsd/dev/arm/systemcalls.c`：定义 `native_pid1_syscall` /
     `native_pid1_syscall_time`，在 `unix_syscall()` 里对 pid==1 更新
     （记录当前 syscall 号与进入时间）。
7. **设备包仓库的改动**：
   - `package/NativeMali.cpp`：恢复 v2 运行时——
     `MaliAs::kWindows = 4`（8MiB VA）；`boAlloc()` 自动 `allocVa` + `mapRange` +
     walk-table `IOStore`，失败回收槽位；`sBoAlloc()` 返回真实 `va`/`mmap_offset`。
     （此前是测试期的 v1：kWindows=1、bo.va=0。）
   - `gpu/make-native-gpu-addon.py`：addon 构建脚本。当前 SCRIPT 标记是 `d183`
     （重打包时记得改标记），任务列表见 §5.3。
   - `scripts/build-display-archive.py`：把 `kits/tb365fc/NativeMali.cpp` 等编进
     legacy display 的 late archive（内核用 `-force_load` 链接）。
     **注意**：现代的 `bsd_init.c` 没有 `armcpu_marker_a..d` / `d117_mark`，
     桩必须提供；如果哪天换回“移植版 bsd_init”，它会自带定义，桩就要移除，
     否则重复符号链接失败（这个坑踩过两次）。

### 5.3 GPU addon 现在长什么样（diag177 起）
`gpu/out/NativeMali.gpuaddon`（NGPUADD2，9 个文件）：
```
bin/mali-vk-probe                  (探针，自己会打开 /dev/console)
bin/libvulkan_panfrost.dylib       (Mesa PanVK，arm64 iOS17，ad-hoc 签名)
daemons/com.unixnu.mali-vk-probe.plist          (我们自己的任务名)
daemons/com.apple.iomfb_fdr_loader.plist        (顶替已有 daemon 的名字)
daemons/com.apple.bluetoothd.plist
daemons/com.apple.keybagd.plist
daemons/com.apple.cfprefsd.xpc.daemon.plist
daemons/com.apple.aggregated.plist
daemons/com.apple.logd.plist
```
每个 plist 的 ProgramArguments 都是：
`/bin/sh -c "mkdir /private/var/ngpu/.probe.lock …; echo dXXX probe-start >/dev/console;
/private/var/ngpu/bin/mali-vk-probe; echo dXXX probe-rc=$? >/dev/console"`
（用 `/private/var/ngpu/.probe.lock` 目录做互斥，保证只跑一次）。

**但注意（§4.2）：iPadOS 17 的 launchd 不认这套，这条路已经证明走不通。**
保留它不是没用——将来如果换了 launcher 或者修好 launchd，也许还能用；真正要做事的是 §10.1。

---

## 6. GPU 内核后端已经做到什么程度（真机验证过，可信）

参考官方文档 `E:\workspace\Repository\UniXNU\docs\porting\gpu.md` 的分步：

1. **匹配 GPU 世代与 OS ABI** ✅
   - GPU_ID = `0x90930010`（Mali-G57 MC2，Valhall JM），L2/MEM/MMU/JS/Shader 都能读。
   - 用户态 ABI：PanVK（Mesa）Mach-O arm64 iOS 17；内核侧 NativeMali v2 线协议。
2. **电源与固件输入** ✅
   - 从厂商 `mali_kbase_mt6835.ko`、`mtk_gpufreq_mt6835.ko` 逆向出：
     MFG0–3 上下电（SPM 寄存器、INFRA_AO、PWR_STATUS 等）、topckgen mfg_pll_sel/
     mfg_ref_sel、MFG_CFG gate 极性、MFG_TOP_CONFIG 序列。
   - G57 是 JM 架构，**没有 MCU 固件**，不需要 request_firmware。
3. **内存与命令提交** ✅
   - Mali LPAE 页表（PTE 形式 PA|0x2C1），TRANSTAB/TRANSCFG/MEMATTR 配置；
   - JM 作业：WRITE_VALUE(ZERO/IMMEDIATE_64)、NULL、CACHE_FLUSH、依赖、16 作业链；
   - 两条硬规则：**描述符必须 128 字节对齐**；**提交前 CPU 要 Store-flush 脏行**；
   - 多地址空间、重复提交、`MaliDevice` 自测（16 作业 + fence）全过；
   - `NativeMaliService`、`AGXAccelerator : IOAcceleratorES`（MetalPluginName=NativeMaliMetal）
     已发布进 IORegistry（diag170–173）。
4. **用户态与前端（Metal）** ⏳
   - 用户态已构建：PanVK 交叉编译（Mesa 26.2，pin 85fda3b3）、libdrm shim
     （`gpu/libdrm/native_drm.c`，把 open/stat/mmap 拦到 NativeMali 合成 BO）、
     探针 `gpu/probe/mali_vk_probe.c`、owner trust bundle；
   - **探针还没在设备上跑起来**（被 §4 的 launchd 卡死挡住）；
   - NativeMaliMetal 前端（对应 AdrenoMetal 插件）还没写（任务 #102）。

---

## 7. Data 卷（/private/var）从哪来（容易搞混，单独说清）

- 不是设备的 stock userdata 分区，也**不碰真实用户数据**。
- 来源：`/mnt/e/workspace/tmp/op13t-ram-data.dbr`
  （DBRDISK1 格式；sha256 `06262c5c…`；打包 1,032,304 字节；解压后 1 GiB 逻辑大小；
  5 个非零页段）。它是 diag140–142 时代生成的 “Apple APFS Data seed”，
  在 Mac 上用 hdiutil 建好 APFS 容器、写好初始结构，再用
  `drivers/DarwinBoot/Test/packed_ramdisk.py` 零页打包。文件名里的 `op13t`
  只是这套工具链早期的命名遗留，内容按 tb365fc 的 owner UUID 使用。
- payload 构建时通过 `--data-ramdisk` 打进 payload，文件是
  `Payloads/scratch-data.img`（与上面种子**逐字节一致**）。
- 启动时：DarwinBoot 把它作为 RAMDisk 交给 XNU → RamBlock 驱动展开成内存块设备
  （UUID `44516143-215D-4F9C-902A-7061DB9C292B`）→ `load_init_program()` 里的
  `native_owner_data_prepare()` 把它挂到 `/private/var`（检查 apfs、非只读、
  设备匹配，失败会 panic）。
- 影响：**每次重启都是“第一次开机”**——没有热缓存、没有偏好、launchd 每次重建数据库。
  这是启动要 40–90 分钟才能看到 Apple logo 的根本原因之一（见 §9）。

---

## 8. 构建与打包（照着做就行）

### 8.1 环境
- 所有编译都在 WSL（Ubuntu 24.04）里跑；工具链在 `/opt/unixnu-linux`。
- CLI 构建的精确环境变量快照：WSL 的 `/tmp/xnu_env.json`
  （如果丢了，可从一次 `unixnu build` 的环境里重新导出；关键变量：
  `UNIXNU_PATCH_LIST=/tmp/patch_order.txt`、`XNU_NATIVE_LEGACY_DISPLAY=1`、
  owner/ufs 一堆开关、`JOBS=8` 等）。
- **`UNIXNU_PATCH_LIST` 非空时，树脚本会跳过“补丁重打/重排”**，
  也就是说 `tree/.cache/xnu-work` 的当前状态就是权威状态，改代码就直接改它。

### 8.2 编内核
```bash
# 1) 只改了 XNU 源码时：
wsl -e bash -lc 'python3 /mnt/e/workspace/tmp/run_xnu_env.py'
#    等价于在 tree 目录下用 /tmp/xnu_env.json 环境跑 scripts/build-xnu-4k-baseline.sh

# 2) 如果改的是设备包里的 NativeMali/LegacyDisplayProbe 等（会进显示归档）：
wsl -e bash -lc 'python3 /tmp/run_archive.py'
#    注意：tree/kits/tb365fc/* 是从设备包同步过去的；最稳妥是先跑一次
#    ./unixnu build payload（它会刷新 kits），再重编内核。
```
树脚本最后只产出
`tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf`，
**不会自动生成 kernel.macho**，必须手动 prelink：

```bash
# 3) prelink（emit_prelink 拒绝覆盖已存在的文件，先把旧 kernel.macho 挪走）
grep -m1 emit_prelink /mnt/e/workspace/log/kernel-diag147-20261104.log \
  | sed 's/^+ //' > /tmp/prelink.sh          # 里面含完整的 --root 列表和 AMFI 地址
cd /mnt/e/.../tb365fc/kernel && mv kernel.macho kernel.macho.bak
wsl -e bash -lc 'bash /tmp/prelink.sh'
```

### 8.3 打包 + 刷机
```bash
# 4) 打包脚本模板：
#    build-t34.sh / build-t36.sh —— kernel-only（无 addon）
#    build-diag176.sh / build-diag177.sh / build-diag180.sh —— 带 GPU addon 的信封
#    脚本会做：mcopy 换 kernel.macho → patch-xnu-entry-probe.py --no-reset --skip-panic-log
#    → patch-xnu-console-bigtext.py（大字号，方便拍照）→ mkbootimg vendor_boot v4
wsl -e bash -lc 'bash /e/workspace/Repository/UniXNU/out/tb365fc/test-bootloop-20261029/build-diag184.sh'

# 5) 设备进 fastboot 后：
/e/workspace/msys64/ucrt64/bin/fastboot.exe flash vendor_boot_a diagXXX-vendor_boot.img
/e/workspace/msys64/ucrt64/bin/fastboot.exe reboot
```
- 打包用的关键输入：
  - owner trust：`gpu/out/native-mali-trust.bundle`（含 probe/dylib 的 cdhash）；
  - addon：`gpu/out/NativeMali.gpuaddon`；
  - Data 种子：`/mnt/e/workspace/tmp/op13t-ram-data.dbr`；
  - 额外启动参数：`--kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-no-lc=1`。
- 启动参数主列表来自设备包 `board.toml [payload].kernel_args`，当前包含：
  `-v serial=3 debug=0x14e -enable_kprintf_spam sheng-pmgr=2 native-ufs-root=1
  native-ufs-diagnostics=1 native-4k-trust-cache=1 offline-legacy-* … sheng-log=1
  native-mali-probe=1`。**`native-4k-trust-cache=1` 每次都在**（跳过 3.4GB
  共享缓存哈希，是 bring-up 加速器，别关，关掉启动会慢到小时级）。

### 8.4 观测（拍照 → 建议以后换 OCR）
- 现在：Iriun 摄像头 + ffmpeg 1fps 抓帧，存 `E:/workspace/Pictures/dXXX/w_%04d.jpg`；
  长录 0.2fps、2400 秒。屏幕是镜像/旋转的，需要先翻转再看。
- ffmpeg 有时“写完了但不退出”，会占着摄像头导致下一次打不开：
  `taskkill //F //IM ffmpeg.exe`。
- 建议接手后立刻做**自动 OCR 流水线**（tesseract），把帧直接转成可搜索文本日志，
  既省时间也省 token。
- 另一个更彻底的办法看 §9.4。

---

## 9. 已知坑 / 重要经验（省得再踩）

1. **CRLF 陷阱**：树里某些文件是 CRLF，补丁是 LF，`git apply` 会静默失败
   （脚本里还带 `>/dev/null 2>&1` 的话完全看不出来）。改法：转 LF 再打。
2. **没有 llvm-objdump / llvm-nm**：`/opt/unixnu-linux/llvm/bin` 里只有 clang。
   别用 `2>/dev/null` 去“验证”（会得到空结果误以为没命中）。
   解析 panic 地址可以用现成的小工具 `/mnt/e/workspace/tmp/ksym.py`
   （自己解析 Mach-O 符号表）。
3. **emit_prelink 拒绝覆盖**：必须先把旧 kernel.macho 改名备份。
4. **exec 观察钩子别在 execve() 入口读用户指针**：diag181 用 copyinstr(uap->fname)
   直接把内核弄崩（Kernel data abort，pc=copyinstr）。要用
   `exec_activate_image()` 里的 `excpath`（内核副本）。
5. **native-ufs-diagnostics=1 会放大 UFS 读失败**：任何运行期读失败会打印整屏
   寄存器并 panic（`NativeUfs: root window read failed @NativeUfs.cpp:1025`）。
   这是诊断行为，不代表新回归；长时间跑机时偶尔会遇到。
6. **launchd 卡死是当前的真问题**，不是 addon、不是 plist（见 §4）。
7. **1GiB RAM Data 导致每次冷启动**：应用/守护进程重读全部二进制，启动慢是必然。
   将来要提速：把 Data 持久化到真实 userdata，或至少用
   `scripts/seed-data-grow.py` / `seed-data-skip-setup.py` 做预置热缓存的种子。
8. **GPU 内核后端的两条硬规则**（改驱动/加作业时别忘）：
   描述符 128B 对齐；提交前把 CPU 写的 buffer Store-flush。
9. **显示归档桩重复符号**：现代 bsd_init 没有 `armcpu_marker_a..d`/`d117_mark`，
   桩要提供；换移植版 bsd_init 时要删桩。
10. **fastboot 卡 “waiting for any device”**：
    `taskkill //F //IM fastboot.exe`，重插 USB/重试。
11. **UFS 吞吐实测约 1MB/s**（16MiB bench 约 15 秒；394MB 策略区 1–2.5 分钟），
    这是所有“慢”的根源之一，不是 bug。
12. **Sheng 的 launchd 结论**（§4.2）：iPadOS 17 不认 overlay plist，
    不要再在 plist 注入方向上浪费设备循环。

---

## 10. 下一步怎么做（按优先级，照这个顺序干）

### 10.1 【最高优先】照抄 Sheng：内核态 posix_spawn 拉起探针（绕开 launchd）
参考文件（直接看、直接仿写）：
`E:\workspace\Repository\UniXNU-a04br3\package\A04BR3ProbeLaunch.c`

在 tb365fc 这边要做的事：
1. 在 `bsd/kern/kern_exec.c` 的 `load_init_program()` 里，**在
   `native_owner_data_prepare()` 成功之后、exec launchd 之前**，加一个
   “启动钩子”调用（对应 Sheng 的 `unixnu_board_launch()`），
   开关用新的 boot-arg（例如 `native-mali-probe-spawn=1`，也可以先复用
   `native-mali-probe=1`，避免改 board.toml 参数）。
2. 钩子里用 `posix_spawn(p, &pid, path, &argv, &envp)`：
   - path = `/private/var/ngpu/bin/mali-vk-probe`（addon 已经会写到那里）；
   - argv = {path, NULL}，envp = 空（Sheng 的注释解释了：有 owner SystemOS
     union 时 dyld 共享缓存在标准位置，子进程不需要环境变量）；
   - 用 init 进程 map 里的 scratch 页放字符串（Sheng 用 0xc00..0xcbf；
     我们的 Data 挂载已用 0xc00 附近给 mkdir，注意别撞——可以先看
     `native_gpu_addon_mount` 用的 offset 0x400/0x600/0x800 和 owner 挂载
     用的区间，挑一个空的，比如 0xd00 起，或者自己分配一小块）；
   - 每次启动只 spawn 一次；失败只打印 errno，不 panic；
   - 打印一行 `tb365fc: mali-vk-probe spawn %d (pid %d)` 方便观察。
3. 重新打包时把 addon 里 plist 相关的“顶替 daemon”脚本保留无所谓
   （launchd 不会执行，但无害）；真正起作用的是 10.1 的 spawn。
4. 验证：控制台应出现 spawn 行，随后是探针自己的输出（它打开 /dev/console，
   打印 PanVK ICD 加载、BO 分配/mmap、作业提交结果）。探针源码在
   `gpu/probe/mali_vk_probe.c`，输出前缀可自行搜。

注意事项：
- launchd 仍然卡死（fcntl），但 **posix_spawn 不需要 launchd**，
  所以这一步可以立刻把 GPU 用户态验证推进下去。
- posix_spawn 需要 init 进程（pid 1）的上下文；`load_init_program()` 正好就是
  init 进程还没 exec launchd 的时刻，Sheng 已经验证可行。

### 10.2 【并行】继续查 launchd 为什么卡在 fcntl（为将来 SpringBoard 铺路）
- 下一次构建可以在 `sys_fcntl`（`bsd/kern/kern_descrip.c`）入口给 pid 1 打印
  `dfcntl cmd=<n> fd=<n>`；看是 `F_SETLKW`（锁）、`F_FULLFSYNC`（刷盘）、
  还是 `F_SETFD` 之类，配合“卡住时是否在 copyin 缺页”判断。
- 同时可以做**静态对比**：把
  `E:\workspace - 副本\…\tree\.cache\xnu-work`（曾经能到 launchd 的旧树）
  与当前树的 `bsd/kern/kern_descrip.c`、`vfs`、`apfs`、`kern_exec.c` 做 diff，
  找出功能差异（旧树有一批 Nov-2 的本地修改，我们只移植了一部分）。
- 如果定位到是 UFS/APFS 读路径卡死（缺页 → verified read 死锁），
  再决定是修驱动还是给 fcntl 路径加超时。

### 10.3 之后按官方 GPU 文档继续
1. 探针跑通后，验证 PanVK 的 device 创建、BO/mmap 回读、作业链提交（任务 #101）；
2. 写 NativeMaliMetal 前端（Metal device discovery、pipeline、presentation），
   即官方文档“第 4 步”的前端部分（任务 #102）；
3. 最后才是 SpringBoard/UI（依赖 launchd 修复 + Metal）。

### 10.4 可选的基础设施改进（有余力再做）
- OCR 日志流水线（tesseract）替代拍照；
- Data 种子扩容/预置热缓存（`scripts/seed-data-grow.py`、`seed-data-skip-setup.py`）；
- UART 串口（拆机接 USB-TTL，最彻底的日志方案）；
- 恢复 `build-boot-progress.py`（旧树有、新树丢了），拿回 `[gpu client text]`
  之类的 boot-progress 观察。

---

## 11. 关键镜像 / 内核（备查）
| 名字 | sha256 | 说明 |
|---|---|---|
| diag175-vendor_boot.img | 3a21ab0f…b364d8 | 600s 根等待，kernel-only；首次挂根+用户态 |
| diag177-vendor_boot.img | 691f97f9…a0b4 | 首次带 addon 安装代码（w=3） |
| diag180-vendor_boot.img | 9e75d7d6…f198 | 6 个顶替 daemon 名（w=9） |
| diag182-vendor_boot.img | ab783d46…c1f5 | 安全版 dexec：只有 `dexec /bin/launchd pid=1` |
| diag183-vendor_boot.img | daf2647e…fe09 | 加 dexit：launchd 没有退出 |
| diag184-vendor_boot.img | 3d2c83c6…b861 | 加 pid1 看门狗：`dstall sys=92 secs=62`（fcntl） |
| m184.macho | 6162224e…e844 | diag184 用的内核（已打 entry-probe/console-bigtext） |

（完整 sha 都在对应 `build-diagXXX-20261108.log` 和 `build-174*/175*…` 日志里。）

---

## 12. 安全/流程约束（务必遵守）
- 只允许推送专用分支 `tb365fc-gpu-ci` 到 fork `LittleStar-OuO/UniXNU`，**绝不推 main**。
- 提交身份用 `LittleStar-OuO <LittleStar-OuO@users.noreply.github.com>`，
  不修改全局 git config。
- 刷机、推送、任何影响共享状态的操作，先和项目所有者确认。
- `E:\workspace - 副本` 里的旧工作树是重要参考，**不要删除**。

---

## 13. 一分钟看懂当前该干什么
> launchd 卡死在 fcntl（sys=92）→ 子进程一个都不起 → GPU 探针没法通过 launchd 启动。
> Sheng 的现成方案是不走 launchd：在内核 `load_init_program()` 里、挂载完成后、
> exec launchd 之前，直接 `posix_spawn("/private/var/ngpu/bin/mali-vk-probe")`。
> 照着 `A04BR3ProbeLaunch.c` 抄一个 tb365fc 版本（换路径、换 boot-arg、换 scratch 偏移），
> 重新打包刷机，控制台就会出现 PanVK 探针输出。之后再回头修 launchd，做 Metal 前端。
