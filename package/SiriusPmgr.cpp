// SPDX-License-Identifier: MIT
// Advertise an ApplePMGRNub to the unchanged AMFI.
//
// AMFI's ConfigurationSettings finds the power manager through
// IOService::serviceMatching("ApplePMGRNub") + waitForMatchingService, then ORs bit 2 into the
// always-on state it reads back. The target helpers behind that call load the provider from
// nub+0x88 and, when the field is NULL, return 0 / return without writing. Publishing a bare
// ApplePMGRNub therefore satisfies the latch without implementing Apple's PMGR and without
// patching AMFI: the target kernelcache stays unmodified.
//
// The call site is the sheng-platform-power.patch hook in bsd_init (CONFIG_XNU_SHENG_PMGR=1),
// which runs before AMFI's configuration settings are initialised.
// Reference implementation: 57UU/UniXNU-sirius package/SiriusPmgr.cpp.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/vm_types.h>

extern "C" void sheng_pmgr_run(void);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

// UFS host state at XNU time (boot arg sheng-ufs-probe=1). Registers only: no reset, no UIC
// command, so the controller and LK's request list are left exactly as found. kprintf is used
// because IOLog bypasses the hooked _console_write_unbuffered and would not appear on the
// big-text console.
extern "C" void sheng_ufs_probe_run(void);

static uint32_t ufs_rd(volatile uint32_t *base, uint32_t off)
{
  return base[off / 4];
}

extern "C" void sheng_ufs_probe_run(void)
{
  unsigned enable = 0;
  if (!PE_parse_boot_argn("sheng-ufs-probe", &enable, sizeof(enable)) || (enable == 0)) {
    return;
  }
  kprintf("ufs: probe start\n");
  volatile uint32_t *hc = reinterpret_cast<volatile uint32_t *>(
      ml_io_map(0x112B0000ULL, 0x2300));
  if (hc == nullptr) {
    kprintf("ufs: hc map failed\n");
    return;
  }
  uint32_t cap = ufs_rd(hc, 0x00);
  uint32_t ver = ufs_rd(hc, 0x08);
  uint32_t hcddid = ufs_rd(hc, 0x10);
  uint32_t hcpmid = ufs_rd(hc, 0x14);
  uint32_t is = ufs_rd(hc, 0x20);
  uint32_t hcs = ufs_rd(hc, 0x30);
  uint32_t hce = ufs_rd(hc, 0x34);
  kprintf("ufs: CAP=%08x VER=%08x DID=%08x MID=%08x\n", cap, ver, hcddid, hcpmid);
  kprintf("ufs: IS=%08x HCS=%08x HCE=%08x\n", is, hcs, hce);
  kprintf("ufs: nutrs=%u 64as=%u dp=%u utrlrdy=%u hce_on=%u\n",
      (cap & 0x1f) + 1, (cap >> 24) & 1, hcs & 1, (hcs >> 1) & 1, hce & 1);
  kprintf("ufs: UTRLBA=%08x:%08x RSR=%08x CNR=%08x DBR=%08x\n",
      ufs_rd(hc, 0x54), ufs_rd(hc, 0x50), ufs_rd(hc, 0x60),
      ufs_rd(hc, 0x64), ufs_rd(hc, 0x58));
  kprintf("ufs: UTMRLBA=%08x:%08x RSR=%08x\n",
      ufs_rd(hc, 0x74), ufs_rd(hc, 0x70), ufs_rd(hc, 0x80));
  kprintf("ufs: vendor140=%08x refclk144=%08x\n",
      ufs_rd(hc, 0x140), ufs_rd(hc, 0x144));
  volatile uint32_t *ao = reinterpret_cast<volatile uint32_t *>(
      ml_io_map(0x112B8000ULL, 0x1000));
  volatile uint32_t *pdn = reinterpret_cast<volatile uint32_t *>(
      ml_io_map(0x112BB000ULL, 0x1000));
  if ((ao != nullptr) && (pdn != nullptr)) {
    kprintf("ufs: AO st=%08x rst=%08x PDN st=%08x rst=%08x\n",
        ufs_rd(ao, 0x04), ufs_rd(ao, 0x4c), ufs_rd(pdn, 0x04), ufs_rd(pdn, 0x4c));
  } else {
    kprintf("ufs: syscon map failed ao=%p pdn=%p\n", (void *)ao, (void *)pdn);
  }
  kprintf("ufs: probe end\n");
}

