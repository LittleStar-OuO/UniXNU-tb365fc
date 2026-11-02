// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production command path. Included inside the root transport namespace.
// The host harness replaces only MMIO, DMA synchronization, clock and lock primitives.
struct HardwareTransferPort {
  uint64_t deadline;
  uint32_t doorbell () { return read32 (gHost.hc, 0x58); }
  uint32_t interrupt () { return read32 (gHost.hc, 0x20); }
  uint32_t completedOcs () {
    cacheInvalidate (gHost.virt, 64);
    return *reinterpret_cast<volatile uint32_t *> (gHost.virt + 8) & 0xff;
  }
  bool expired () { return mach_absolute_time () >= deadline; }
  void delay () { IODelay (2); }
};

static void putBe32 (uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t> (value >> 24);
  at[1] = static_cast<uint8_t> (value >> 16);
  at[2] = static_cast<uint8_t> (value >> 8);
  at[3] = static_cast<uint8_t> (value);
}

static uint32_t loadBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// Issue the UPIU already placed at gHost.virt + kUcdOffset through request slot 0. `direction` is
// the UTRD data direction (0 none, 1 from host, 2 to host). Direction 1 is reachable only through hostWrite4k, which
// confines it to the persistent store window. Returns the overall command status
// (0 = completed) in *ocs. Timeout/fatal status quarantines the allocation.
// diag93 diagnostic only: moves the PRD destination inside the allocation to test address dependence.
static uint64_t gProbeDataShift = 0;

static IOReturn hostSubmit (uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint32_t *ocs)
{
  *ocs = 15;
  if (!timeoutUs || dataBytes > kDataMax || (dataBytes & 3) ||
      (dataBytes ? (direction != 1 && direction != 2) : direction != 0)) return kIOReturnBadArgument;
  if (gHost.poisoned) return kIOReturnNotReady;
  if (read32 (gHost.hc, 0x58) != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnBusy, *ocs);
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnIOError, *ocs);
  }
  gRootDiagnostic.command++;
  gRootDiagnostic.type = gHost.virt[kUcdOffset];
  gRootDiagnostic.lun = gHost.virt[kUcdOffset + 2];
  gRootDiagnostic.opcode = gHost.virt[kUcdOffset + (gRootDiagnostic.type == 1 ? 16 : 12)];
  gRootDiagnostic.scsi = 0xffff;
  gRootDiagnostic.response = 0;
  gRootDiagnostic.protocol = 0;
  gRootDiagnostic.residual = 0;
  gRootDiagnostic.sense = 0;
  uint8_t *utrd = gHost.virt;
  uint64_t ucdPhys = gHost.phys + kUcdOffset;
  uint32_t *words = reinterpret_cast<uint32_t *> (utrd);
  for (unsigned index = 0; index < 8; index++) {
    words[index] = 0;
  }
  words[0] = 0x10000000U | (direction << 25);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  words[7] = (dataBytes ? 1U : 0U) | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint64_t dataPhys = gHost.phys + kDataOffset + gProbeDataShift;
    uint32_t *prd = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset + kPrdtOffset);
    prd[0] = static_cast<uint32_t> (dataPhys);
    prd[1] = static_cast<uint32_t> (dataPhys >> 32);
    prd[2] = 0;
    prd[3] = dataBytes - 1;
  }
  size_t syncBytes = static_cast<size_t> (kDataOffset + gProbeDataShift + dataBytes);
  cacheCivac (gHost.virt, syncBytes);
  hostWrite (0x20, 1);  // Acknowledge completion only; preserve error evidence.
  HardwareTransferPort port;
  static unsigned d112Submits = 0;
  static unsigned d114Submits = 0;
  bool verb = d112Submits < 16;
  if (verb) {
    d112Submits++;
    kprintf ("d112 hs%u pre db=%x is=%x t=%llx\n", d112Submits,
        read32 (gHost.hc, 0x58), read32 (gHost.hc, 0x20), mach_absolute_time ());
  } else if ((++d114Submits & 255) == 0) {
    // diag114: silent submits hid where the root I/O loop went. Every 256th one
    // names opcode, LUN, direction, bytes and LBA (READ/WRITE 10/16).
    const uint8_t *cdb = reinterpret_cast<const uint8_t *> (gHost.virt) + kUcdOffset + 16;
    uint32_t op = gRootDiagnostic.opcode;
    uint64_t lba = 0;
    if (op == 0x28 || op == 0x2a) {
      lba = (uint64_t (loadBe32 (cdb + 2)) << 8) | cdb[6];
    } else if (op == 0x88 || op == 0x8a) {
      lba = (uint64_t (loadBe32 (cdb + 2)) << 32) | loadBe32 (cdb + 6);
    }
    kprintf ("d114 s%u op=%x lun=%u dir=%u n=%u lba=%llx t=%llx\n", d114Submits,
        op, gRootDiagnostic.lun, direction, dataBytes, lba, mach_absolute_time ());
  }
  // Arm the 200 ms budget after the diagnostic print: rendering the console
  // line can consume the whole deadline while the doorbell is still 0.
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, &port.deadline);
  hostWrite (0x58, 1);
  UfsTransferPoll state;
  UfsTransferEnd end = pollUfsTransfer (port, state);
  if (verb) {
    kprintf ("d112 hs%u end=%d ocs=%x polls=%u db=%x\n", d112Submits,
        static_cast<int> (end), state.ocs, state.polls, state.doorbellSeen);
  } else if (end != UfsTransferEnd::Completed || state.ocs != 0) {
    kprintf ("d114 bad end=%d ocs=%x polls=%u db=%x op=%x n=%u t=%llx\n",
        static_cast<int> (end), state.ocs, state.polls, state.doorbellSeen,
        gRootDiagnostic.opcode, dataBytes, mach_absolute_time ());
  }
  gRootDiagnostic.polls = state.polls;
  gRootDiagnostic.dbSeen = state.doorbellSeen;
  gRootDiagnostic.isSeen = state.interruptSeen;
  *ocs = state.ocs;
  if (end != UfsTransferEnd::Completed) {
    // No cancel/reset/retry: the controller may still own the buffer, even
    // with DB=0. Keep it allocated and prohibit all subsequent CPU writes.
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (end == UfsTransferEnd::Timeout ? kIOReturnTimeout : kIOReturnIOError, *ocs);
  }
  cacheInvalidate (gHost.virt, syncBytes);
  gRootDiagnostic.response = loadBe32 (gHost.virt + kUcdOffset + kRspOffset);
  return recordTransport (*ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
}

