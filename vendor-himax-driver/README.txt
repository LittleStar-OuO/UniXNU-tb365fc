原厂触摸驱动资料包（TB365FC / Himax HX83122B）
=================================================
日期：2026-11-19
设备：Lenovo TB365FC（cava），BOE 12.1" TDDI，触摸 IC = Himax HX83122B（Zero-Flash 固件方案）

文件清单
--------
1. vendor-himax-driver-full.txt   原厂驱动源码全文（16 个 .c/.h 合并成单个 txt，按文件名排序）
   - 来源：开源驱动树 hxchipset_240802（与设备内 hxchipset.ko 同源同代），已与反汇编逐函数核对
   - 主要文件：
     himax_ic_core.c        核心流程：电源序列、ZF 下载、事件栈、CRC、复位
     himax_ic_HX83102e.c    本 IC 系列配置表与 sense_on/off、system_reset、TF 刷新
     himax_common.c         驱动主体：probe、中断、报点解析、suspend/resume
     himax_platform.c       SPI 总线读写（himax_bus_read/write 帧格式）
     himax_debug.c          自检/调试命令（含 0x900000A8 复位自检流程）
2. hxchipset.ko                   设备原厂内核模块本体（AArch64，1,230,072 字节）
   - sha256 见 hxchipset-ko.sha256.txt
   - 提取自设备 vendor 分区（/vendor/lib/modules/hxchipset.ko）
3. hxchipset-ko-symbols.txt       模块符号表（llvm-nm），查找函数地址用
4. hxchipset-ko-key-disasm.txt    关键函数反汇编（arm64，含重定位），已核对：
   himax_mcu_power_on_init        电源初始化 = rawout/sorting/frame/chk2 清除 + sense_on + 0x72C0 轮询
   hx83122_system_reset           cmd reset：ctrl_fw(8B)清零 + 0x31<-{27,95} 20ms + 0x31<-0 10ms
                                  + 等 0x900000E4 = 00 02（最多 6 次）
   himax_mcu_ic_reset             level 0 在本 build 走 _system_reset（无引脚复位，HX_RST_PIN_FUNC 未编译）
   himax_mcu_read_event_stack     pipe 0x30 直读（himax_bus_read(0x30, buf, len)）
   himax_mcu_firmware_update_0f   ZF 入口：system_reset 写 + sense_off(false)
   himax_mcu_init_psl             PSL 复位（0x900000A0 <- 0）
   himax_mcu_idle_mode            0x10000488[0] 写 idle en/dis（本 SKU：dis=0x17, en=0x1F）
   himax_mcu_assign_sorting_mode  0x10000104 写入并回读校验
5. README.txt                     本文件

本 SKU 关键寄存器（从 .ko 配置表 + 反汇编实测，与本项目驱动一致）
---------------------------------------------------------------
  ctrl_fw            0x9000005C     system_reset 数据 0x55（本 build 的 cmd reset 写法见上）
  reload_to_active   0x90000048     写 0xEC，回读 EC 01 表示 BL 接受
  flag_reset_event   0x900000E4     等 00 02
  cs_central_state   0x900000A8     0x05=FW 运行 0x0C=安全模式 0x04=过渡
  chk_fw_reload2     0x100006C0     FW 启动后应为 C0 72 00 00
  chk_fw_reload      0x10000100     ZF 用 0x9AA9 禁 flash reload
  rawout_sel         0x100006EC
  set_frame          0x10000694
  sorting_mode_en    0x10000104
  event_stack        pipe 0x30       帧长 (max_pt + max_pt/4 + 2)*4 = 56B；count 在 buf[max_pt*4] 低 nibble，0xFF=无触点
  info_channel_num   0x100004F4     [2]=rx [3]=tx（本机 60/40）
  info_max_pt        0x100004F8     [0]=最大触点数（10）
  chk_irq_edge       0x10000488     [1]&1 = INT 边沿标志；[0]=idle 模式控制
  psl                0x900000A0
  HW CRC             0x80050000/18/20/28

SPI 读取注意
------------
- 读帧：[F3][pipe][0x00] 后跟数据；芯片回读整体左移 1 bit（数据位 0 位于帧位 23），
  必须按位 23 起重排对齐（本项目 hxRegRead/hxPipeRead 已实现并全量验证）
- 网络访问受限时源码树可用 gh-proxy 拉取：git clone https://v6.gh-proxy.org/https://github.com/yamikiri/hxchipset_240802.git

备注
----
- 原厂 .ko 未做任何修改；仅用于静态参考。
- 若需要固件本体（himax_boe_fw.bin，261120B）与载入流程，见项目交接文档
  E:\workspace\Repository\UniXNU-tb365fc-work\PROJECT-STATUS-2026-11-19.md 第五节。
