// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native MediaTek MT6835 UFS bring-up for TB365FC (cava). Default-off: no MMIO is
// touched unless native-ufs-probe=1 or native-ufs-root=1 is present.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/crypto/sha2.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <sys/param.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <IOKit/IOBSD.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include "GccUfs.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "verified_root.h"

static UfsRootDiagnostics gRootDiagnostic;
static bool gRootDiagnosticEnabled;
static void rootDiagnosticFailure (const char *reason);

extern "C" {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
#endif
// Legacy display client gate: the probe defines this; other boards leave it out.
extern volatile int native_legacy_client_status __attribute__((weak));
volatile uint32_t native_ufs_probe_calls = 0;
volatile uint32_t native_ufs_matched = 0;
volatile uint32_t native_ufs_probe_requested = 0;
volatile uint32_t native_ufs_mapped = 0;
volatile uint32_t native_ufs_status = 0;
// Root start: 1 entered, 2 host up, 3 layout found; the last soft-failure reason (a static string), for the boot page.
volatile uint32_t native_ufs_root_step = 0;
const char *volatile native_ufs_root_failure = nullptr;
// diag116 ISR-beacon markers, defined by the ArmCpu driver.
extern volatile uint32_t armcpu_marker_a, armcpu_marker_b, armcpu_marker_d;
// diag117 framebuffer breadcrumbs, defined by bsd_init.c.
void d117_mark (uint32_t slot);
volatile uint32_t native_ufs_cap = 0;
volatile uint32_t native_ufs_version = 0;
volatile uint32_t native_ufs_hcs = 0;
volatile uint32_t native_ufs_hce = 0;
volatile uint32_t native_ufs_utrl_ready = 0;
volatile uint32_t native_ufs_uic_status = 0;
volatile uint32_t native_ufs_uic_result = 0;
volatile uint32_t native_ufs_policy_status = 0xffffffff;
volatile uint32_t native_ufs_verified_windows = 0;
volatile uint64_t native_ufs_verified_blocks = 0;
volatile uint32_t native_ufs_integrity_failures = 0;
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOBufferMemoryDescriptor> fCmdDesc;
  bool startRoot (IOService *provider);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = 0;
  while (needle[wanted]) {
    wanted++;
  }
  for (size_t at = 0; at + wanted <= length; at++) {
    size_t index = 0;
    while (index < wanted && data[at + index] == needle[index]) {
      index++;
    }
    if (index == wanted) {
      return true;
    }
  }
  return false;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  native_ufs_probe_calls = native_ufs_probe_calls + 1;
  if (OSDynamicCast (IOPlatformExpertDevice, provider)) {
    // Second personality (KernelConfigTables: IOMatchCategory NativeUfsRoot): attaches to the
    // root device, which starts before the platform expert publishes IOResources, so the
    // hardware probes run before the original AMFI (whose start panics without a PMGR).
    native_ufs_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  bool match = false;
  if (data) {
    const char *text = static_cast<const char *> (data->getBytesNoCopy ());
    match = text && contains (text, data->getLength (), "mediatek,mt8183-ufshci");
  } else if (string) {
    const char *text = string->getCStringNoCopy ();
    match = text && contains (text, string->getLength (), "mediatek,mt8183-ufshci");
  }
  if (!match) {
    return nullptr;
  }
  native_ufs_matched = 1;
  if (score) {
    *score = 1000;
  }
  return this;
}

// The controller window is the MT6835 DT's ufshci@112b0000 range. LK hands the
// controller over enabled but with the UniPro link parked by its power-off
// notify; the driver maps it and takes the request list over after the link is
// walked back up.
static uintptr_t mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}


// ======================================================================================
// Root media: the live controller as a read-only block device.
//
// One shared host (a single UTP request slot, polled; interrupts stay masked) serves every
// block device. The DMA region is ordinary cached, physically contiguous kernel memory with
// explicit cache maintenance around each command (the MT6835 UFS node has no iommus, so the
// controller's DMA uses physical addresses as-is).
// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus
// the single START STOP UNIT that wakes the device from the power-down the firmware left.
// ======================================================================================
namespace {
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  IOLock *lock;
  bool ready;
  bool poisoned = false;
};
UfsHost gHost = { OSSharedPtr<IOMemoryMap> (), 0, nullptr, nullptr, 0, nullptr, false };

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

#include "RootQueue.h"

#include "RootCommands.h"