static bool hostClearUcd (void)
{
  if (gHost.poisoned) return false;
  // Check before touching the command/response area, not just before ringing.
  if (read32 (gHost.hc, 0x58) != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnBusy, 15);
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnIOError, 15);
    return false;
  }
  uint32_t *words = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset);
  for (unsigned index = 0; index < (kPrdtOffset + 16) / 4; index++) {
    words[index] = 0;
  }
  return true;
}

static IOReturn hostProtocolError (uint32_t reason)
{
  gRootDiagnostic.protocol = reason;
  gHost.ready = false;
  gHost.poisoned = true;
  return recordTransport (kIOReturnIOError, gRootDiagnostic.ocs);
}

// ---- MTK UIC link bring-up -----------------------------------------------------------------------------------------
// LK's power-off notify cuts the device rails and parks the link: diag85 showed every UIC DME command completing while
// LINKSTARTUP returned result 1 on all six attempts and the SSU to 0xD0 got OCS 5, i.e. there was no device on the
// wire. The MTK UEFI driver powers the device back through an EL3 SMC call set (MTCMOS, SRAM, rails with a reset
// pulse), releases the PDN/AO resets, unisolates the M-PHY, requests REFCLK and enables XOUFS before the link
// startup. Attributes are encoded as UIC_ARG_MIB(value) = value << 16, as in Linux ufs-mediatek and UfsMtkDxe.

// MTK_SIP_UFS_CONTROL through the secure monitor, the call set UfsMtkDxe makes at PreHce. The returned x0 is the
// per-call status; callers record it, since a command the EL3 firmware does not know comes back non-zero.
static uint32_t mtkSmc (uint32_t command, uint32_t arg1, uint32_t arg2)
{
  register uint64_t x0 __asm__ ("x0") = 0x82000276ULL;
  register uint64_t x1 __asm__ ("x1") = command;
  register uint64_t x2 __asm__ ("x2") = arg1;
  register uint64_t x3 __asm__ ("x3") = arg2;
  __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3)
                    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                      "cc", "memory");
  return static_cast<uint32_t> (x0);
}

// REFCLK request and XOUFS enable, rerun after an HCE reset in case the reset cleared them.
static void hostMtkClockOn (void)
{
  kprintf ("d109 q0 m=%llx\n", mach_absolute_time ());
  write32 (gHost.hc, 0x144, read32 (gHost.hc, 0x144) | 1);       // REFCLK_REQUEST
  kprintf ("d109 q1\n");
  uint32_t ack = 0, reg = 0;
  unsigned attempt = 0;
  for (; attempt < 300; ++attempt) {
    reg = read32 (gHost.hc, 0x144);
    ack = ((reg >> 1) & 1) == (reg & 1);                          // REFCLK_ACK follows REQUEST
    if (ack) break;
    IODelay (10);
  }
  kprintf ("d109 q2 ack=%u n=%u reg=%x\n", ack, attempt, reg);
  gRootDiagnostic.phyAck = ack;
  write32 (gHost.hc, 0x140, read32 (gHost.hc, 0x140) | 0x80);    // XOUFS enable
  gRootDiagnostic.xoufs2 = read32 (gHost.hc, 0x140);
  kprintf ("d109 q3 x=%x\n", gRootDiagnostic.xoufs2);
}

