# HANDOVER 2026-11-09 — TB365FC GPU 探针启动 / 诊断修正

接手对象：联想小新 Pad 12.1 (2025) / Lenovo TB365FC，MT6835 + Mali-G57 MC2。
目标系统：iPadOS 17.7.11 (21H461)，XNU 10063.141.1。

本文件只记录 **2026-11-09 这一轮**的实际改动与验证状态。历史内容见
`HANDOVER-2026-11-08-FULL.md`，但其中的两个结论必须按本文第 2 节修正后使用。

---

## 1. 一句话结论

本轮完成了"内核态直接 spawn GPU 用户态探针"的代码接线、诊断看门狗误报修正、
补丁可重放性修复，但 **没有产出任何新镜像**：归档编译未执行成功（工具审批服务故障），
内核未链接、未 prelink、未打包、未刷机，真机现场保持原状。

---

## 2. 必须先纠正的两个历史结论

### 2.1 `sys=92` 不能证明卡在 fcntl 内部

原代码只在 pid 1 进入 Unix syscall 时记录 `native_pid1_syscall` /
`native_pid1_syscall_time`，**在 syscall 返回时不更新也不清除**。
因此 `dstall sys=92 secs=62` 只能说明"很久没有观察到新的 Unix syscall 入口"，
无法区分：

- 真的阻塞在 fcntl 里；
- 已返回，之后在 Mach trap / 用户态执行 / 其它等待上停住。

本轮把输出改名为 `dlastsys sys=%u age=%llu`（`bsd/kern/kern_exec.c:7734`），
并加了成对 fcntl 日志。**不要再用 sys=92 作为根因证据。**

### 2.2 不能照搬参考板的空 envp

`kern_exec.o.json` 的实际编译记录：
`XNU_OWNER_UNION_SYSTEMOS=0`、`XNU_OWNER_CACHE_MOUNT=1`、`XNU_OWNER_CRYPTEX_FILES=0`。
参考板 A04BR3 的空 envp 建立在 union SystemOS 已启用的前提上。
本板必须把 `init_envp`（含 `DYLD_SHARED_CACHE_DIR`）传给探针，否则 dyld 找不到共享缓存。
新增函数签名因此是显式三参数：`void *process, uint64_t scratch, uint64_t init_envp`。

---

## 3. 设备当前现场（未改动）

- 设备连续运行数小时，已从 Apple logo 进入其后的"等待"图标，图标**尚未转动**，屏幕底部有一行日志。
- 本轮**未重启、未刷机、未断电**，现场保留。
- 用户环境中的 Iriun Webcam 程序（IriunWebcam.exe）在运行；但本轮**没有成功取得当前帧，也没有 OCR 出那行日志**。
- 那行日志的实际文本仍是空白项，是判断"启动是否还在推进"的第一手证据。
  拿到文本前，不要基于图标状态推断具体服务或错误码。

---

## 4. 本轮改动清单

### 4.1 新增 `package/TB365FCProbeLaunch.c`（新文件，未编译）

一次性、可选、失败不致命的板级 probe 启动入口，boot-arg 开关
`native-mali-probe-spawn=1`（默认关闭，当前 board.toml **未启用**）。

```c
int tb365fc_mali_probe_spawn(void *process, uint64_t scratch, uint64_t init_envp);
```

行为：
- `attempted` 静态标志在首次实际尝试前就置位，失败也不会反复 spawn；
- 目标路径 `/private/var/ngpu/bin/mali-vk-probe`；
- 字符串/向量通过 `copyout` 写入 init 的用户地址空间，调用真正的 XNU 接口
  `int posix_spawn(proc_t, struct posix_spawn_args *, int32_t *)`（不是 libc 版）；
- `init_envp` 非空时直接复用，空时才写一个空 envp。

scratch 偏移（1 页 4 KiB 内，互不重叠）：

| 用途 | 偏移 |
| --- | --- |
| 探针 path | 0xa00 |
| argv[2] | 0xa80 |
| 空 envp | 0xa90 |
| pid | 0xaa0 |
| owner 环境字符串 | 0xd00 |
| 环境向量 | 0xe00 |
| cryptex fallback 字符串 | 0xf00 |

> `spawn 0 (pid N)` 只代表 spawn 成功，**不代表探针跑完，更不代表 GPU 提交成功**。

### 4.2 修改 `.cache/xnu-work/bsd/kern/kern_exec.c`（仅缓存树，未固化）