// Bring the live controller up for reading: map, take the request list over from the firmware,
// wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.entryHcs = gRootDiagnostic.hcs;
    gRootDiagnostic.entryHce = gRootDiagnostic.hce;
    gRootDiagnostic.entryIs = read32 (gHost.hc, 0x20);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.entryUic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    // Read-only power evidence: whether LK's power-off notify also gated the DFS/PDN
    // blocks and the PHY reference clock, or only parked the link.
    gRootDiagnostic.xoufs = read32 (gHost.hc, 0x140);
    gRootDiagnostic.refclk = read32 (gHost.hc, 0x144);
    gRootDiagnostic.ipVer = read32 (gHost.hc, 0x2240);
    OSSharedPtr<IOMemoryMap> aoMap, pdnMap;
    uintptr_t ao = mapPhys (UFS_AO_BASE, 0x1000, aoMap);
    uintptr_t pdn = mapPhys (UFS_PDN_BASE, 0x1000, pdnMap);
    gRootDiagnostic.aoStatus = ao ? read32 (ao, 0x04) : 0xffffffff;
    gRootDiagnostic.aoReset = ao ? read32 (ao, 0x4c) : 0xffffffff;
    gRootDiagnostic.pdnStatus = pdn ? read32 (pdn, 0x04) : 0xffffffff;
    gRootDiagnostic.pdnReset = pdn ? read32 (pdn, 0x4c) : 0xffffffff;
  }
  gRootDiagnostic.stage = "DMA allocation";
  kprintf ("d113 w0 m=%llx\n", mach_absolute_time ());
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = IOLockAlloc ();
  kprintf ("d113 w1 dma=%llx lock=%llx m=%llx\n", static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (gHost.dma)),
      static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (gHost.lock)), mach_absolute_time ());
  if (!gHost.dma || !gHost.lock) {
    *why = "dma buffer";
    return false;
  }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  kprintf ("d113 w2 virt=%llx m=%llx\n", static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (gHost.virt)),
      mach_absolute_time ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  kprintf ("d113 w3 phys=%llx c=%llx m=%llx\n", gHost.phys, static_cast<unsigned long long> (contiguous),
      mach_absolute_time ());
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(read32 (gHost.hc, 0) & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL)) {
    *why = "DMA address/contiguity";
    return false;
  }
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  kprintf ("d113 w4 m=%llx\n", mach_absolute_time ());
  // A healthy handoff has both UTP lists and the UIC channel ready and no fatal
  // power-mode change pending: take the request list over as before. Anything
  // else (the MTK bootloader parks the UniPro link) is reset and brought up the
  // way the MTK UEFI driver does, including its pre-link DME writes.
  uint32_t status = read32 (gHost.hc, 0x30);
  kprintf ("d113 w5 hcs=%x m=%llx\n", status, mach_absolute_time ());
  bool healthy = (read32 (gHost.hc, 0x34) & 1) && (status & 0x0f) == 0x0f &&
                 ((status >> 8) & 7) == 0 && !(read32 (gHost.hc, 0x20) & kUfsFatalStatus);
  kprintf ("d113 w6 healthy=%u m=%llx\n", healthy ? 1U : 0U, mach_absolute_time ());
  if (healthy) {
    gRootDiagnostic.stage = "request list";
    if (!hostStopQueue (why)) return false;
    if (!hostStartQueue (why)) return false;
  } else {
    // LK's power-off notify cut the device rails and stopped inside a host power-mode change
    // (HCS.UPMCRS=PWR_LOCAL), which makes every link-level UIC command fail. Power the MTK
    // stack up, then run the mainline MTK host reset: assert and release hci/crypto/unipro,
    // set XOUFS with HCE still 0, and only then enable HCE. The queue starts after the link,
    // since DP drops to 0 across the reset.
    gRootDiagnostic.stage = "mtk power up";
    hostMtkPowerUp ();
    bool linked = false;
    for (unsigned round = 0; round < 2 && !linked; ++round) {
      gRootDiagnostic.stage = round ? "host reset retry" : "host controller reset";
      if (!hostResetController (why)) return false;
      kprintf ("d110 L0 m=%llx\n", mach_absolute_time ());
      gRootDiagnostic.stage = "link prelink";
      hostLinkPrelink ();
      gRootDiagnostic.stage = "link startup";
      linked = hostLinkStartup (why);
      if (!linked) {
        gRootDiagnostic.stage = "link retry";
        hostLinkRetry ();
        hostLinkWake ();
        hostLinkPrelink ();
        gRootDiagnostic.stage = "link startup";
        linked = hostLinkStartup (why);
      }
    }
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    gRootDiagnostic.linkOk = linked ? 1 : 0;
    if (!linked) {
      gRootDiagnostic.stage = "link startup";
      *why = "link startup";
      return false;
    }
    gRootDiagnostic.stage = "request list";
    kprintf ("d111 stq in\n");
    // UTRLBA is only writable while the request list is stopped; the old
    // firmware list must be stopped before the takeover re-programs it.
    if (!hostStopQueue (why)) return false;
    kprintf ("d111 stq out\n");
    if (!hostStartQueue (why)) return false;
    kprintf ("d111 str out\n");
  }
  // The handoff leaves the device in UFS-PowerDown, where every UPIU but SSU is dropped
  // (diag84's NOP OUT got OCS 5 PEER_COMM_FAILURE). Wake it to Active before the NOP proof.
  gRootDiagnostic.stage = "device wake";
  kprintf ("d111 w in\n");
  IOReturn woke = hostWakeDevice ();
  kprintf ("d111 w out r=%x ocs=%x scsi=%x\n", woke, gRootDiagnostic.ocs, gRootDiagnostic.wakeScsi);
  if (woke != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "device wake (START STOP UNIT)";
    return false;
  }
  gRootDiagnostic.stage = "NOP OUT";
  // NOP OUT proves the UPIU path before touching the SCSI layer.
  kprintf ("d111 n in\n");
  IOReturn nop = hostNop ();
  kprintf ("d111 n out r=%x\n", nop);
  if (nop != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  kprintf ("d111 p in\n");
  IOReturn qr = hostQueryRead (0x03, 0x02, &power);
  kprintf ("d111 p out r=%x v=%x\n", qr, power);
  if (qr != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
    // Still not Active (bCurrentPowerMode != 0x11): repeat the SSU wake, as Linux resumes.
    if (hostWakeDevice () != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  // UFS device initialization (Linux ufshcd_complete_dev_init) belongs between the NOP proof and the first LU command.
  // diag90 proved the UPIU path end to end with the device Active, yet both the SSU and the LUN 0 TUR ended CHECK
  // CONDITION 5/25/00 - the signature of LU access before the device initialized. Not gated; INIT line records it.
  gRootDiagnostic.stage = "device init";
  IOReturn initStatus = hostDeviceInit ();
  kprintf ("d112 di out st=%x n=%u v=%x\n", static_cast<uint32_t> (initStatus),
      gRootDiagnostic.devInitPolls, gRootDiagnostic.devInitFlag);
  gRootDiagnostic.stage = "LUN ready";
  // diag89 left the SSU at CHECK CONDITION (UA then 5/25/00), so the LUN 0 TEST UNIT READY below is the real
  // "device usable" proof. A freshly powered mode change can answer UA / not-ready for a while; retry long enough.
  kprintf ("d112 lr in poisoned=%u\n", gHost.poisoned ? 1U : 0U);
  for (unsigned attempt = 0; attempt < 40; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    IOReturn turStatus = hostScsi (0, tur, 6, false, 0, 200000, &scsi);
    if (attempt < 3 || turStatus == kIOReturnSuccess || gHost.poisoned) {
      kprintf ("d112 tur a=%u st=%x ocs=%x scsi=%x t=%llx\n", attempt,
          static_cast<uint32_t> (turStatus), gRootDiagnostic.ocs, scsi, mach_absolute_time ());
    }
    if (turStatus == kIOReturnSuccess) {
      gHost.ready = true;
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (20);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
}

// diag108: let a kernel thread outside this file read where the bring-up stopped.
extern "C" const char *native_ufs_d108_stage (void)
{
  return gRootDiagnostic.stage;
}

extern "C" uint32_t native_ufs_d108_flags (void)
{
  return (gHost.ready ? 1U : 0U) | (gHost.poisoned ? 2U : 0U);
}

namespace {
static uint32_t be32w (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

#include "RootLayout.h"
#include "RootPolicy.h"
#include "RootWindowRead.h"
#include "RootPublication.h"

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
// The region as read from userdata, then made read-only, followed by a read-only copy of the owner key on its own
// page. vr_open requires both to stay immutable for as long as the views opened on them are used: the windows never
// free it.
struct RootPolicyStore {
  vm_offset_t storage = 0;
  vm_size_t storageBytes = 0;
  const uint8_t *key = nullptr;
  RootPolicyHeader header;
};
RootPolicyStore gRootPolicy;

static void policyDigest (const uint8_t *header, uint8_t digest[32])
{
  static const uint8_t zero[32] = {};
  SHA256_CTX context;
  SHA256_Init (&context);
  SHA256_Update (&context, header, kRootPolicyHashed);
  SHA256_Update (&context, zero, sizeof (zero));
  SHA256_Final (digest, &context);
}

static const char *policyReason (unsigned status)
{
  switch (status) {
  case kRootPolicyNoMagic: return "no A04RPOL1 policy region after the root windows";
  case kRootPolicyVersion: return "policy version or entry count";
  case kRootPolicyHash: return "policy header SHA-256 mismatch";
  case kRootPolicySize: return "policy region size";
  case kRootPolicyEntryBounds: return "policy entry bounds";
  case kRootPolicyEntryOverlap: return "policy entries overlap";
  default: return "policy region rejected";
  }
}

static void freeRootPolicy (void)
{
  if (gRootPolicy.storage) kmem_free (kernel_map, gRootPolicy.storage, gRootPolicy.storageBytes);
  gRootPolicy = {};
}

// Reads the whole region into kernel memory, checks its header again from that copy and makes it read-only.
// Returns nullptr or the reason the region is unusable.
static const char *loadRootPolicy (const RootLayout *layout)
{
  uint64_t partitionBytes = (layout->userdataLast - layout->userdataFirst + 1) * 4096ULL;
  uint64_t start = rootPolicyRegionOffset (layout);
  if (start >= partitionBytes || partitionBytes - start < kRootPolicyHeaderBytes) return "policy region outside userdata";
  uint64_t available = partitionBytes - start;
  uint64_t firstLba = layout->userdataFirst + start / 4096;
  RootPolicyHeader header;
  unsigned status;
  uint8_t digest[32];
  gRootDiagnostic.stage = "policy header";
  if (hostRead4k (kUserdataLun, firstLba, 1, gScratch) != kIOReturnSuccess) return "policy header read failed";
  policyDigest (gScratch, digest);
  status = parseRootPolicyHeader (gScratch, digest, available, header);
  native_ufs_policy_status = status;
  kprintf ("d114 pol hdr st=%u bytes=%llx t=%llx\n", status,
      status == kRootPolicyOk ? header.totalBytes : 0ULL, mach_absolute_time ());
  if (status != kRootPolicyOk) return policyReason (status);
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (header.totalBytes));
  vm_size_t keyBytes = round_page (static_cast<vm_size_t> (VR_KEY_BYTES));
  if (tableBytes < header.totalBytes || uint64_t (tableBytes) > UINT64_MAX - keyBytes) return "policy size overflow";
  vm_offset_t storage = 0;
  gRootDiagnostic.stage = "policy allocation";
  armcpu_marker_a = 1;
  d117_mark (0);
  kprintf ("d114 pol mem in n=%llx t=%llx\n", static_cast<uint64_t> (tableBytes + keyBytes),
      mach_absolute_time ());
  d117_mark (1);
  armcpu_marker_d = 1;
  d117_mark (2);
  kern_return_t allocResult = kmem_alloc (kernel_map, &storage, tableBytes + keyBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY);
  d117_mark (3);
  armcpu_marker_b = 1;
  kprintf ("d115 alloc ret kr=%d a=%llx t=%llx\n", allocResult, static_cast<uint64_t> (storage),
      mach_absolute_time ());
  if (allocResult != KERN_SUCCESS)
    return "policy allocation";
  gRootPolicy.storage = storage;
  gRootPolicy.storageBytes = tableBytes + keyBytes;
  uint8_t *region = reinterpret_cast<uint8_t *> (storage);
  IOLog ("NativeUfs: reading the signed root policy, %llu bytes at userdata +%llu\n", header.totalBytes, start);
  kprintf ("d115 iolog ok t=%llx\n", mach_absolute_time ());
  kprintf ("d114 pol mem ok t=%llx\n", mach_absolute_time ());
  gRootDiagnostic.stage = "policy region read";
  uint64_t blocks = header.totalBytes / 4096, chunk = kDataMax / 4096;
  kprintf ("d114 pol rd in blocks=%llu ch=%llu t=%llx\n", blocks, chunk, mach_absolute_time ());
  for (uint64_t done = 0; done < blocks; done += chunk) {
    uint32_t count = blocks - done > chunk ? static_cast<uint32_t> (chunk) : static_cast<uint32_t> (blocks - done);
    if (hostRead4k (kUserdataLun, firstLba + done, count, region + done * 4096) != kIOReturnSuccess) {
      freeRootPolicy ();
      return "policy region read failed";
    }
  }
  kprintf ("d114 pol rd ok t=%llx\n", mach_absolute_time ());
  // The header is taken from the copy that the envelopes are opened from, and must describe the same region.
  policyDigest (region, digest);
  status = parseRootPolicyHeader (region, digest, available, gRootPolicy.header);
  native_ufs_policy_status = status;
  if (status != kRootPolicyOk || gRootPolicy.header.totalBytes != header.totalBytes) {
    freeRootPolicy ();
    return status != kRootPolicyOk ? policyReason (status) : "policy region changed while it was read";
  }
  uint8_t *key = region + tableBytes;
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (vm_map_protect (kernel_map, storage, storage + tableBytes + keyBytes, VM_PROT_READ, TRUE) != KERN_SUCCESS) {
    freeRootPolicy ();
    return "policy read-only protection";
  }
  gRootPolicy.key = key;
  gRootDiagnostic.policyCount = gRootPolicy.header.count;
  IOLog ("NativeUfs: signed root policy read, %u entries\n", gRootPolicy.header.count);
  return nullptr;
}
#endif

}

// ---- Persistent Data store window (sheng-data-persist) ----------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the userdata partition. Mode 1 (boot argument sheng-data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The diagnostic log (sheng-log=1): always the last 32 MiB of userdata, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window (the render server's pipeline caches, kits/sheng BootLog.c). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;
// The reserve at the end of userdata that the Data store stays clear of: the log and its export region sit inside it (sheng-log=1), the rest
// is unused. sheng-log-reserve=<MiB>, default 1024; 0 gives all the room to Data and is only accepted with the log off. The store's place
// depends on it, so an image keeps one value for its whole life (a different value moves the store: the old one is read as blank).
constexpr uint32_t kDefaultReserveMiB = 1024;
static uint64_t reserveBlocks (void)
{
  static int64_t cached = -1;
  if (cached < 0) {
    uint32_t mib = kDefaultReserveMiB;
    if (!PE_parse_boot_argn ("sheng-log-reserve", &mib, sizeof (mib)) || mib > 16384) mib = kDefaultReserveMiB;
    cached = static_cast<int64_t> (mib) * 256;   // 4096-byte blocks
  }
  return static_cast<uint64_t> (cached);
}

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    const uint64_t reserve = reserveBlocks ();
    if (!blocks || reserve >= partition || blocks > partition - reserve) reason = "size";
    else if (partition - blocks - reserve < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The reserve at the end of the partition belongs to the diagnostic log (its last 32 MiB, with the export region below them);
      // the store ends just below it.
      uint64_t first = gStoreGeometry.last + 1 - reserve - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostRead4k (kUserdataLun, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    // Forced unit access: the block is on the medium when the command completes, so the overlay needs no cache flush.
    if (hostWrite4k (kUserdataLun, gStoreRegionFirst + rel, chunk, src, true) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_sync (void)
{
  return storeMode () == 2 && gStoreRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// ---- Diagnostic log window (sheng-log=1) --------------------------------------------------------------------------
// The board's log writer (kits/sheng BootLog.c) gets the last kLogReserveBlocks of userdata and the kExportBlocks below them, refused
// unless the partition holds them clear of the flashed root image plus a 4 MiB guard. Off without the boot argument.
static unsigned logMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-log", &value, sizeof (value)) || value > 1) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}

// Returns 1 with the window's first absolute LBA and size in 4096-byte blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open (uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!logMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gLogRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (reserveBlocks () < kLogReserveBlocks + kExportBlocks) reason = "reserve too small for the log";
    else if (partition - reserveBlocks () < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      uint64_t first = gStoreGeometry.last + 1 - kLogReserveBlocks;
      // One write window covers the export region and the log: [first - kExportBlocks, first + kLogReserveBlocks).
      if (!hostLogOpen (first - kExportBlocks, kExportBlocks + kLogReserveBlocks)) reason = "window refused";
      else {
        gLogRegionFirst = first;
        gLogRegionBlocks = kLogReserveBlocks;
        if (firstLba) *firstLba = first;
        if (blocks) *blocks = kLogReserveBlocks;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= gLogRegionBlocks || blocks > gLogRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (kUserdataLun, gLogRegionFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The export region below the log window: its size in blocks once the log is open, else 0; writes at a block offset inside it.
extern "C" uint64_t native_ufs_export_blocks (void) { return gLogRegionBlocks ? kExportBlocks : 0; }

extern "C" int native_ufs_export_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= kExportBlocks || blocks > kExportBlocks - rel) return 0;
  const uint64_t base = gLogRegionFirst - kExportBlocks;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (kUserdataLun, base + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_log_sync (void)
{
  return gLogRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// Re-read the GPT header and entry array and compare their CRCs with what was recorded at root start. 0 = unchanged.
extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (kUserdataLun, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (kUserdataLun, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

// One read-only window of the root image as a block device: 512-byte sectors on top of 4096-byte
// UFS blocks, so the unchanged dmg disk images (GPT with 512-byte sectors) parse as they do on
// the virtio path.
class NativeUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsBlock)
  uint64_t absolute = 0;      // byte offset of the window on the userdata LUN
  uint64_t bytes = 0;
  char label[32] = {};
  uint8_t *bounce = nullptr;
  IOLock *windowLock = nullptr;
  bool online = false;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool verifiedActive = false;
  struct vr_view verified = {};
  const uint8_t *envelope = nullptr;    // the selected VROOT01 envelope in the read-only policy copy
#endif
public:
  const char *windowLabel () const { return label; }
  bool configure (const char *name, uint64_t absoluteOffset, uint64_t size);
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool configureVerification ();
  IOReturn verifyWindowBlock (uint64_t index, const void *data, size_t length);
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const;
#endif
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("UFS userdata-LUN window, read-only"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeUfsBlock, IOBlockStorageDevice)

extern "C" {
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_bytes_read = 0;
volatile uint64_t native_ufs_block_failures = 0;
}

static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
// Passive diagnostic reader: cached software values only. It must remain safe
// when called from an IRQ while the transport is busy: no locks, allocations,
// MMIO reads, queue resets, or changes to the command/trust state. Fields can
// represent adjacent updates; this is explicitly not an atomic HW snapshot.
extern "C" void native_ufs_progress_text (char *out, size_t size)
{
  UfsRootDiagnostics sample = gRootDiagnostic;
  sample.published = native_ufs_blocks_published;
  sample.reads = native_ufs_block_reads;
  sample.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (out, size, sample, "cached values; not a new HW fault sample", "UFS CACHED SNAPSHOT");
}

static void rootDiagnosticFailure (const char *reason)
{
  // Kept even when the panic diagnostic is off, so the boot page can say why nothing was published.
  native_ufs_root_failure = reason;
  if (!gRootDiagnosticEnabled) return;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (gRootDiagnosticText, sizeof (gRootDiagnosticText), gRootDiagnostic, reason);
  strlcat (gRootDiagnosticText, gRootMediaText, sizeof (gRootDiagnosticText));
  // The console keeps only the newest rows; the beginning of this text (which names the reason) can scroll off.
  // Repeat the reason at the very end so the panic page always shows it.
  strlcat (gRootDiagnosticText, "\nWHY ", sizeof (gRootDiagnosticText));
  if (reason) strlcat (gRootDiagnosticText, reason, sizeof (gRootDiagnosticText));
  panic ("%s", gRootDiagnosticText);
}

bool NativeUfsBlock::configure (const char *name, uint64_t absoluteOffset, uint64_t size)
{
  if (!name || !ufsWindowGeometry (absoluteOffset, size)) {
    return false;
  }
  strlcpy (label, name, sizeof (label));
  absolute = absoluteOffset;
  bytes = size;
  windowLock = IOLockAlloc ();
  if (!windowLock) return false;
  bounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!bounce) {
    return false;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  // Reads are served only after each 4096-byte integrity block verifies against the owner-signed
  // policy; without a matching policy the device is never published.
  return configureVerification ();
#else
  return true;
#endif
}

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
// Selects this window's policy and opens it (vr_open, owner key, minimum generation 1). A window without
// exactly one capacity-matching, owner-signed envelope is never served.
bool NativeUfsBlock::configureVerification ()
{
  gRootDiagnostic.stage = "policy selection";
  gRootDiagnostic.windowBytes = bytes;
  gRootDiagnostic.policyCount = gRootPolicy.header.count;
  gRootDiagnostic.verify = 0xffffffff;
  if (!gRootPolicy.storage || !gRootPolicy.key) {
    gRootDiagnostic.stage = "no signed root policy";
    return false;
  }
  const uint8_t *region = reinterpret_cast<const uint8_t *> (gRootPolicy.storage);
  unsigned index = 0;
  switch (selectRootPolicy (region, gRootPolicy.header, bytes, index)) {
  case kRootPolicySelected: break;
  case kRootPolicyDuplicate: gRootDiagnostic.stage = "two policies match the window"; return false;
  default: gRootDiagnostic.stage = "no policy matches the window"; return false;
  }
  const RootPolicyEntry &entry = gRootPolicy.header.entry[index];
  struct vr_view view = {};
  int status = vr_open (region + entry.offset, static_cast<size_t> (entry.bytes), gRootPolicy.key, 1,
                        &vr_xnu_crypto, &view);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  if (status != VR_OK) return false;
  if (view.image_bytes != bytes) {
    gRootDiagnostic.stage = "signed image size differs from the window";
    return false;
  }
  verified = view;
  envelope = region + entry.offset;
  verifiedActive = true;
  native_ufs_verified_windows = native_ufs_verified_windows + 1;
  return true;
}

IOReturn NativeUfsBlock::verifyWindowBlock (uint64_t index, const void *data, size_t length)
{
  if (!verifiedActive) return kIOReturnNotReady;
  if (vr_verify_block (&verified, index, data, length, &vr_xnu_crypto) != VR_OK) {
    __atomic_fetch_add (&native_ufs_integrity_failures, 1, __ATOMIC_RELAXED);
    return kIOReturnIOError;
  }
  __atomic_fetch_add (&native_ufs_verified_blocks, 1, __ATOMIC_RELAXED);
  return kIOReturnSuccess;
}

bool NativeUfsBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedActive || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      expected[at++] = '-';
    }
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) {
    return false;
  }
  if (manifestDigest) {
    // Hash only the authenticated envelope, excluding allocation padding and key.
    const uint64_t envelopeBytes = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, envelope, static_cast<size_t> (envelopeBytes));
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}
#endif

bool NativeUfsBlock::start (IOService *provider)
{
  if (!bounce || !windowLock || !IOBlockStorageDevice::start (provider)) {
    return false;
  }
  online = true;
  registerService ();
  return true;
}

IOReturn NativeUfsBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= bytes / 512 ||
      count > bytes / 512 - block || buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) {
    return kIOReturnNotWritable;
  }
  if (!online) {
    return kIOReturnNotReady;
  }
  struct Operations {
    IOLock *mutex;
    IOMemoryDescriptor *buffer;
    NativeUfsBlock *owner;
    void lock () { IOLockLock (mutex); }
    void unlock () { IOLockUnlock (mutex); }
    IOReturn read (uint64_t lba, uint32_t blocks, void *out) { return hostRead4k (kUserdataLun, lba, blocks, out); }
    IOReturn verify (uint64_t index, const void *data, size_t length) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      return owner->verifyWindowBlock (index, data, length);
#else
      (void) index; (void) data; (void) length;
      return kIOReturnSuccess;
#endif
    }
    uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
      uint64_t copied = buffer->writeBytes (offset, data, length);
      if (copied == length) {
        __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add (&native_ufs_bytes_read, length, __ATOMIC_RELAXED);
      }
      return copied;
    }
  } operations = { windowLock, buffer, this };
  UfsWindowResult outcome = ufsReadWindow (operations, absolute, bytes, block, count, bounce);
  IOReturn result = outcome.status;
  uint64_t done = outcome.done;
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    gRootDiagnostic.result = static_cast<uint32_t> (result);
    rootDiagnosticFailure ("root window read failed");
  }
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

void NativeUfsBlock::free ()
{
  if (bounce) {
    IOFree (bounce, kDataMax);
    bounce = nullptr;
  }
  if (windowLock) {
    IOLockFree (windowLock);
    windowLock = nullptr;
  }
  IOBlockStorageDevice::free ();
}

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
// Called from the root authorization query in NativeVirtioBlock: -1 when `provider` is not a
// UFS window, otherwise 1 if its signed volume UUID equals `uuid`, else 0.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeUfsBlock *block = OSDynamicCast (NativeUfsBlock, provider);
  if (!block) {
    return -1;
  }
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif

// Ordered publication of the root windows. The original owner-trust code resolves the SystemOS
// volume by UUID immediately after the root is mounted, so SystemOS must already have published its
// volume media by then; the OS window (whose volume is the root being waited for) goes last.
static NativeUfsBlock *gPendingDevice[4];
static unsigned gPending;
static bool gPendingAttached[4];
static UfsRootPlan gPublicationPlan;
static NativeUfs *gPendingProvider;

static bool windowMediaExists (NativeUfsBlock *window)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  bool found = false;
  for (unsigned matches = 0; iterator && matches < 64 && !found; ++matches) {
    IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
    if (!media) break;
    IORegistryIterator *parents = IORegistryIterator::iterateOver (
      media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (parents) {
      found = ufsMediaHasAncestor (*parents, window);
      parents->release ();
    }
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
  return found;
}

static void captureMissingMedia (const char *label)
{
  int used = snprintf (gRootMediaText, sizeof (gRootMediaText), "Waiting for %s window media\n", label);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  unsigned count = 0;
  while (iterator && used < int (sizeof (gRootMediaText)) - 180 && count++ < 8) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) break;
    OSString *id = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gRootMediaText + used, sizeof (gRootMediaText) - used,
                      "media %.24s bytes=%llx uuid=%.36s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", size ? size->unsigned64BitValue () : 0ULL,
                      id ? id->getCStringNoCopy () : "-");
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
}

// Used only for objects that were never started/published. Started objects
// retain their owner references even on a later failure; clients can have I/O.
static void discardPendingWindow (unsigned index)
{
  NativeUfsBlock *device = gPendingDevice[index];
  if (!device) return;
  if (gPendingAttached[index]) device->detach (gPendingProvider);
  gPendingAttached[index] = false;
  gPendingDevice[index] = nullptr;
  device->release ();
}

static void publishThread (void *, wait_result_t)
{
  kprintf ("d114 pub in t=%llx\n", mach_absolute_time ());
  struct Operations {
    bool start (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "root window service start";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Start);
      gRootDiagnostic.publicationWindow = index;
      if (!device->start (gPendingProvider)) return false;
      native_ufs_blocks_published = native_ufs_blocks_published + 1;
      IOLog ("NativeUfs: published %s\n", device->windowLabel ());
      kprintf ("d114 pub st %s t=%llx\n", device->windowLabel (), mach_absolute_time ());
      return true;
    }
    bool wait (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "media publication";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Media);
      gRootDiagnostic.publicationWindow = index;
      bool ready = false;
      // Diagnostic mode affects reporting only, never the dependency gate.
      for (unsigned tick = 0; tick <= 300; ++tick) {
        ready = windowMediaExists (device);
        if (ready || tick == 300) break;
        IOSleep (100);
      }
      IOLog ("NativeUfs: %s media %s\n", device->windowLabel (), ready ? "ready" : "not ready");
      kprintf ("d114 media %s r=%u t=%llx\n", device->windowLabel (), ready ? 1U : 0U,
          mach_absolute_time ());
      if (!ready) captureMissingMedia (device->windowLabel ());
      return ready;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations;
  UfsRootPublicationResult result = ufsPublishRootWindows (operations, gPublicationPlan);
  if (result.phase != UfsRootPhase::Complete) {
    gRootDiagnostic.result = result.phase == UfsRootPhase::Start ? kIOReturnIOError : kIOReturnTimeout;
    const char *reason = result.phase == UfsRootPhase::Start ? "root window service start failed" :
      (!strcmp (gPendingDevice[result.index]->windowLabel (), "SystemOS") ?
       "SystemOS window media not found" : "root window media not found");
    IOLog ("NativeUfs: root publisher stopped at window %u; published=%u\n", result.index, result.published);
    // Start failures have already released their unpublished object; do not
    // dereference it here. The media-failure object is deliberately retained.
    rootDiagnosticFailure (reason);
  }
  gPendingProvider->release ();  // Balanced with successful thread dispatch.
}

// Memory snapshot (native-ufs-probe=13): root mode as usual, then, after a delay, the kernel's own
// page counters on the panic screen. Observation only.
extern "C" {
extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
  vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count;
extern uint32_t vm_page_pages;
extern unsigned int memorystatus_available_pages;
extern int nprocs;
}

static void statsThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  panic ("NATIVE_UFS memory snapshot after %u s\n"
         "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
         "available=%u max_mem=%llx procs=%d blocks=%llu\n",
         delay, vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
         vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
         memorystatus_available_pages, max_mem, nprocs, native_ufs_block_reads);
}

// Storage discovery report (native-ufs-probe=12): a separate thread waits for the original
// IOStorageFamily to publish IOMedia for the root windows, then shows what exists on the screen.
// It must not run inside the matching thread that is starting NativeUfs.
static char gMediaReport[3000];

static void mediaReportThread (void *, wait_result_t)
{
  unsigned count = 0;
  for (unsigned tick = 0; tick < 200; tick++) {      // up to 20 s
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    count = 0;
    if (iterator) {
      while (iterator->getNextObject ()) {
        count++;
      }
      iterator->release ();
    }
    if (matching) {
      matching->release ();
    }
    if (count >= 4) {
      break;
    }
    IOSleep (100);
  }
  IOSleep (500);
  int used = snprintf (gMediaReport, sizeof (gMediaReport),
                       "NATIVE_UFS media report: published=%u reads=%llu failures=%llu media=%u\n",
                       native_ufs_blocks_published, native_ufs_block_reads, native_ufs_block_failures, count);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && used < int (sizeof (gMediaReport)) - 200) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gMediaReport + used, sizeof (gMediaReport) - used, "%s %s size=%llx uuid=%s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", content ? content->getCStringNoCopy () : "-",
                      size ? size->unsigned64BitValue () : 0ULL, uuid ? uuid->getCStringNoCopy () : "-");
  }
  if (iterator) {
    iterator->release ();
  }
  if (matching) {
    matching->release ();
  }
  panic ("%s", gMediaReport);
}

bool NativeUfs::start (IOService *provider)
{
  unsigned probe = 0, rootMode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &probe, sizeof (probe));
  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));
  native_ufs_probe_requested = probe != 0;
  // Default: match for observability only, never touch the controller. Hardware work runs once,
  // from the root-device personality; the ufshc nub match stays observation-only.
  if ((!probe && !rootMode) || !OSDynamicCast (IOPlatformExpertDevice, provider)) {
    return false;
  }
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("native-ufs-diagnostics", &diagnostic, sizeof (diagnostic));
  gRootDiagnosticEnabled = diagnostic == 1;
  if (probe == 10) {
    // Read-only check before anything is flashed: userdata-LUN GPT -> "userdata" -> its first block.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root check: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root check: %s", why);
    }
    panic ("NATIVE_UFS root check\n"
           "userdata first=%llx last=%llx\n"
           "block0 magic %08x %08x ext4magic=%04x ext4blocks=%u\n"
           "manifest status=%u windows=%u",
           layout->userdataFirst, layout->userdataLast, layout->magic0, layout->magic1,
           layout->ext4Magic, layout->ext4Blocks, layout->manifestStatus, layout->windows);
  }
  if (probe == 11) {
    // After the root image is flashed: manifest, then each window's own GPT (sector 1 of the
    // attached dmg disk image carries "EFI PART"). Read-only.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root windows: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root windows: %s", why);
    }
    uint32_t sig[4][2] = {};
    uint32_t seen[4] = {};
    for (uint32_t index = 0; index < layout->windows && index < 4; index++) {
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (hostRead4k (kUserdataLun, absolute / 4096, 1, gScratch) == kIOReturnSuccess) {
        seen[index] = 1;
        sig[index][0] = le32 (gScratch + 512);
        sig[index][1] = le32 (gScratch + 516);
      }
    }
    panic ("NATIVE_UFS root windows\n"
           "userdata first=%llx last=%llx manifest status=%u windows=%u\n"
           "w0 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w1 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w0 sha %08x %08x %08x %08x\n"
           "w1 sha %08x %08x %08x %08x",
           layout->userdataFirst, layout->userdataLast, layout->manifestStatus, layout->windows,
           layout->window[0].name, layout->window[0].offset, layout->window[0].size, sig[0][0], sig[0][1], seen[0],
           layout->window[1].name, layout->window[1].offset, layout->window[1].size, sig[1][0], sig[1][1], seen[1],
           be32w (layout->window[0].sha256), be32w (layout->window[0].sha256 + 4), be32w (layout->window[0].sha256 + 8), be32w (layout->window[0].sha256 + 12),
           be32w (layout->window[1].sha256), be32w (layout->window[1].sha256 + 4), be32w (layout->window[1].sha256 + 8), be32w (layout->window[1].sha256 + 12));
  }
  if (probe == 14) {
    // Immediate memory snapshot at root-device start, before the userspace that triggers jetsam.
    panic ("NATIVE_UFS early memory snapshot\n"
           "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
           "available=%u max_mem=%llx procs=%d",
           vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
           vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
           memorystatus_available_pages, max_mem, nprocs);
  }
  if (probe == 13) {
    unsigned delay = 12;
    PE_parse_boot_argn ("native-ufs-stats-delay", &delay, sizeof (delay));
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS memory snapshot: root start failed");
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (statsThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS memory snapshot: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 12) {
    bool published = startRoot (provider);
    if (!published) {
      panic ("NATIVE_UFS media report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (mediaReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS media report: cannot start the report thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (rootMode && !probe) {
    return startRoot (provider);
  }
  return false;
}

// Boot the root image's windows as block devices. Soft-fails (logs, publishes nothing).
bool NativeUfs::startRoot (IOService *provider)
{
  const char *why = "";
  native_ufs_root_step = 1;
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: root bring-up failed: %s\n", why);
    return false;
  }
  kprintf ("d114 bring ok t=%llx\n", mach_absolute_time ());
  native_ufs_root_step = 2;
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    rootDiagnosticFailure (layout ? why : "root layout allocation failed");
    IOLog ("NativeUfs: root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  kprintf ("d114 layout w=%u first=%llx last=%llx img=%llx ms=%u t=%llx\n",
      layout->windows, layout->userdataFirst, layout->userdataLast, layout->imageBytes,
      layout->manifestStatus, mach_absolute_time ());
  {
    uint64_t benchLba = layout->userdataFirst + layout->window[0].offset / 4096;
    uint64_t benchStart = mach_absolute_time ();
    unsigned benchOk = 0;
    for (unsigned i = 0; i < 128; ++i) {
      if (hostRead4k (kUserdataLun, benchLba + i * 32, 32, gScratch) == kIOReturnSuccess) ++benchOk;
    }
    kprintf ("d144 bench n=128 ok=%u t0=%llx t1=%llx\n", benchOk, benchStart,
        mach_absolute_time ());
  }
  native_ufs_root_step = 3;
  if (layout->manifestStatus != 0) {
    rootDiagnosticFailure ("root manifest rejected");
    IOLog ("NativeUfs: no valid root manifest in userdata (status %u, magic %08x %08x)\n",
           layout->manifestStatus, layout->magic0, layout->magic1);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  // The owner-signed policy region lives after the flashed root image; it is loaded and verified
  // before any window exists, because each window's vr_view opens on it.
  native_ufs_root_step = 4;
  kprintf ("d114 policy in t=%llx\n", mach_absolute_time ());
  if (const char *policyWhy = loadRootPolicy (layout)) {
    rootDiagnosticFailure (policyWhy);
    IOLog ("NativeUfs: signed root policy: %s\n", policyWhy);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  kprintf ("d114 policy ok bytes=%llx t=%llx\n",
      static_cast<uint64_t> (gRootPolicy.header.totalBytes), mach_absolute_time ());
#endif
  gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                     layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount };
  const char *names[4] = {};
  for (unsigned i = 0; i < layout->windows; ++i) names[i] = layout->window[i].name;
  if (!ufsBuildRootPlan (names, layout->windows, gPublicationPlan)) {
    rootDiagnosticFailure ("root windows require distinct OS and SystemOS");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  kprintf ("d114 plan w=%u t=%llx\n", layout->windows, mach_absolute_time ());
  if (!IOService::start (provider)) {
    rootDiagnosticFailure ("root IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  kprintf ("d114 svc ok t=%llx\n", mach_absolute_time ());
  // Prepare all windows or publish none. Diagnostics never decide policy.
  gPending = 0;
  gPendingProvider = this;
  struct Operations {
    NativeUfs *owner;
    RootLayout *layout;
    bool prepare (unsigned index) {
      gRootDiagnostic.stage = "root window allocation/configuration";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Prepare);
      gRootDiagnostic.publicationWindow = index;
      gRootDiagnostic.windowBytes = layout->window[index].size;
      NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
      gPendingDevice[index] = device;
      gPendingAttached[index] = false;
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
          !device->configure (layout->window[index].name, absolute, layout->window[index].size)) return false;
      gRootDiagnostic.stage = "root window attachment";
      if (!device->attach (owner)) return false;
      gPendingAttached[index] = true;
      ++gPending;
      gRootDiagnostic.prepared = gPending;
      return true;
    }
    bool validate () {
      // Plain mode: the windows are published read-only and the root volume is matched
      // by the UUID carried in the ADT, not by an owner-signed policy.
      gRootDiagnostic.stage = "window identities";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Identity);
      return gPending >= 2;
    }
    bool schedule () {
      gRootDiagnostic.stage = "root publisher thread";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Schedule);
      gRootDiagnostic.publicationWindow = 0xffffffff;
      thread_t thread = nullptr;
      owner->retain ();
      if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
        owner->release ();
        return false;
      }
      thread_deallocate (thread);
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations = { this, layout };
  UfsRootPublicationResult result = ufsPrepareRootWindows (operations, layout->windows);
  IOFree (layout, sizeof (RootLayout));
  if (result.phase != UfsRootPhase::Complete) {
    gPending = 0;
    gRootDiagnostic.result = result.phase == UfsRootPhase::Identity ? kIOReturnNotPrivileged :
      result.phase == UfsRootPhase::Schedule ? kIOReturnNoResources : kIOReturnError;
    IOLog ("NativeUfs: root preparation failed phase=%u window=%u; nothing published\n",
           static_cast<unsigned> (result.phase), result.index);
    IOService::stop (provider);
    rootDiagnosticFailure (result.phase == UfsRootPhase::Schedule ? "root publisher thread creation failed" :
                          result.phase == UfsRootPhase::Identity ? "signed window identities rejected" :
                          "root window preparation failed");
    return false;
  }
  kprintf ("d114 prep n=%u t=%llx\n", gPending, mach_absolute_time ());
  return true;
}

void NativeUfs::stop (IOService *provider)
{
  fCmdDesc.reset ();
  fMap.reset ();
  IOService::stop (provider);
}