// Power the UFS device up the way UfsMtkDxe does before HCE: EL3 turns MTCMOS and SRAM on and gives the device rails
// a reset pulse, then the M-PHY leaves isolation, the 26 MHz reference is requested and XOUFS enabled. The host-side
// resets are released separately in hostResetController; without this the link startup has no peer to talk to.
static void hostMtkPowerUp (void)
{
  kprintf ("d113 u0 m=%llx\n", mach_absolute_time ());
  gRootDiagnostic.smc[0] = mtkSmc (1u << 9, 1, 0);        // MTCMOS on
  kprintf ("d113 u1 s=%x m=%llx\n", gRootDiagnostic.smc[0], mach_absolute_time ());
  gRootDiagnostic.smc[1] = mtkSmc (1u << 5, 1, 0);        // UFS SRAM on
  kprintf ("d113 u2 s=%x m=%llx\n", gRootDiagnostic.smc[1], mach_absolute_time ());
  gRootDiagnostic.smc[2] = mtkSmc (1u << 7, 1, 0x310);    // device rails on
  kprintf ("d113 u3 s=%x m=%llx\n", gRootDiagnostic.smc[2], mach_absolute_time ());
  IODelay (2000);
  gRootDiagnostic.smc[3] = mtkSmc (1u << 1, 0, 0);        // device reset assert
  kprintf ("d113 u4 s=%x m=%llx\n", gRootDiagnostic.smc[3], mach_absolute_time ());
  IODelay (20);
  gRootDiagnostic.smc[4] = mtkSmc (1u << 1, 1, 0);        // device reset deassert
  kprintf ("d113 u5 s=%x m=%llx\n", gRootDiagnostic.smc[4], mach_absolute_time ());
  IODelay (12000);
  kprintf ("d113 u6 m=%llx\n", mach_absolute_time ());

  OSSharedPtr<IOMemoryMap> pdnMap, aoMap, phyMap;
  uintptr_t pdn = mapPhys (UFS_PDN_BASE, 0x1000, pdnMap);
  uintptr_t ao = mapPhys (UFS_AO_BASE, 0x1000, aoMap);
  uintptr_t phy = mapPhys (UFS_PHY_BASE, UFS_PHY_SIZE, phyMap);
  // Read-only gate evidence. diag86 answered UFSHCI and UIC with status 0x04 = 0, so on this block 0 means the
  // clocks are running; never touch set/clear 0x08/0x0c here (a wrong bit gates the whole controller).
  gRootDiagnostic.gateAo = ao ? read32 (ao, 0x04) : 0xffffffff;
  gRootDiagnostic.gatePdn = pdn ? read32 (pdn, 0x04) : 0xffffffff;
  if (phy) {
    uint32_t reg = read32 (phy, 0x8c);   // PLL and CDR out of isolation, RX squelch on
    write32 (phy, 0x8c, (reg | (1u << 11)) & ~((1u << 10) | (1u << 9) | (1u << 8)));
    reg = read32 (phy, 0xb044);
    write32 (phy, 0xb044, (reg | (1u << 18)) & ~((1u << 20) | (1u << 19) | (1u << 17)));
    reg = read32 (phy, 0xa0ac);
    write32 (phy, 0xa0ac, (reg | (1u << 1)) & ~1u);
    IODelay (10);
    write32 (phy, 0xa09c, read32 (phy, 0xa09c) & ~(1u << 18));   // release the DIFZ force
    IODelay (10);
    gRootDiagnostic.phy8c = read32 (phy, 0x8c);
  }
  hostMtkClockOn ();
  kprintf ("d113 u7 m=%llx\n", mach_absolute_time ());
  gRootDiagnostic.aoStatus2 = ao ? read32 (ao, 0x04) : 0xffffffff;
  gRootDiagnostic.pdnStatus2 = pdn ? read32 (pdn, 0x04) : 0xffffffff;
  kprintf ("d113 u8 m=%llx\n", mach_absolute_time ());
}

static bool hostWaitMaskFor (unsigned offset, uint32_t mask, uint32_t value, uint32_t timeoutUs)
{
  uint64_t deadline;
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, &deadline);
  do {
    if ((read32 (gHost.hc, offset) & mask) == value) return true;
    IODelay (10);
  } while (mach_absolute_time () < deadline);
  return false;
}

// One UIC command through the host controller channel. `result` receives the UIC result code (ARG2 low byte),
// `value` the ARG3 the controller returned (the read value for DME_GET, zero otherwise).
static IOReturn hostUicCommand (uint32_t opcode, uint32_t arg1, uint32_t arg3, uint32_t timeoutUs,
                                uint32_t *result, uint32_t *value)
{
  uint32_t code = 0xffffffff, got = 0;
  IOReturn status = kIOReturnSuccess;
  if (gHost.poisoned) {
    if (result) *result = code;
    if (value) *value = got;
    return kIOReturnNotReady;
  }
  uint32_t pending = read32 (gHost.hc, 0x20);
  if (pending) hostWrite (0x20, pending);   // acknowledge, including a stale UIC completion
  hostWrite (0x94, arg1);
  hostWrite (0x98, 0);
  hostWrite (0x9c, arg3);
  if (!hostWaitMaskFor (0x30, 0x8, 0x8, 3000000)) {
    status = kIOReturnNotReady;             // HCS.UCRDY never came back after the reset
  } else {
    hostWrite (0x90, opcode);
    if (!hostWaitMaskFor (0x20, 0x400, 0x400, timeoutUs)) {
      status = kIOReturnTimeout;
    } else {
      code = read32 (gHost.hc, 0x98) & 0xff;
      got = read32 (gHost.hc, 0x9c);
      if (opcode != 0x14 && code != 0) status = kIOReturnIOError;
    }
  }
  if (result) *result = code;
  if (value) *value = got;
  return status;
}