1. owner 环境从 `0x400 / 0x500 / 0x600` 迁到 `0xd00 / 0xe00 / 0xf00`
   —— 避免与后续 addon 挂载（0x400..0x9ff）和 mkdir（0xc00..0xcbf）重叠。
   实际位置：`7094`、`7096`、`7154`、`7158`。
2. `load_init_program()` 内、Data 挂载成功之后、watchdog 与 launchd exec 之前插入：

```c
#if defined(XNU_NATIVE_LEGACY_DISPLAY) && XNU_NATIVE_LEGACY_DISPLAY
    extern int tb365fc_mali_probe_spawn(void *, uint64_t, uint64_t);
    set_security_token(p, proc_ucred_unsafe(p));
    (void)tb365fc_mali_probe_spawn(p, (uint64_t)scratch_addr, (uint64_t)init_envp);
#endif
```

   位置：`7763`–`7765`。
3. watchdog 文案 `dstall sys=` → `dlastsys sys=`（`7734`）。

### 4.3 修改 `.cache/xnu-work/bsd/dev/arm/systemcalls.c`（仅缓存树，未固化）

- 保留原 `native_pid1_syscall[_time]` 全局，但注释改为明确声明
  "这不代表该 syscall 仍在执行"。
- 新增 pid 1 前 64 次 `fcntl` 的**成对**日志（`201`、`207`）：

```c
dfcntl enter n=%u tid=%llu fd=%d cmd=%d
dfcntl leave n=%u tid=%llu err=%d rval=%d
```

边界说明：
- `enter` 与真正调用 fcntl 之间还有审计入口；
- `leave` 表示处理函数返回，不保证整条 syscall 返回路径走完；
- 只打前 64 次，避免刷屏。

### 4.4 修改 `gpu/probe/mali_vk_probe.c`（未重新编译、未签名）

原逻辑：第二次映射失败或出现 mismatch 时，最终仍 `return 0`（假成功）。
现在：

```c
destroyBuffer(device, buffer, NULL);
freeMemory(device, memory, NULL);
int failed = res != VK_SUCCESS || mismatches != 0;
probe("BO CPU readback %s (GPU submission not tested)\n", failed ? "FAIL" : "PASS");
if (g_log)
    fclose(g_log);
return failed ? 1 : 0;
```

同时把顺序改成先销毁 buffer 再释放其绑定 memory。

> 该探针**没有** `vkQueueSubmit`、命令缓冲录制或 fence 等待。
> 即使 PASS，也只证明 Vulkan 对象创建 + BO 分配 + CPU 映射回读，不证明 GPU 作业、渲染或 Metal。

### 4.5 新增 `patches/xnu/tb365fc-mali-probe-launch.patch`（未注册进 series）

三个 hunk：owner 环境地址迁移、环境 copyout 地址迁移、`load_init_program()` 新增板级调用。

- 第三 hunk 最初只有 4 行上下文，无法定位（`patch does not apply` at 7758）；
  补上 `{` / `thread_t watcher` / `kernel_thread_start` 三行尾随上下文后，
  `git apply --check --reverse` 对当前缓存树**返回 0**。
- **但仍不能直接用**，原因见 5.2：它依赖缓存树里尚未固化的 `native_stall_watchdog` 代码。

### 4.6 修改 `scripts/build-display-archive.py`（已同步进生成树）

在 NativeMali 之后新增 `TB365FCProbeLaunch.c` 编译，复用内核 C 编译记录
`build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json`：

- 替换 `record['file']` 为 `kits/tb365fc/TB365FCProbeLaunch.c`；
- `-o` 指向 `TB365FCProbeLaunch.o`；
- 剥掉 `-MF / -MT / -MQ / -MJ` 及其参数，避免覆盖原内核依赖/记录文件；
- `llvm-ar rcs` 成员新增该对象；
- `report_sources` 新增该源文件的 SHA-256。

生成树脚本已同步（409 行）。注意 `tree/kits/tb365fc` 是指向设备包 `package/` 的符号链接，
所以新增源文件**不需要拷贝**，放设备包即可。

### 4.7 其它

- 新增 `E:/workspace/tmp/run_archive.sh`：只是 WSL 侧短包装脚本（cd 到生成树后执行
  `/tmp/run_archive.py`），为绕开命令长度问题而写，非项目产物。
- 备份：`E:/workspace/tmp/tb365fc-diag185-systemcalls.before.c`（成功）。
  `kern_exec.c` 的备份**没有成功创建**（整文件读取返回错误对象），
  如需回退只能从缓存树的 git 状态或重新 fetch 恢复。

---

## 5. 验证状态

