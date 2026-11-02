// SPDX-License-Identifier: BSD-2-Clause-Patent
// Root queue takeover; shared with the host fault-injection harness.
static bool hostWaitRegister (unsigned offset, uint32_t mask, uint32_t value)
{
  uint64_t deadline;
  clock_interval_to_deadline (100, kMillisecondScale, &deadline);
  do {
    if ((read32 (gHost.hc, offset) & mask) == value) return true;
    IODelay (10);
  } while (mach_absolute_time () < deadline);
  return false;
}

static bool hostStopQueue (const char **why)
{
  // Confirm the firmware has no active transfer/task before changing its
  // request list or the storage stream's SMMU routing.
  kprintf ("d111 s0 db=%x tm=%x hcs=%x hce=%x\n", read32 (gHost.hc, 0x58),
      read32 (gHost.hc, 0x78), read32 (gHost.hc, 0x30), read32 (gHost.hc, 0x34));
  if (!(read32 (gHost.hc, 0x34) & 1) || (read32 (gHost.hc, 0x30) & 0x0f) != 0x0f) {
    recordTransport (kIOReturnNotReady, 15);
    *why = "controller not ready for takeover";
    return false;
  }
  if (read32 (gHost.hc, 0x58) || read32 (gHost.hc, 0x78)) {
    recordTransport (kIOReturnBusy, 15);
    *why = "firmware request outstanding";
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    recordTransport (kIOReturnIOError, 15);
    *why = "firmware controller error";
    return false;
  }
  hostWrite (0x24, 0);
  hostWrite (0x60, 0);
  kprintf ("d111 s1\n");
  if (!hostWaitRegister (0x60, 1, 0)) {
    kprintf ("d111 s1f rsr=%x\n", read32 (gHost.hc, 0x60) & 1);
    recordTransport (kIOReturnTimeout, 15);
    *why = "request list stop";
    return false;
  }
  kprintf ("d111 s2 hcs=%x\n", read32 (gHost.hc, 0x30));
  return true;
}

static bool hostStartQueue (const char **why)
{
  kprintf ("d111 t0 base=%llx\n", (unsigned long long) gHost.phys);
  hostWrite (0x50, static_cast<uint32_t> (gHost.phys));
  hostWrite (0x54, static_cast<uint32_t> (gHost.phys >> 32));
  kprintf ("d111 t1 rba=%x rbau=%x\n", read32 (gHost.hc, 0x50), read32 (gHost.hc, 0x54));
  if (read32 (gHost.hc, 0x50) != uint32_t (gHost.phys) ||
      read32 (gHost.hc, 0x54) != uint32_t (gHost.phys >> 32)) {
    recordTransport (kIOReturnIOError, 15);
    *why = "request list address";
    return false;
  }
  gRootDiagnostic.isBefore = read32 (gHost.hc, 0x20);
  kprintf ("d111 t2 is=%x\n", gRootDiagnostic.isBefore);
  hostWrite (0x20, gRootDiagnostic.isBefore);
  hostWrite (0x60, 1);
  kprintf ("d111 t3 rsr=%x\n", read32 (gHost.hc, 0x60) & 1);
  if (!hostWaitRegister (0x60, 1, 1) || !hostWaitRegister (0x30, 3, 3)) {
    kprintf ("d111 t3f rsr=%x hcs=%x\n", read32 (gHost.hc, 0x60) & 1, read32 (gHost.hc, 0x30));
    recordTransport (kIOReturnTimeout, 15);
    *why = "request list ready";
    return false;
  }
  kprintf ("d111 t4 hcs=%x\n", read32 (gHost.hc, 0x30));
  return true;
}