// The handoff leaves HCS.UPMCRS=PWR_LOCAL (LK's power-off notify stopped inside a power mode change) and a failed
// SSU turns that into PWR_FATAL; UFSHCI names a host controller reset as the remedy for both, and it also clears
// the stale request list. mainline ufs_mtk_host_reset asserts hci, crypto and unipro, waits ~100us and releases
// unipro, crypto, hci in that order; the live mt6835 DT puts unipro on the AO block (mask 0x01) and crypto/hci on
// the PDN block (masks 0x02/0x01). diag86/87 never released unipro, so HCE never came back after a 0->1 toggle.
// XOUFS is set with HCE still 0, as ufs_mtk_hce_enable_notify does before the controller enable.
static void hostMtkResetPulse (uintptr_t ao, uintptr_t pdn)
{
  kprintf ("d109 p0\n");
  if (pdn) {
    write32 (pdn, 0x48, read32 (pdn, 0x48) | 0x03);          // assert hci + crypto
    write32 (pdn, 0x4c, read32 (pdn, 0x4c) & ~0x03u);
  }
  kprintf ("d109 pa\n");
  if (ao) {
    write32 (ao, 0x48, read32 (ao, 0x48) | 0x01);            // assert unipro
    write32 (ao, 0x4c, read32 (ao, 0x4c) & ~0x01u);
  }
  kprintf ("d109 pb\n");
  IODelay (110);
  kprintf ("d109 pc\n");
  if (ao) {
    write32 (ao, 0x48, read32 (ao, 0x48) & ~0x01u);
    write32 (ao, 0x4c, read32 (ao, 0x4c) | 0x01);            // release unipro
  }
  kprintf ("d109 pd\n");
  if (pdn) {
    write32 (pdn, 0x48, read32 (pdn, 0x48) & ~0x02u);
    write32 (pdn, 0x4c, read32 (pdn, 0x4c) | 0x02);          // release crypto
    IODelay (20);
    write32 (pdn, 0x48, read32 (pdn, 0x48) & ~0x01u);
    write32 (pdn, 0x4c, read32 (pdn, 0x4c) | 0x01);          // release hci
  }
  kprintf ("d109 pe\n");
  IODelay (100);
}

static bool hostResetController (const char **why)
{
  OSSharedPtr<IOMemoryMap> aoMap, pdnMap;
  kprintf ("d109 r0 m=%llx\n", mach_absolute_time ());
  uintptr_t ao = mapPhys (UFS_AO_BASE, 0x1000, aoMap);
  kprintf ("d109 r1 ao=%lx\n", (unsigned long)ao);
  uintptr_t pdn = mapPhys (UFS_PDN_BASE, 0x1000, pdnMap);
  kprintf ("d109 r1b pdn=%lx\n", (unsigned long)pdn);
  // Polled driver: the interrupt line must never assert. LK left IE enabled and stale
  // status set, and with no XNU handler a level-high UFS INT storms the GIC.
  hostWrite (0x24, 0);
  hostWrite (0x20, 0xffffffff);
  kprintf ("d110 r1c ie=%x is=%x\n", read32 (gHost.hc, 0x24), read32 (gHost.hc, 0x20));
  for (unsigned outer = 0; outer < 3; ++outer) {
    hostWrite (0x34, 0);
    kprintf ("d109 r2 m=%llx\n", mach_absolute_time ());
    if (!hostWaitMaskFor (0x34, 1, 0, 3000000)) {
      kprintf ("d109 r2f hce=%x\n", read32 (gHost.hc, 0x34));
      *why = "host controller reset (HCE clear)";
      return false;
    }
    kprintf ("d109 r3 hce=%x m=%llx\n", read32 (gHost.hc, 0x34) & 1, mach_absolute_time ());
    gRootDiagnostic.hceClear = read32 (gHost.hc, 0x34) & 1;
    hostMtkResetPulse (ao, pdn);
    kprintf ("d109 r4 m=%llx\n", mach_absolute_time ());
    hostMtkClockOn ();
    kprintf ("d109 r5 m=%llx\n", mach_absolute_time ());
    hostWrite (0x34, 1);
    kprintf ("d109 r6 m=%llx\n", mach_absolute_time ());
    IODelay (600);   // hba_enable_delay_us
    kprintf ("d109 r6b hce=%x\n", read32 (gHost.hc, 0x34) & 1);
    for (unsigned ms = 0; ms < 50 && !(read32 (gHost.hc, 0x34) & 1); ++ms) {
      IODelay (1000);
      if ((ms % 10) == 0) { kprintf ("d109 r6c %u hce=%x m=%llx\n", ms, read32 (gHost.hc, 0x34) & 1, mach_absolute_time ()); }
    }
    gRootDiagnostic.hceSet = read32 (gHost.hc, 0x34) & 1;
    kprintf ("d109 r7 hce=%x m=%llx\n", gRootDiagnostic.hceSet, mach_absolute_time ());
    if (gRootDiagnostic.hceSet) break;
  }
  gRootDiagnostic.resetHce = read32 (gHost.hc, 0x34) & 1;
  gRootDiagnostic.resetHcs = read32 (gHost.hc, 0x30);
  kprintf ("d110 r8a hce=%x hcs=%x\n", gRootDiagnostic.resetHce, gRootDiagnostic.resetHcs);
  uint32_t postIs = read32 (gHost.hc, 0x20);
  kprintf ("d110 r8b is=%x\n", postIs);
  kprintf ("d110 r8c ie=%x\n", read32 (gHost.hc, 0x24));
  if (!gRootDiagnostic.resetHce) {
    *why = "host controller reset (HCE set)";
    return false;
  }
  return true;
}