### 5.1 已验证

| 项 | 结果 |
| --- | --- |
| 三处内核相关源文件语法 | 用实际编译记录跑 `-fsyntax-only`，返回 0 |
| 启动补丁 vs 缓存树 | `git apply --check --reverse` 返回 0 |
| 归档脚本同步 | 生成树内 409 行，与设备包一致 |
| 新源文件可达性 | `tree/kits/tb365fc/TB365FCProbeLaunch.c` 存在（符号链接） |
| 现有归档成员 | 8 个，**不含** `TB365FCProbeLaunch.o` |

### 5.2 未验证 / 阻断

1. **归档编译未执行**。最后一次成功归档是 `2026-11-08T03:29`，
   `libNativeLegacyDisplay.late.a` 仍是旧内容。
   每次尝试运行构建命令都被审批服务拒绝：
   `Automatic approval review failed: auto review assessment was not valid JSON`。
   这是审批服务故障，不是命令不安全，也不是构建报错——**不要以为构建失败过**。
2. **补丁依赖未固化**：`native_stall_watchdog` 只存在于缓存树，
   主仓库 `patches/xnu` 里搜不到。新补丁第三 hunk 的上下文正好压在这段代码上，
   所以在干净树上重放会失败。
3. `patches/xnu/series` 仍只有一行注释，**新补丁未注册**。
4. `/tmp/prelink.sh` 已不存在，prelink 的完整参数尚未恢复。
5. probe 用户态二进制未重新编译，签名 / trust bundle / GPU addon 未更新。
6. `board.toml` 的 `kernel_args` 里只有 `native-mali-probe=1`，
   **没有** `native-mali-probe-spawn=1`。
7. `porting.toml` 未更新，记录检查器未运行。

---

## 6. 建议的下一步（按顺序）

1. **先拿现场日志**。设备不要重启。用现有 Iriun 采集取一帧当前画面，
   OCR（WSL `/usr/bin/tesseract -l eng --psm 7`）底部那行；取不到就人工抄录。
   文本决定后面是查 launchd 卡点还是查用户态阶段。
2. **消除补丁隐含依赖**：把 `native_stall_watchdog` 一并纳入补丁，或把新补丁的
   锚点改成"仅基于 offline-owner-systemos-mount.patch 之后的干净上下文"，
   然后用干净工作副本验证**正向** `git apply`（不要只做 reverse check）。
3. **跑归档编译**（审批服务恢复后）：
   ```sh
   # WSL
   cd /mnt/e/workspace/Repository/UniXNU/out/tb365fc/ipados-17.7.11-21H461/tb365fc/tree
   python3 scripts/build-display-archive.py
   llvm-ar t build/native-legacy-display/libNativeLegacyDisplay.late.a   # 应有 9 个成员
   ```
   验收：成员含 `TB365FCProbeLaunch.o`；`nm` 里能看到 `_tb365fc_mali_probe_spawn`（T）。
4. 内核链接 → prelink → 重新编译 probe 并签名/addon → payload 加
   `native-mali-probe-spawn=1` → 生成独立测试镜像。
5. 真机前**先取得刷机确认**。刷入后按顺序分别确认：
   - `tb365fc: mali-vk-probe spawning env=owner-cache`
   - `tb365fc: mali-vk-probe spawn 0 (pid N)`
   - `/tmp/mali-vk-probe.log` 里的 Vulkan 逐步结果与最后一行 `BO CPU readback ...`
   三者不能互相代替。
6. 之后才进入有界提交 + fence + 已知结果的 GPU 验证（按官方 `docs/porting/gpu.md`），
   不要提前宣布 Metal / UI 完成。

---

## 7. 约束（下一位务必遵守）

- 不要删除 `E:/workspace - 副本`。
- 不要还原、不要 clean、不要覆盖设备仓库和设备包里已有的大量现场改动
  （`git status` 有数十个未跟踪补丁，`porting.toml` / `board.toml` /
  `BootLog.cpp` / `LegacyDisplayProbe.cpp` 均有未提交修改）。
- 不要原样执行 `build-diag184.sh`：它会 rm/覆盖旧 diag184 产物；
  且其头部"由 launchd 启动探针"的说明已不适用于本方案。
- 不要重置设备、不要擦除 userdata、不要动真实用户数据。
- 不推送 main；仅允许在确认后推送专用分支 `tb365fc-gpu-ci` 到指定 fork。
- 文档与代码按 UniXNU 规范分开提交；代码变更需同步更新 `porting.toml`。

---

生成时间：2026-11-09