// Bring-up evidence: 0 nothing tried, 1 class absent, 2 allocation failed, 3 published,
// 4 discovery check failed.
volatile uint32_t tb365fc_pmgr_step;
volatile uint32_t tb365fc_pmgr_class_size;
volatile uint64_t tb365fc_pmgr_nub;

static const OSMetaClass *nubMetaClass(void)
{
  const OSSymbol *name = OSSymbol::withCString("ApplePMGRNub");
  if (name == nullptr) {
    return nullptr;
  }
  const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(name);
  name->release();
  return meta;
}

extern "C" void sheng_pmgr_run(void)
{
  sheng_ufs_probe_run();
  IOLog("pmgr-mark 1 enter\n");
  unsigned mode = 0;
  bool arg = PE_parse_boot_argn("sheng-pmgr", &mode, sizeof(mode));
  IOLog("pmgr-mark 2 arg=%d mode=%u\n", arg, mode);
  if (!arg || (mode == 0)) {
    return;
  }
  tb365fc_pmgr_step = 1;
  const OSMetaClass *meta = nubMetaClass();
  IOLog("pmgr-mark 3 meta=%p\n", meta);
  if (meta == nullptr) {
    // The prelinked kext may not have registered its classes yet at this point; load it explicitly
    // (a no-op when it is already loaded) and look the class up again.
    IOLog("pmgr-mark 4 loading kext\n");
    bool loaded = OSKext::loadKextWithIdentifier("com.apple.driver.ApplePMGR");
    IOLog("pmgr-mark 5 load=%d\n", loaded);
    meta = nubMetaClass();
    IOLog("pmgr-mark 6 meta=%p\n", meta);
  }
  if (meta == nullptr) {
    IOLog("tb365fc-pmgr: ApplePMGRNub class absent\n");
    return;
  }
  tb365fc_pmgr_class_size = meta->getClassSize();
  IOService *nub = static_cast<IOService *>(meta->alloc());
  IOLog("pmgr-mark 7 nub=%p\n", nub);
  if (nub == nullptr) {
    tb365fc_pmgr_step = 2;
    IOLog("tb365fc-pmgr: ApplePMGRNub allocation failed\n");
    return;
  }
  bool inited = nub->init();
  IOLog("pmgr-mark 8 init=%d\n", inited);
  if (!inited) {
    tb365fc_pmgr_step = 2;
    IOLog("tb365fc-pmgr: ApplePMGRNub init failed\n");
    nub->release();
    return;
  }
  IOService *platform = IOService::getPlatform();
  IOLog("pmgr-mark 9 platform=%p\n", platform);
  bool attached = (platform == nullptr) || nub->attach(platform);
  IOLog("pmgr-mark 10 attach=%d\n", attached);
  if (!attached) {
    tb365fc_pmgr_step = 2;
    IOLog("tb365fc-pmgr: ApplePMGRNub attach failed\n");
    nub->release();
    return;
  }
  // No provider is set: the field AMFI dereferences stays NULL, which is the path that returns 0
  // instead of reaching into Apple's PMGR object.
  nub->registerService(kIOServiceSynchronous);
  IOLog("pmgr-mark 11 registered\n");
  tb365fc_pmgr_nub = reinterpret_cast<uint64_t>(nub);
  tb365fc_pmgr_step = 3;
  IOService *found = nullptr;
  OSDictionary *matching = IOService::serviceMatching("ApplePMGRNub");
  IOLog("pmgr-mark 12 matching=%p\n", matching);
  if (matching != nullptr) {
    // OSPtr is the raw pointer in this configuration; waitForMatchingService returns it retained.
    found = IOService::waitForMatchingService(matching, 1000000);
    matching->release();
  }
  IOLog("pmgr-mark 13 found=%p\n", found);
  tb365fc_pmgr_step = (found == nub) ? 4 : 3;
  IOLog("tb365fc-pmgr: ApplePMGRNub size=%u nub=%p found=%p\n", tb365fc_pmgr_class_size, nub, found);
  if (found != nullptr) {
    found->release();
  }
}