// The vendor DME writes the MTK link startup needs. Failures are recorded and the link startup still runs, as in the
// reference driver; the UIC result codes tell whether the host-side PA accepted them.
static void hostLinkPrelink (void)
{
  uint32_t code = 0xffffffff, value = 0;
  gRootDiagnostic.preSet[0] = 0xffffffff;
  gRootDiagnostic.preSet[1] = 0xffffffff;
  gRootDiagnostic.preSet[2] = 0xffffffff;
  gRootDiagnostic.preSet3 = 0xffffffff;
  kprintf ("d110 a0 in\n");
  hostUicCommand (0x02, 0xD0A8u << 16, 0, 500000, &code, &value);   // VS_UNIPROPOWERDOWNCONTROL = 0
  kprintf ("d110 a0 code=%x\n", code);
  gRootDiagnostic.preSet[0] = code;
  kprintf ("d110 a1 in\n");
  hostUicCommand (0x02, 0x155Eu << 16, 0, 500000, &code, &value);   // PA_LOCAL_TX_LCC_ENABLE = 0
  kprintf ("d110 a1 code=%x\n", code);
  gRootDiagnostic.preSet[1] = code;
  kprintf ("d110 a2 in\n");
  IOReturn got = hostUicCommand (0x01, 0xD0A6u << 16, 0, 500000, &code, &value);   // VS_SAVEPOWERCONTROL
  kprintf ("d110 a2 code=%x val=%x\n", code, value);
  gRootDiagnostic.preSet[2] = code;
  gRootDiagnostic.savePower = value;
  if (got == kIOReturnSuccess) {
    kprintf ("d110 a3 in\n");
    hostUicCommand (0x02, 0xD0A6u << 16, value & ~0x40u, 500000, &code, &value);  // clear deep stall
    kprintf ("d110 a3 code=%x\n", code);
    gRootDiagnostic.preSet3 = code;
  }
  kprintf ("d110 a4 out\n");
}

// Last rung after endpoint reset: reset both UniPro PA endpoints and enable the local
// endpoint (the broken-HCE flow in ufshcd_hba_enable), then let the caller retry.
static void hostLinkWake (void)
{
  uint32_t code = 0xffffffff, value = 0;
  hostUicCommand (0x14, 0, 0, 2000000, &code, &value);   // DME_RESET
  gRootDiagnostic.dmeReset = code;
  hostUicCommand (0x12, 0, 0, 2000000, &code, &value);   // DME_ENABLE
  gRootDiagnostic.dmeEnable = code;
}

// Second rung after a failed startup round: reset the peer UniPro endpoint (UFSHCI's
// link recovery) and let the caller retry the vendor prelink + link startup.
static void hostLinkRetry (void)
{
  uint32_t code = 0xffffffff, value = 0;
  hostUicCommand (0x15, 0, 0, 2000000, &code, &value);   // DME_ENDPOINT_RESET
  gRootDiagnostic.retryReset = code;
}

static bool hostLinkStartup (const char **why)
{
  gRootDiagnostic.linkRound++;
  for (unsigned attempt = 1; attempt <= 3; ++attempt) {
    uint32_t code = 0xffffffff, value = 0;
    kprintf ("d110 b%u in\n", attempt);
    hostUicCommand (0x16, 0, 0, 3000000, &code, &value);              // DME_LINKSTARTUP
    uint32_t hcs = read32 (gHost.hc, 0x30);
    kprintf ("d110 b%u code=%x hcs=%x\n", attempt, code, hcs);
    gRootDiagnostic.linkResult = code;
    gRootDiagnostic.linkAttempts = attempt;
    gRootDiagnostic.linkHcs = hcs;
    gRootDiagnostic.linkInterrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.linkCodes[attempt - 1] = code;
    // HCS.DP alone proves nothing: the handoff leaves it set from LK's own link. The link
    // only counts as up when the UIC command succeeded and the device reports present.
    if (code == 0 && (hcs & 1)) {
      uint32_t pending = read32 (gHost.hc, 0x20);
      if (pending) hostWrite (0x20, pending);
      return true;
    }
    hostWaitMaskFor (0x20, 0x100, 0x100, 300000);                     // wait for IS.ULSS before the retry
    uint32_t pending = read32 (gHost.hc, 0x20);
    if (pending) hostWrite (0x20, pending);
  }
  *why = "link startup";
  return false;
}

// Validate a completed response before interpreting zero-filled status bytes.
// Protocol codes: 1 type, 2 task tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 short/overrun transfer, 7 nonzero NOP result.
static IOReturn hostResponseHeader (uint8_t type, uint8_t lun, uint8_t tag)
{
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[0] != type) return hostProtocolError (1);
  if (rsp[3] != tag) return hostProtocolError (2);
  if (rsp[2] != lun) return hostProtocolError (3);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (rsp[8] || length > 512 - 32) return hostProtocolError (4);
  return kIOReturnSuccess;
}

static IOReturn hostNop (void)
{
  if (!hostClearUcd ()) return kIOReturnNotReady;
  gHost.virt[kUcdOffset + 3] = 1;
  uint32_t ocs = 15;
  IOReturn result = hostSubmit (0, 0, 200000, &ocs);
  if (result != kIOReturnSuccess) return result;
  result = hostResponseHeader (0x20, 0, 1);
  if (result != kIOReturnSuccess) return result;
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[6] || rsp[7]) return hostProtocolError (7);
  return kIOReturnSuccess;
}

// Run a SCSI command. direction 0: no data, 1: host -> device from the DMA data area (already filled by the caller), 2: device
// -> host into the DMA data area. Callers other than hostWrite4k never pass direction 1.
static IOReturn hostScsiDir (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction, uint32_t dataBytes,
                             uint32_t timeoutUs, uint32_t *scsiStatus)
{
  *scsiStatus = 0xffff;
  if (!cdb || !cdbLength || cdbLength > 16 || dataBytes > kDataMax || direction > 2 ||
      (dataBytes & 3) || (dataBytes ? !direction : direction)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = direction == 2 ? 0x40 : direction == 1 ? 0x20 : 0x00;   // R: data in, W: data out
  ucd[2] = lun;
  putBe32 (ucd + 12, dataBytes);
  for (unsigned index = 0; index < cdbLength; index++) {
    ucd[16 + index] = cdb[index];
  }
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (direction ? direction : 0U, dataBytes, timeoutUs, &ocs);
  if (status != kIOReturnSuccess) return status;
  status = hostResponseHeader (0x21, lun, 0);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  *scsiStatus = (uint32_t (rsp[6]) << 8) | rsp[7];
  gRootDiagnostic.scsi = *scsiStatus;
  gRootDiagnostic.residual = loadBe32 (rsp + 12);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (length) {
    unsigned senseLength = (unsigned (rsp[32]) << 8) | rsp[33];
    if (length < 2 || senseLength > length - 2) return hostProtocolError (4);
    const uint8_t *sense = rsp + 34;
    if (senseLength >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71))
      gRootDiagnostic.sense = (uint32_t (sense[2] & 15) << 16) | (uint32_t (sense[12]) << 8) | sense[13];
    else if (senseLength >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73))
      gRootDiagnostic.sense = (uint32_t (sense[1] & 15) << 16) | (uint32_t (sense[2]) << 8) | sense[3];
  }
  if (!*scsiStatus && ((rsp[1] & 0x60) || gRootDiagnostic.residual)) return hostProtocolError (6);
  IOReturn result = *scsiStatus == 0 ? kIOReturnSuccess : kIOReturnNotReady;
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  return result;
}

// The original read-only entry point: data, when present, always flows device -> host.
static IOReturn hostScsi (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t dataBytes,
                          uint32_t timeoutUs, uint32_t *scsiStatus)
{
  if (dataBytes && !toHost) { *scsiStatus = 0xffff; return kIOReturnBadArgument; }
  return hostScsiDir (lun, cdb, cdbLength, dataBytes ? 2U : 0U, dataBytes, timeoutUs, scsiStatus);
}

// START STOP UNIT, power condition ACTIVE (1h in the high nibble, as Linux ufshcd_set_dev_pwr_mode), to the UFS
// Device well-known LU (0xD0). A device in UFS-PowerDown answers this command and ignores every other UPIU, so the
// wake must precede the NOP OUT; on a clean handoff it is a no-op transition. IMMED stays 0: the command completes
// only once the device is Active. After a link reset the first command legitimately completes with CHECK CONDITION /
// UNIT ATTENTION "power-on/reset occurred" (sense 6/29/00); the device clears the UA when it reports it, so retry a
// few times. diag89 saw a later rejection (sense 5/25/00, LUN not supported) that repeating did not fix, so this is
// not a gate: as long as the UTP itself completed (OCS == 0) the device is answering, and the NOP OUT / power query /
// LUN 0 TUR decide whether it is up.
static IOReturn hostWakeDevice (void)
{
  static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
  IOReturn result = kIOReturnNotReady;
  uint32_t scsi = 0xffff;
  for (unsigned attempt = 1; attempt <= 4; ++attempt) {
    scsi = 0xffff;
    result = hostScsi (0xd0, ssu, 6, false, 0, 3000000, &scsi);
    gRootDiagnostic.wakeResult = static_cast<uint32_t> (result);
    gRootDiagnostic.wakeOcs = gRootDiagnostic.ocs;
    gRootDiagnostic.wakeScsi = scsi;
    gRootDiagnostic.wakeAttempts = attempt;
    gRootDiagnostic.wakeLastSense = gRootDiagnostic.sense;
    if (attempt == 1) {
      gRootDiagnostic.wakeFirstScsi = scsi;
      gRootDiagnostic.wakeFirstSense = gRootDiagnostic.sense;
    }
    if (result == kIOReturnSuccess) break;
    if (gRootDiagnostic.ocs != 0) break;
    IOSleep (10);   // the UA clears on its first report; the extra attempts are cheap and harmless
  }
  if (gRootDiagnostic.ocs != 0) return result;
  return kIOReturnSuccess;
}

// Query read request (opcode 0x03 READ ATTRIBUTE / 0x05 READ FLAG). Returns the value field.
static IOReturn hostQueryRead (uint8_t opcode, uint8_t idn, uint32_t *value)
{
  if (!value || (opcode != 3 && opcode != 5)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x16;                      // QUERY REQUEST
  ucd[3] = 1;
  ucd[5] = 0x01;                      // standard read request (header dword 1, byte 1)
  ucd[12] = opcode;
  ucd[13] = idn;
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (0, 0, 200000, &ocs);
  if (status != kIOReturnSuccess) {
    return status;
  }
  status = hostResponseHeader (0x36, 0, 1);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[12] != opcode || rsp[13] != idn || rsp[14] || rsp[15]) return hostProtocolError (5);
  if (rsp[6] != 0) {
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnError);
    return kIOReturnError;
  }
  *value = loadBe32 (rsp + 20);
  return kIOReturnSuccess;
}

// Query write request (opcode 0x06 SET FLAG). The request layout is hostQueryRead's with the query function switched to
// standard write (0x81); index/selector/value stay zero, as Linux sends for fDeviceInit. The response echoes opcode/idn
// and reports the query result code in byte 6.
static IOReturn hostQuerySet (uint8_t opcode, uint8_t idn, uint8_t index)
{
  kprintf ("d112 qs in op=%x idn=%x\n", opcode, idn);
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x16;                      // QUERY REQUEST
  ucd[3] = 1;
  ucd[5] = 0x81;                      // standard write request
  ucd[12] = opcode;
  ucd[13] = idn;
  ucd[14] = index;
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (0, 0, 200000, &ocs);
  kprintf ("d112 qs out st=%x ocs=%x\n", static_cast<uint32_t> (status), ocs);
  if (status != kIOReturnSuccess) {
    return status;
  }
  status = hostResponseHeader (0x36, 0, 1);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[12] != opcode || rsp[13] != idn || rsp[14] != index || rsp[15]) return hostProtocolError (5);
  if (rsp[6] != 0) {
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnError);
    return kIOReturnError;
  }
  return kIOReturnSuccess;
}

// UFS device initialization, the step Linux runs as ufshcd_complete_dev_init before it touches any LU: SET FLAG
// fDeviceInit (flag IDN 0x01, index 0), then poll READ FLAG until the device clears it (it clears the flag when its own
// initialization completed, Linux allows 5 s at 500-1000 us). diag90 had the UPIU path fully working (SSU and TUR both
// OCS=00) yet both ended CHECK CONDITION 5/25/00 while this step had never run. Recorded, not gated: the LUN 0 TUR after
// it stays the single "device usable" decision, so one boot shows both outcomes.
static IOReturn hostDeviceInit (void)
{
  kprintf ("d112 di0 m=%llx\n", mach_absolute_time ());
  IOReturn status = kIOReturnError;
  unsigned attempt = 0;
  for (; attempt < 3; ++attempt) {
    kprintf ("d112 di1 a=%u\n", attempt);
    status = hostQuerySet (0x06, 0x01, 0);
    kprintf ("d112 di2 a=%u st=%x\n", attempt, static_cast<uint32_t> (status));
    if (status == kIOReturnSuccess || gHost.poisoned) break;
    IOSleep (10);
  }
  gRootDiagnostic.devInitResult = static_cast<uint32_t> (status);
  gRootDiagnostic.devInitAttempts = attempt + 1;
  if (status != kIOReturnSuccess) return status;
  uint32_t value = 1;
  unsigned poll = 0;
  status = kIOReturnTimeout;
  for (; poll < 5000; ++poll) {
    if (poll < 4 || (poll % 500) == 0) kprintf ("d112 dp n=%u t=%llx\n", poll, mach_absolute_time ());
    IOReturn readStatus = hostQueryRead (0x05, 0x01, &value);
    if (readStatus != kIOReturnSuccess) {
      status = readStatus;
      break;
    }
    if (!value) {
      status = kIOReturnSuccess;
      break;
    }
    IOSleep (1);
  }
  kprintf ("d112 dr st=%x n=%u v=%x\n", static_cast<uint32_t> (status), poll, value);
  gRootDiagnostic.devInitPolls = poll;
  gRootDiagnostic.devInitFlag = value;
  return status;
}

// Read `blocks` (<= kDataMax / 4096) 4096-byte blocks of LUN `lun` starting at `lba` into dst.
// diag93 data-path evidence: how many dwords of the 4 KiB destination still hold the 0xA5A5A5A5
// prefill, and where (if anywhere) in the allocation the GPT header magic landed.
static uint32_t hostDataPattern (uint32_t *firstDword)
{
  const volatile uint32_t *data =
    reinterpret_cast<const volatile uint32_t *> (gHost.virt + kDataOffset + gProbeDataShift);
  uint32_t pattern = 0;
  for (unsigned index = 0; index < 1024; ++index) {
    pattern += data[index] == 0xa5a5a5a5U;
  }
  if (firstDword) *firstDword = data[0];
  return pattern;
}

static uint32_t hostFindGptMagic (void)
{
  const volatile uint32_t *whole = reinterpret_cast<const volatile uint32_t *> (gHost.virt);
  for (unsigned index = 0; index + 1 < kDmaBytes / 4; ++index) {
    if (whole[index] == 0x20494645U && whole[index + 1] == 0x54524150U) return index;
  }
  return 0xffffffffU;
}

static IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) {
    return kIOReturnBadArgument;
  }
  IOLockLock (gHost.lock);
  gRootDiagnostic.lba = lba;
  uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  bool probe = gRootDiagnosticEnabled && lun == kUserdataLun && lba == 1 && blocks == 1;
  if (probe) {
    volatile uint32_t *data = reinterpret_cast<volatile uint32_t *> (gHost.virt + kDataOffset);
    for (unsigned index = 0; index < 1024; ++index) data[index] = 0xa5a5a5a5U;
  }
  IOReturn status = hostScsi (lun, cdb, 10, true, blocks * 4096U, 1000000, &scsi);
  // diag94's LU2 LBA 0 read was the first command ever sent to the userdata LU and came back
  // as a failure while the next read succeeded: a unit attention pending on that LU. SCSI
  // leaves it armed until one command consumes it, so one re-issue of the same CDB is enough.
  if (status != kIOReturnSuccess) {
    if (probe) {
      volatile uint32_t *data = reinterpret_cast<volatile uint32_t *> (gHost.virt + kDataOffset);
      for (unsigned index = 0; index < 1024; ++index) data[index] = 0xa5a5a5a5U;
    }
    scsi = 0;
    status = hostScsi (lun, cdb, 10, true, blocks * 4096U, 1000000, &scsi);
  }
  if (probe) {
    gRootDiagnostic.dataPattern = hostDataPattern (&gRootDiagnostic.dataFirst);
    gRootDiagnostic.dataAt = hostFindGptMagic ();
    const volatile uint32_t *whole = reinterpret_cast<const volatile uint32_t *> (gHost.virt);
    gRootDiagnostic.dataUtrd6 = whole[6];
    gRootDiagnostic.dataUtrd7 = whole[7];
    const volatile uint32_t *prd =
      reinterpret_cast<const volatile uint32_t *> (gHost.virt + kUcdOffset + kPrdtOffset);
    for (unsigned index = 0; index < 4; ++index) gRootDiagnostic.dataPrd[index] = prd[index];
    gRootDiagnostic.dataIs = read32 (gHost.hc, 0x20);
    // Retry once with the destination 64 KiB higher when the first transfer left its page untouched.
    if (gRootDiagnostic.dataPattern == 1024 && gRootDiagnostic.dataAt == 0xffffffffU && !gHost.poisoned) {
      volatile uint32_t *shifted = reinterpret_cast<volatile uint32_t *> (gHost.virt + kDataOffset + 65536);
      for (unsigned index = 0; index < 1024; ++index) shifted[index] = 0xa5a5a5a5U;
      gProbeDataShift = 65536;
      uint32_t shiftedScsi = 0;
      IOReturn shiftedStatus = hostScsi (lun, cdb, 10, true, blocks * 4096U, 1000000, &shiftedScsi);
      gRootDiagnostic.dataShiftOk = static_cast<uint32_t> (shiftedStatus);
      gRootDiagnostic.dataShiftPattern = hostDataPattern (&gRootDiagnostic.dataShiftFirst);
      gRootDiagnostic.dataShiftAt = hostFindGptMagic ();
      gProbeDataShift = 0;
    }
  }
  if (status == kIOReturnSuccess) {
    memcpy (dst, gHost.virt + kDataOffset, blocks * 4096U);
  }
  IOLockUnlock (gHost.lock);
  return status;
}

// ---- Persistent store and log windows ------------------------------------------------------------------------------
// The only place this driver writes. At most two windows of 4096-byte LBAs on LUN 0 (the Data store and the diagnostic log),
// each opened once by its integration after it has proved that the window lies inside the userdata partition and clear of the
// root image; every write is checked against them again here, so nothing outside them can be written even if a caller is wrong.
static bool gStoreOpen = false, gLogOpen = false;
static uint64_t gStoreFirst = 0, gStoreBlocks = 0, gLogFirst = 0, gLogBlocks = 0;

static bool hostLogOpen (uint64_t first, uint64_t blocks)
{
  if (gLogOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gStoreOpen && first < gStoreFirst + gStoreBlocks && gStoreFirst < first + blocks) return false;
  gLogFirst = first;
  gLogBlocks = blocks;
  gLogOpen = true;
  return true;
}

static bool hostInWindow (uint64_t lba, uint32_t blocks)
{
  if (gStoreOpen && lba >= gStoreFirst && lba - gStoreFirst < gStoreBlocks && blocks <= gStoreBlocks - (lba - gStoreFirst)) return true;
  return gLogOpen && lba >= gLogFirst && lba - gLogFirst < gLogBlocks && blocks <= gLogBlocks - (lba - gLogFirst);
}

static bool hostStoreOpen (uint64_t first, uint64_t blocks)
{
  if (gStoreOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gLogOpen && first < gLogFirst + gLogBlocks && gLogFirst < first + blocks) return false;
  gStoreFirst = first;
  gStoreBlocks = blocks;
  gStoreOpen = true;
  return true;
}

// WRITE(10) of `blocks` 4096-byte blocks from src to lba on the userdata LUN; the range must lie inside one open window.
static IOReturn hostWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua = false)
{
  if (!gHost.ready || !src || !blocks || blocks > kDataMax / 4096 || lun != kUserdataLun || !hostInWindow (lba, blocks)) {
    return kIOReturnBadArgument;
  }
  IOLockLock (gHost.lock);
  gRootDiagnostic.lba = lba;
  memcpy (gHost.virt + kDataOffset, src, blocks * 4096U);
  uint8_t cdb[10] = { 0x2a, uint8_t (fua ? 0x08 : 0), uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (lun, cdb, 10, 1, blocks * 4096U, 2000000, &scsi);
  IOLockUnlock (gHost.lock);
  return status;
}

// SYNCHRONIZE CACHE(10) for the whole userdata LUN: everything written so far is on the medium when this returns.
static IOReturn hostSyncCache (void)
{
  if (!gHost.ready || (!gStoreOpen && !gLogOpen)) return kIOReturnNotReady;
  IOLockLock (gHost.lock);
  uint8_t cdb[10] = { 0x35 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (kUserdataLun, cdb, 10, 0, 0, 10000000, &scsi);
  IOLockUnlock (gHost.lock);
  return status;
}

