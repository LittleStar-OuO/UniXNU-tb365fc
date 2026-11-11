// SPDX-License-Identifier: BSD-2-Clause-Patent
// MT6835 MFG bring-up probe (diag157/158). Mirrors the stock mtk_gpufreq
// __gpufreq_power_control order that the vendor module actually uses:
// MFG0 -> MFG1 -> reference clock gate -> MFG2 -> MFG3 -> MFG_CFG gate ->
// MFG_TOP_CONFIG. diag155 touched MFG_CFG before the domains were powered and
// took an SError; every MFG_CFG access now waits for all four power-good bits.
// diag157 read the Mali identification registers. diag158 adds a GPU soft
// reset, core power-on, an AArch64-4K page table for one address space and a
// WRITE_VALUE job on job slot 0, polled without interrupts.
// Gated by native-mali-probe=1.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSPtr.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/task.h>
#include "OriginalAcceleratorABI.h"
#include "NativeMaliABI.h"

extern "C" void native_boot_log_flush(void) __attribute__((weak_import));

// The display probe calls this after OSMetaClass::preModLoad. The extern
// declaration also gives the compiler-emitted static initializer external
// linkage, so the archive link can resolve it.
extern "C" void native_mali_static_init(void) __asm__("__GLOBAL__sub_I_NativeMali.cpp");

void native_mali_service_start(void);

namespace {

struct Window {
    OSSharedPtr<IOMemoryMap> map;
    volatile uint32_t *base;
};

bool mapWindow(uint64_t phys, uint64_t length, Window &w)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) {
        return false;
    }
    w.map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    if (!w.map) {
        return false;
    }
    w.base = reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress());
    return w.base != nullptr;
}

inline uint32_t r32(volatile uint32_t *b, uint32_t off) { return b[off / 4]; }
inline void w32(volatile uint32_t *b, uint32_t off, uint32_t v) { b[off / 4] = v; }
inline void or32(volatile uint32_t *b, uint32_t off, uint32_t v) { w32(b, off, r32(b, off) | v); }
inline void and32(volatile uint32_t *b, uint32_t off, uint32_t v) { w32(b, off, r32(b, off) & v); }

bool waitMask(volatile uint32_t *b, uint32_t off, uint32_t mask, uint32_t value, int loops)
{
    for (int i = 0; i < loops; i++) {
        if ((r32(b, off) & mask) == value) {
            return true;
        }
        IOSleep(5);
    }
    return (r32(b, off) & mask) == value;
}

struct Domain {
    uint32_t ctl;
    uint32_t bit;
    const char *name;
};

const Domain kDomains[4] = {
    {0xeb8, 0x02, "mfg0"},
    {0xebc, 0x04, "mfg1"},
    {0xec0, 0x08, "mfg2"},
    {0xec4, 0x10, "mfg3"},
};

// Release reset/isolation (bits 2,3), wait for power-good in both PWR_STATUS
// and PWR_STATUS_2ND, then clear clock-disable (bit 4) and secondary power
// (bit 1), raise PWR_ON (bit 0) and clear the secondary request (bit 8).
// MFG1..3 additionally wait for bit 12 to fall. INFRA_AO bus-protection
// writes follow for MFG0/MFG1 exactly as the stock driver does.
bool powerDomain(volatile uint32_t *spm, volatile uint32_t *infra, int index)
{
    const uint32_t ctl = kDomains[index].ctl;
    const uint32_t bit = kDomains[index].bit;

    or32(spm, ctl, 0x4);
    or32(spm, ctl, 0x8);
    bool good = false;
    for (int i = 0; i < 501; i++) {
        if ((r32(spm, 0xf4c) & bit) && (r32(spm, 0xf50) & bit)) {
            good = true;
            break;
        }
        IOSleep(5);
    }
    if (!good) {
        return false;
    }
    IOSleep(30);
    and32(spm, ctl, ~0x10u);
    and32(spm, ctl, ~0x2u);
    or32(spm, ctl, 0x1);

    if (index != 0) {
        and32(spm, ctl, ~0x100u);
        if (!waitMask(spm, ctl, 0x1000, 0, 501)) {
            return false;
        }
    }

    switch (index) {
    case 0:
        w32(infra, 0xc48, 0x200);
        w32(infra, 0xca8, 0x10);
        break;
    case 1:
        w32(infra, 0xca8, 0x8);
        w32(infra, 0xca8, 0x4);
        w32(infra, 0xc68, 0xc0000);
        w32(infra, 0xca8, 0x2);
        w32(infra, 0xc58, 0x100000);
        w32(infra, 0xca8, 0x1);
        break;
    default:
        break;
    }
    return true;
}

void mfgTopInit(volatile uint32_t *m)
{
    or32(m, 0x20, 0x3c00000);
    and32(m, 0xb0, ~0x600u);
    or32(m, 0xb4, 0x10);
    w32(m, 0xb0, (r32(m, 0xb0) & 0x80000u) | 0xfff7feffu);
    or32(m, 0x24, 0x1);
    or32(m, 0xf60, 0x1);
    const uint32_t a = 0x0808ff81u;
    const uint32_t b = 0x4040ff81u;
    w32(m, 0x8a0, a);
    w32(m, 0x8a8, a);
    w32(m, 0x8c0, a);
    w32(m, 0x8c8, a);
    w32(m, 0x8b0, b);
    w32(m, 0x8b8, b);
    w32(m, 0x8d0, b);
    w32(m, 0x8d8, b);
}

// GPU control plane (panfrost_gpu.c order): reset, then power the L2, shader
// and tiler core groups, polling the READY bitmaps. Register offsets follow
// panfrost_regs.h: GPU_CMD 0x30, INT 0x20-0x2c, PWRON/READY/PWRTRANS below
// 0x270, JOB_INT 0x1000, job slot 0 0x1800, MMU AS0 0x2400.
bool gpuSoftReset(volatile uint32_t *g)
{
    w32(g, 0x28, 0);
    w32(g, 0x24, 1u << 8);
    w32(g, 0x30, 0x1);
    bool ok = false;
    for (int i = 0; i < 200; i++) {
        if (r32(g, 0x20) & (1u << 8)) {
            ok = true;
            break;
        }
        IOSleep(1);
    }
    if (!ok) {
        w32(g, 0x30, 0x2);
        for (int i = 0; i < 200; i++) {
            if (r32(g, 0x20) & (1u << 8)) {
                ok = true;
                break;
            }
            IOSleep(1);
        }
    }
    w32(g, 0x24, 0xffffffff);
    w32(g, 0x28, 0xffffffff);
    w32(g, 0x1004, 0xffffffff);
    w32(g, 0x1008, 0xffffffff);
    return ok;
}

bool gpuPowerOn(volatile uint32_t *g, uint32_t l2, uint32_t shader, uint32_t tiler)
{
    w32(g, 0x1a0, l2);
    bool l2ok = waitMask(g, 0x160, l2, l2, 200);
    w32(g, 0x180, shader);
    bool shok = waitMask(g, 0x140, shader, shader, 200);
    w32(g, 0x190, tiler);
    bool tlok = waitMask(g, 0x150, tiler, tiler, 20);
    return l2ok && shok && tlok;
}

struct GpuMem {
    OSPtr<IOBufferMemoryDescriptor> desc;
    uint8_t *cpu;
    uint64_t phys;
    size_t len;
};

bool allocGpuMem(GpuMem &m, size_t len)
{
    m.desc = IOBufferMemoryDescriptor::withOptions(
        (IOOptionBits)kIOMemoryPhysicallyContiguous | (IOOptionBits)kIODirectionInOut,
        len, 0x1000);
    if (!m.desc) {
        return false;
    }
    IOByteCount segLen = 0;
    m.phys = m.desc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    m.cpu = reinterpret_cast<uint8_t *>(m.desc->getBytesNoCopy());
    m.len = len;
    return m.phys != 0 && segLen >= len && m.cpu != nullptr;
}

inline uint32_t asReg(int as, uint32_t reg) { return 0x2400 + ((uint32_t)as << 6) + reg; }

bool mmuWaitIdle(volatile uint32_t *g, int as)
{
    for (int i = 0; i < 200; i++) {
        if (!(r32(g, asReg(as, 0x28)) & 1)) {
            return true;
        }
        IOSleep(1);
    }
    return false;
}

// AArch64-4K translation: table descriptors 0x3, page descriptors carry AF,
// inner-shareable and attribute index 0. MEMATTR 0x4c (all eight entries) is
// non-cacheable, and the page-table walker is non-cacheable as well, so the
// GPU sees DRAM directly; IOKit incoherent store/flush handles the CPU side.
void mmuProgram(volatile uint32_t *g, int as, uint64_t pgdPa, uint32_t transcfg, uint32_t memattr)
{
    mmuWaitIdle(g, as);
    w32(g, asReg(as, 0x18), 5); // FLUSH_MEM before reprogramming
    mmuWaitIdle(g, as);
    w32(g, asReg(as, 0x00), (uint32_t)pgdPa);
    w32(g, asReg(as, 0x04), (uint32_t)(pgdPa >> 32));
    w32(g, asReg(as, 0x08), memattr);
    w32(g, asReg(as, 0x0c), memattr);
    w32(g, asReg(as, 0x30), transcfg);
    w32(g, asReg(as, 0x34), 0);
    w32(g, asReg(as, 0x18), 1);
    mmuWaitIdle(g, as);
}

void mmuDisable(volatile uint32_t *g, int as)
{
    mmuWaitIdle(g, as);
    w32(g, asReg(as, 0x00), 0);
    w32(g, asReg(as, 0x04), 0);
    w32(g, asReg(as, 0x08), 0);
    w32(g, asReg(as, 0x0c), 0);
    w32(g, asReg(as, 0x30), 1);
    w32(g, asReg(as, 0x34), 0);
    w32(g, asReg(as, 0x18), 1);
    mmuWaitIdle(g, as);
}

void putWriteValueJob(uint8_t *jd, uint32_t index, uint32_t dep1, uint32_t dep2,
    uint64_t nextVa, uint64_t targetVa, uint64_t faultVa, uint32_t wtype = 3,
    uint64_t imm = 0)
{
    for (int i = 0; i < 0x40; i++) {
        jd[i] = 0;
    }
    *reinterpret_cast<uint64_t *>(jd + 0x08) = faultVa;
    *reinterpret_cast<uint32_t *>(jd + 0x10) = 1u | (2u << 1) | (index << 16);
    *reinterpret_cast<uint32_t *>(jd + 0x14) = dep1 | (dep2 << 16);
    *reinterpret_cast<uint64_t *>(jd + 0x18) = nextVa;
    *reinterpret_cast<uint64_t *>(jd + 0x20) = targetVa;
    *reinterpret_cast<uint32_t *>(jd + 0x28) = wtype;
    *reinterpret_cast<uint64_t *>(jd + 0x30) = imm;
}

// CACHE_FLUSH job (type 3). The payload format is not documented; the first
// word carries the flush selector and is kept zero unless a test sets it.
void putCacheFlushJob(uint8_t *jd, uint32_t index, uint32_t dep1, uint32_t dep2,
    uint64_t nextVa, uint64_t faultVa, uint32_t flushType)
{
    for (int i = 0; i < 0x40; i++) {
        jd[i] = 0;
    }
    *reinterpret_cast<uint64_t *>(jd + 0x08) = faultVa;
    *reinterpret_cast<uint32_t *>(jd + 0x10) = 1u | (3u << 1) | (index << 16);
    *reinterpret_cast<uint32_t *>(jd + 0x14) = dep1 | (dep2 << 16);
    *reinterpret_cast<uint64_t *>(jd + 0x18) = nextVa;
    *reinterpret_cast<uint32_t *>(jd + 0x20) = flushType;
}

bool submitAndWait(volatile uint32_t *g, uint64_t headVa, uint32_t shader,
    uint32_t jscfg, uint32_t *rawOut, uint32_t *jsstOut)
{
    w32(g, 0x1004, 0xffffffff);
    w32(g, 0x1860, 0);
    w32(g, 0x1850, shader);
    w32(g, 0x1854, 0);
    w32(g, 0x1840, (uint32_t)headVa);
    w32(g, 0x1844, (uint32_t)(headVa >> 32));
    w32(g, 0x1858, jscfg);
    w32(g, 0x1860, 1);

    uint32_t raw = 0;
    for (int i = 0; i < 400; i++) {
        raw = r32(g, 0x1000);
        if (raw & 0x00010001u) {
            break;
        }
        IOSleep(5);
    }
    *rawOut = raw;
    *jsstOut = r32(g, 0x1824);
    return (raw & 1) != 0;
}

// Single address space over the 2 MiB L2[1] window (VA 0x200000..0x3fffff).
// The walk tables live in their own allocation; buffers are page-granular
// bump allocations mapped with the PROVEN Mali-LPAE block PTE.
struct MaliAs {
    // L0, L1, L2 and four L3 tables: four 2 MiB windows, 8 MiB of user VA.
    static const uint32_t kWindows = 4;
    static const uint32_t kWindowBytes = 0x200000;
    GpuMem tab;
    uint32_t usedPages;
    uint64_t baseVa;

    bool init(uint64_t va)
    {
        if (!allocGpuMem(tab, 0x4000 + kWindows * 0x1000)) {
            return false;
        }
        baseVa = va;
        usedPages = 0;
        uint64_t *l0 = reinterpret_cast<uint64_t *>(tab.cpu + 0x0000);
        uint64_t *l1 = reinterpret_cast<uint64_t *>(tab.cpu + 0x1000);
        uint64_t *l2t = reinterpret_cast<uint64_t *>(tab.cpu + 0x2000);
        uint32_t l2i = (uint32_t)((va >> 21) & 0x1ff);
        l0[0] = (tab.phys + 0x1000) | 0x3;
        l1[(va >> 30) & 0x1ff] = (tab.phys + 0x2000) | 0x3;
        for (uint32_t w = 0; w < kWindows; w++) {
            l2t[l2i + w] = (tab.phys + 0x4000 + w * 0x1000) | 0x3;
        }
        return true;
    }

    uint64_t allocVa(size_t len)
    {
        uint32_t pages = (uint32_t)((len + 0xfff) >> 12);
        if (!pages || usedPages + pages > kWindows * 512) {
            return 0;
        }
        uint64_t va = baseVa + (uint64_t)usedPages * 0x1000;
        usedPages += pages;
        return va;
    }

    void mapRange(uint64_t va, uint64_t pa, size_t len)
    {
        uint64_t *l3 = reinterpret_cast<uint64_t *>(
            tab.cpu + 0x4000 + ((va - baseVa) >> 21) * 0x1000);
        uint32_t first = (uint32_t)((va >> 12) & 0x1ff);
        uint32_t pages = (uint32_t)((len + 0xfff) >> 12);
        for (uint32_t i = 0; i < pages; i++) {
            l3[first + i] = (pa + (uint64_t)i * 0x1000) | 0x2c1;
        }
    }
};

// Kernel device object matching the shape of the future user-client ABI:
// opaque BO handles, explicit VA mapping, one synchronous submission and a
// fence counter. All cache maintenance the GPU requires is done here.
struct MaliBo {
    GpuMem mem;
    uint64_t va;
    uint32_t pages;
    bool used;
};

struct MaliDevice {
    static const int kMaxBo = kMaliAbiMaxBo;
    static const uint64_t kVaBase = 0x200000;
    volatile uint32_t *gpu = nullptr;
    uint32_t shader = 0;
    MaliAs as;
    MaliBo bo[kMaxBo];
    uint32_t fence = 0;

    bool init(volatile uint32_t *g)
    {
        gpu = g;
        const uint32_t l2 = r32(g, 0x120);
        const uint32_t tiler = r32(g, 0x110);
        shader = r32(g, 0x100);
        if (!l2 || !shader || !tiler || !r32(g, 0x18) || !r32(g, 0x1c)) {
            return false;
        }
        if (!gpuSoftReset(g) || !gpuPowerOn(g, l2, shader, tiler)) {
            return false;
        }
        if (!as.init(kVaBase)) {
            return false;
        }
        as.tab.desc->performOperation(kIOMemoryIncoherentIOStore, 0, as.tab.len);
        mmuProgram(g, 0, as.tab.phys | 0x7, 0, 0x008d0888u);
        return true;
    }

    int boAlloc(size_t bytes)
    {
        for (int i = 0; i < kMaxBo; i++) {
            if (bo[i].used) {
                continue;
            }
            if (!allocGpuMem(bo[i].mem, bytes)) {
                return -1;
            }
            const uint64_t va = as.allocVa(bytes);
            if (!va) {
                bo[i].mem.desc->release();
                bo[i].mem = GpuMem{};
                return -1;
            }
            as.mapRange(va, bo[i].mem.phys, bytes);
            as.tab.desc->performOperation(kIOMemoryIncoherentIOStore, 0, as.tab.len);
            bo[i].pages = (uint32_t)((bytes + 0xfff) >> 12);
            bo[i].va = va;
            bo[i].used = true;
            return i;
        }
        return -1;
    }

    bool boMap(int h, uint64_t va)
    {
        if (h < 0 || h >= kMaxBo || !bo[h].used || (va & 0xfff)) {
            return false;
        }
        if (va < as.baseVa ||
            va + bo[h].mem.len > as.baseVa + MaliAs::kWindows * MaliAs::kWindowBytes) {
            return false;
        }
        as.mapRange(va, bo[h].mem.phys, bo[h].mem.len);
        bo[h].va = va;
        return true;
    }

    void boFree(int h)
    {
        if (h < 0 || h >= kMaxBo || !bo[h].used) {
            return;
        }
        // The descriptor stays retained by the allocator; the slot is simply
        // no longer addressable from the device API.
        bo[h].va = 0;
        bo[h].used = false;
    }

    uint8_t *boCpu(int h)
    {
        return (h >= 0 && h < kMaxBo && bo[h].used) ? bo[h].mem.cpu : nullptr;
    }

    uint64_t boGpu(int h)
    {
        return (h >= 0 && h < kMaxBo && bo[h].used) ? bo[h].va : 0;
    }

    void storeAll()
    {
        as.tab.desc->performOperation(kIOMemoryIncoherentIOStore, 0, as.tab.len);
        for (int i = 0; i < kMaxBo; i++) {
            if (bo[i].used) {
                bo[i].mem.desc->performOperation(kIOMemoryIncoherentIOStore, 0,
                    bo[i].mem.len);
            }
        }
    }

    void flushAll()
    {
        for (int i = 0; i < kMaxBo; i++) {
            if (bo[i].used) {
                bo[i].mem.desc->performOperation(kIOMemoryIncoherentIOFlush, 0,
                    bo[i].mem.len);
            }
        }
    }

    bool submit(uint64_t headVa, uint32_t *raw, uint32_t *jsst)
    {
        storeAll();
        const bool done = submitAndWait(gpu, headVa, shader, 0x00083700u, raw, jsst);
        flushAll();
        fence++;
        return done;
    }
};

// One GPU core shared by the plain service and the Metal accelerator.
struct MaliCore {
    Window gpuWin;
    MaliDevice dev;
    bool up;
};
static MaliCore *gMaliCore;

static MaliCore *maliCoreGet()
{
    if (gMaliCore) {
        return gMaliCore;
    }
    MaliCore *core = new MaliCore;
    if (!core) {
        return nullptr;
    }
    core->up = false;
    if (mapWindow(0x13000000, 0x4000, core->gpuWin)) {
        core->up = core->dev.init(core->gpuWin.base);
    }
    gMaliCore = core;
    return core;
}

void gpuJobProbe(volatile uint32_t *g)
{
    const uint32_t l2 = r32(g, 0x120);
    const uint32_t tiler = r32(g, 0x110);
    const uint32_t shader = r32(g, 0x100);
    kprintf("d2m1 mp f l2=%08x sh=%08x tl=%08x\n", l2, shader, tiler);
    kprintf("d2m2 mp as=%08x js=%08x coh=%08x\n",
        r32(g, 0x18), r32(g, 0x1c), r32(g, 0x300));
    if (!l2 || !shader || !tiler || !r32(g, 0x18) || !r32(g, 0x1c)) {
        kprintf("d2m2 mp feat stop\n");
        return;
    }

    MaliDevice dev;
    if (!dev.init(g)) {
        kprintf("d2m5 mp dev init fail\n");
        return;
    }
    const int boA = dev.boAlloc(0x8000);
    const int boB = dev.boAlloc(0x9000);
    if (boA < 0 || boB < 0 || !dev.boMap(boA, 0x200000) ||
        !dev.boMap(boB, 0x208000)) {
        kprintf("d2m5 mp mem fail\n");
        return;
    }
    uint8_t *bufA = dev.boCpu(boA);
    uint8_t *bufB = dev.boCpu(boB);
    const uint64_t chainVa = dev.boGpu(boA);
    const uint64_t faultVa = chainVa;
    kprintf("d2m4 mp rdy=%08x/%08x/%08x\n",
        r32(g, 0x160), r32(g, 0x140), r32(g, 0x150));
    kprintf("d2f0 mp dev bo=%d/%d a=%x b=%x tab=%08x\n",
        boA, boB, (uint32_t)dev.boGpu(boA), (uint32_t)dev.boGpu(boB),
        (uint32_t)dev.as.tab.phys);
    const unsigned kJobs = 16;

    kprintf("d2a0 mp matrix start\n");
    struct ExpResult {
        uint32_t done, raw, jsst, z, z2, wtype, fence;
    };
    static ExpResult res[2];
    for (unsigned e = 0; e < 2; e++) {
        ExpResult &r = res[e];
        bzero(&r, sizeof(r));
        // e0: IMMEDIATE_64 chain through the device API. e1: ZERO chain.
        const uint32_t wtype = (e == 0) ? 7 : 3;
        r.wtype = wtype;
        for (unsigned i = 0; i < kJobs; i++) {
            uint8_t *jd = bufA + (size_t)i * 0x80;
            uint64_t next = (i + 1 < kJobs) ? chainVa + (uint64_t)(i + 1) * 0x80 : 0;
            uint64_t imm = 0x1000000000000000ULL | i;
            putWriteValueJob(jd, i + 1, 0, 0, next,
                chainVa + 0x1000 + (uint64_t)i * 0x1000, faultVa, wtype, imm);
        }
        for (unsigned i = 0; i < kJobs; i++) {
            uint8_t *page = (i < 7) ? bufA + (size_t)(i + 1) * 0x1000
                                    : bufB + (size_t)(i - 7) * 0x1000;
            *reinterpret_cast<uint64_t *>(page) = 0xdeadbeefcafef00dULL;
        }
        kprintf("d2a6 e=%u begin wt=%u\n", e + 1, wtype);

        uint32_t raw = 0, jsst = 0;
        bool done = dev.submit(chainVa, &raw, &jsst);
        for (unsigned i = 0; i < kJobs; i++) {
            const uint8_t *page = (i < 7) ? bufA + (size_t)(i + 1) * 0x1000
                                          : bufB + (size_t)(i - 7) * 0x1000;
            uint64_t want = (wtype == 7) ? (0x1000000000000000ULL | i) : 0;
            if (*reinterpret_cast<const uint64_t *>(page) == want) {
                r.z++;
            }
        }
        IOSleep(100);
        dev.flushAll();
        for (unsigned i = 0; i < kJobs; i++) {
            const uint8_t *page = (i < 7) ? bufA + (size_t)(i + 1) * 0x1000
                                          : bufB + (size_t)(i - 7) * 0x1000;
            uint64_t want = (wtype == 7) ? (0x1000000000000000ULL | i) : 0;
            if (*reinterpret_cast<const uint64_t *>(page) == want) {
                r.z2++;
            }
        }
        r.done = done;
        r.raw = raw;
        r.jsst = jsst;
        r.fence = dev.fence;
        kprintf("d2a1 e=%u done=%d raw=%x fence=%u\n", e + 1, done, raw, r.fence);
        kprintf("d2a2 e=%u st=%x z=%u/%u\n", e + 1, jsst, r.z, r.z2);
        if (!done) {
            kprintf("d2a5 e=%u g=%x/%08x/%08x\n", e + 1,
                r32(g, 0x3c), r32(g, 0x40), r32(g, 0x44));
        }
    }
    mmuDisable(g, 0);

    // Two compact passes so the result survives a UFS burst, then return so
    // the probe thread can publish the device service.
    for (unsigned pass = 0; pass < 2; pass++) {
        kprintf("d2b0 mp results\n");
        for (unsigned e = 0; e < 2; e++) {
            const ExpResult &r = res[e];
            kprintf("d2b%u W=%u F=%u D=%u R=%08x S=%02x Z=%u/%u\n", e + 1,
                r.wtype, r.fence, r.done, r.raw, r.jsst & 0xff, r.z, r.z2);
        }
        kprintf("d2bf mp results end\n");
        if (pass == 0) {
            IOSleep(4000);
        }
    }
}



void probe(void)
{
    Window top, infra, mfg, spm, gpu;
    if (!mapWindow(0x10000000, 0x1000, top) ||
        !mapWindow(0x10001000, 0x1000, infra) ||
        !mapWindow(0x13fbf000, 0x1000, mfg) ||
        !mapWindow(0x1c001000, 0x1000, spm) ||
        !mapWindow(0x13000000, 0x4000, gpu)) {
        kprintf("d171 mp map fail %d%d%d%d%d\n",
            top.base != nullptr, infra.base != nullptr, mfg.base != nullptr,
            spm.base != nullptr, gpu.base != nullptr);
        return;
    }

    kprintf("d172 mp pre top=%08x ref=%08x spm=%08x/%08x con=%08x,%08x,%08x,%08x\n",
        r32(top.base, 0x120), r32(top.base, 0xe0),
        r32(spm.base, 0xf4c), r32(spm.base, 0xf50),
        r32(spm.base, 0xeb8), r32(spm.base, 0xebc),
        r32(spm.base, 0xec0), r32(spm.base, 0xec4));
    kprintf("d173 mp infra bp=%08x,%08x,%08x,%08x st=%08x,%08x,%08x,%08x\n",
        r32(infra.base, 0xc44), r32(infra.base, 0xc54),
        r32(infra.base, 0xc64), r32(infra.base, 0xca4),
        r32(infra.base, 0xc4c), r32(infra.base, 0xc5c),
        r32(infra.base, 0xc6c), r32(infra.base, 0xcac));

    uint32_t status = (r32(spm.base, 0xf4c) >> 1) & 0x0f;
    if (status != 0x0f) {
        for (int i = 0; i < 4; i++) {
            if (i == 2) {
                // mfg_ref_sel gate is active-low in the set/clr pair; clear
                // register 0xe8 enables it and 0x8 carries the update bit.
                if (r32(top.base, 0xe0) & (1u << 23)) {
                    w32(top.base, 0xe8, 1u << 23);
                    w32(top.base, 0x8, 1u << 23);
                }
            }
            bool ok = powerDomain(spm.base, infra.base, i);
            kprintf("d174 mp pwr %s=%d st=%08x/%08x ctl=%08x\n",
                kDomains[i].name, ok,
                r32(spm.base, 0xf4c), r32(spm.base, 0xf50),
                r32(spm.base, kDomains[i].ctl));
        }

        status = (r32(spm.base, 0xf4c) >> 1) & 0x0f;
        if (status != 0x0f) {
            kprintf("d175 mp abort pwr=%x top=%08x ref=%08x\n",
                status, r32(top.base, 0x120), r32(top.base, 0xe0));
            if (native_boot_log_flush) native_boot_log_flush();
            return;
        }
    }

    // All four domains are up, so the INFRA_AO bus protection is open and the
    // MFG_CFG block is safe to touch. Probe both gate paths: the set register
    // 0x0 and the clear register 0x8, reading the status 0x4 after each, and
    // dump the low register block so the real gate/status bits are visible.
    kprintf("d176 mp cfg %08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
        r32(mfg.base, 0x00), r32(mfg.base, 0x04), r32(mfg.base, 0x08),
        r32(mfg.base, 0x0c), r32(mfg.base, 0x10), r32(mfg.base, 0x14),
        r32(mfg.base, 0x18), r32(mfg.base, 0x1c));
    // MTK clk-gate set/clr layout (mfg syscon): STA is 0x0 and bit 0 reads 0
    // when the bg3d gate is enabled; CLR 0x8 enables, SET 0x4 disables.
    const uint32_t g0 = r32(mfg.base, 0x0);
    w32(mfg.base, 0x8, 0x1);
    const uint32_t g1 = r32(mfg.base, 0x0);
    w32(mfg.base, 0x4, 0x1);
    const uint32_t g2 = r32(mfg.base, 0x0);
    w32(mfg.base, 0x8, 0x1);
    const uint32_t g3 = r32(mfg.base, 0x0);
    const uint32_t mux = r32(top.base, 0x120);
    const uint32_t ref = r32(top.base, 0xe0);
    kprintf("d177 mp cfg2 %08x,%08x,%08x g0=%08x g1=%08x g2=%08x g3=%08x\n",
        r32(mfg.base, 0x20), r32(mfg.base, 0x24), r32(mfg.base, 0x28),
        g0, g1, g2, g3);
    kprintf("d178 mp clk mux=%08x ref=%08x pwr=%x\n",
        mux, ref, (r32(spm.base, 0xf4c) >> 1) & 0x0f);
    if ((r32(spm.base, 0xf4c) >> 1 & 0x0f) != 0x0f || (mux & 0x10000) != 0 ||
        (r32(mfg.base, 0x0) & 0x1) != 0) {
        if (native_boot_log_flush) native_boot_log_flush();
        return;
    }

    mfgTopInit(mfg.base);
    kprintf("d179 mp gpu go mfg=%08x,%08x,%08x,%08x,%08x,%08x\n",
        r32(mfg.base, 0x20), r32(mfg.base, 0xb0), r32(mfg.base, 0xb4),
        r32(mfg.base, 0x24), r32(mfg.base, 0xf60), r32(mfg.base, 0x8a0));
    kprintf("d180 mp gpu id=%08x l2=%08x mem=%08x mmu=%08x js=%08x\n",
        r32(gpu.base, 0x000), r32(gpu.base, 0x004), r32(gpu.base, 0x010),
        r32(gpu.base, 0x014), r32(gpu.base, 0x01c));
    kprintf("d181 mp gpu sh=%08x/%08x st=%08x feats=%08x/%08x\n",
        r32(gpu.base, 0x100), r32(gpu.base, 0x104), r32(gpu.base, 0x034),
        r32(gpu.base, 0x060), r32(gpu.base, 0x064));
    gpuJobProbe(gpu.base);
    if (native_boot_log_flush) native_boot_log_flush();
}

void probeThread(void *, wait_result_t)
{
    IOSleep(3000);
    probe();
    native_mali_service_start();
    if (native_boot_log_flush) {
        native_boot_log_flush();
    }
}

} // namespace

extern "C" void native_mali_probe_start(void)
{
    uint32_t on = 0;
    if (!PE_parse_boot_argn("native-mali-probe", &on, sizeof(on)) || on != 1) {
        return;
    }
    static bool started;
    if (started) {
        return;
    }
    started = true;
    thread_t thread;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}

// ---------------------------------------------------------------------------
// NativeMali device service and user client. The wire ABI follows Sheng's
// NativeAdrenoABI shape: versioned fixed-size records copied through external
// methods (no ioctl, no user-supplied lengths). All GPU work goes through the
// same MaliDevice used by the probe. Record layouts live in NativeMaliABI.h
// so the PanVK libdrm shim and the kernel cannot drift apart.

class NativeMaliService : public IOService {
    OSDeclareDefaultStructors(NativeMaliService)
public:
    bool bringUp();
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(NativeMaliService, IOService)

bool NativeMaliService::bringUp()
{
    MaliCore *core = maliCoreGet();
    return core && core->up;
}

class NativeMaliUserClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeMaliUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch, OSObject *target,
        void *reference) APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
        IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;

    static IOReturn sQuery(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    static IOReturn sBoAlloc(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    static IOReturn sBoMap(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    static IOReturn sBoFree(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    static IOReturn sSubmit(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    static IOReturn sWaitFence(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    static IOReturn sQueryParam(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);

    MaliCore *core = nullptr;
};

OSDefineMetaClassAndStructors(NativeMaliUserClient, IOUserClient)

static NativeMaliUserClient *maliClientOf(OSObject *target)
{
    return OSDynamicCast(NativeMaliUserClient, target);
}

extern "C" IOReturn native_mali_new_user_client(IOService *provider,
    task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
    IOUserClient **handler);

IOReturn NativeMaliService::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
    return native_mali_new_user_client(this, owningTask, securityID, type,
        properties, handler);
}

extern "C" IOReturn native_mali_new_user_client(IOService *provider,
    task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
    IOUserClient **handler)
{
    if (!provider || !handler) {
        return kIOReturnBadArgument;
    }
    NativeMaliUserClient *client = new NativeMaliUserClient;
    if (!client) {
        return kIOReturnNoMemory;
    }
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnBadArgument;
    }
    if (!client->attach(provider)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(provider)) {
        client->detach(provider);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

bool NativeMaliUserClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
    return IOUserClient::initWithTask(owningTask, securityID, type, properties);
}

bool NativeMaliUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    core = maliCoreGet();
    return core != nullptr && core->up;
}

IOReturn NativeMaliUserClient::clientClose()
{
    core = nullptr;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::clientMemoryForType(UInt32 type,
    IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (options) {
        *options = 0;
    }
    if (!memory) {
        return kIOReturnBadArgument;
    }
    *memory = nullptr;
    if (!core || !core->up) {
        return kIOReturnNotReady;
    }
    const int h = static_cast<int>(type) - 1;
    if (h < 0 || h >= MaliDevice::kMaxBo || !core->dev.bo[h].used) {
        return kIOReturnBadArgument;
    }
    IOBufferMemoryDescriptor *desc = core->dev.bo[h].mem.desc;
    if (!desc) {
        return kIOReturnNotReady;
    }
    desc->retain();
    *memory = desc;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::sQuery(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    if (!client || !arguments || !arguments->structureOutput) {
        return kIOReturnBadArgument;
    }
    MaliAbiParams *out = reinterpret_cast<MaliAbiParams *>(arguments->structureOutput);
    out->abi_version = kMaliAbiVersion;
    out->input_bits = 48;
    out->output_bits = 40;
    out->flags = 0;
    out->max_bo_bytes = 256ULL << 20;
    out->va_base = MaliDevice::kVaBase;
    out->va_size = (uint64_t)MaliAs::kWindows * MaliAs::kWindowBytes;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::sBoAlloc(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    const MaliAbiBoAllocRequest *in =
        reinterpret_cast<const MaliAbiBoAllocRequest *>(arguments->structureInput);
    MaliAbiHandle *out = reinterpret_cast<MaliAbiHandle *>(arguments->structureOutput);
    if (!client || !client->core || !client->core->up) {
        return kIOReturnNotReady;
    }
    if (!in || !out || !in->bytes || (in->bytes & 0xfff) ||
        in->bytes > (256ULL << 20)) {
        return kIOReturnBadArgument;
    }
    const int handle = client->core->dev.boAlloc(static_cast<size_t>(in->bytes));
    if (handle < 0) {
        return kIOReturnNoMemory;
    }
    const MaliBo &bo = client->core->dev.bo[handle];
    out->handle = static_cast<uint64_t>(handle + 1);
    out->va = bo.va;
    out->mmap_offset = bo.va;
    out->size = bo.mem.len;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::sBoMap(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    const MaliAbiBoMapRequest *in =
        reinterpret_cast<const MaliAbiBoMapRequest *>(arguments->structureInput);
    if (!client || !client->core || !client->core->up || !in ||
        !in->handle) {
        return kIOReturnBadArgument;
    }
    if (in->iova < MaliDevice::kVaBase ||
        in->iova >= MaliDevice::kVaBase +
            (uint64_t)MaliAs::kWindows * MaliAs::kWindowBytes) {
        return kIOReturnBadArgument;
    }
    if (!client->core->dev.boMap(static_cast<int>(in->handle - 1), in->iova)) {
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::sBoFree(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    const MaliAbiHandle *in =
        reinterpret_cast<const MaliAbiHandle *>(arguments->structureInput);
    if (!client || !client->core || !in || !in->handle) {
        return kIOReturnBadArgument;
    }
    client->core->dev.boFree(static_cast<int>(in->handle - 1));
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::sSubmit(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    const MaliAbiSubmitRequest *in =
        reinterpret_cast<const MaliAbiSubmitRequest *>(arguments->structureInput);
    MaliAbiSubmitReply *out =
        reinterpret_cast<MaliAbiSubmitReply *>(arguments->structureOutput);
    if (!client || !client->core || !client->core->up) {
        return kIOReturnNotReady;
    }
    if (!in || !out ||
        in->head_va < MaliDevice::kVaBase ||
        in->head_va >= MaliDevice::kVaBase +
            (uint64_t)MaliAs::kWindows * MaliAs::kWindowBytes ||
        (in->head_va & 0x7f) || in->buffer_count > kMaliAbiMaxBuffers) {
        return kIOReturnBadArgument;
    }
    uint32_t raw = 0, jsst = 0;
    const bool done = client->core->dev.submit(in->head_va, &raw, &jsst);
    out->fence = client->core->dev.fence;
    out->raw = raw;
    out->jsst = jsst;
    out->af = r32(client->core->gpuWin.base, asReg(0, 0x1c));
    out->reserved = 0;
    return done ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn NativeMaliUserClient::sWaitFence(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    const MaliAbiWaitRequest *in =
        reinterpret_cast<const MaliAbiWaitRequest *>(arguments->structureInput);
    if (!client || !client->core || !in) {
        return kIOReturnNotReady;
    }
    uint32_t wait = in->timeout_ms ? in->timeout_ms : 1000;
    while (in->fence > client->core->dev.fence && wait) {
        IOSleep(1);
        wait--;
    }
    return (in->fence <= client->core->dev.fence) ? kIOReturnSuccess
                                                     : kIOReturnTimeout;
}

// Panfrost GET_PARAM values. The feature registers follow the mainline
// panfrost_regs.h layout; the kernel driver hands userspace the raw register
// and pan_props derives the device properties from it.
IOReturn NativeMaliUserClient::sQueryParam(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliUserClient *client = maliClientOf(target);
    const MaliAbiQueryParamRequest *in =
        reinterpret_cast<const MaliAbiQueryParamRequest *>(arguments->structureInput);
    MaliAbiQueryParamReply *out =
        reinterpret_cast<MaliAbiQueryParamReply *>(arguments->structureOutput);
    if (!client || !client->core || !client->core->up || !in || !out) {
        return kIOReturnNotReady;
    }
    volatile uint32_t *g = client->core->gpuWin.base;
    const uint32_t gpuId = r32(g, 0x00);
    uint64_t value = 0;
    if (in->param >= kMaliParamTextureFeatures0 &&
        in->param <= kMaliParamTextureFeatures3) {
        value = r32(g, 0xb0 + 4 * (in->param - kMaliParamTextureFeatures0));
    } else if (in->param >= kMaliParamJsFeatures0 &&
        in->param <= kMaliParamJsFeatures15) {
        value = r32(g, 0xc0 + 4 * (in->param - kMaliParamJsFeatures0));
    } else {
        switch (in->param) {
        case kMaliParamGpuProdId: value = gpuId >> 16; break;
        case kMaliParamGpuRevision: value = gpuId & 0xffff; break;
        case kMaliParamShaderPresent: value = r32(g, 0x100); break;
        case kMaliParamTilerPresent: value = r32(g, 0x110); break;
        case kMaliParamL2Present: value = r32(g, 0x120); break;
        case kMaliParamStackPresent: value = r32(g, 0xe00); break;
        case kMaliParamAsPresent: value = r32(g, 0x18); break;
        case kMaliParamJsPresent: value = r32(g, 0x1c); break;
        case kMaliParamL2Features: value = r32(g, 0x04); break;
        case kMaliParamCoreFeatures: value = r32(g, 0x08); break;
        case kMaliParamTilerFeatures: value = r32(g, 0x0c); break;
        case kMaliParamMemFeatures: value = r32(g, 0x10); break;
        case kMaliParamMmuFeatures: value = r32(g, 0x14); break;
        case kMaliParamThreadFeatures: value = r32(g, 0xac); break;
        case kMaliParamMaxThreads: value = r32(g, 0xa0); break;
        case kMaliParamThreadMaxWorkgroupSz: value = r32(g, 0xa4); break;
        case kMaliParamThreadMaxBarrierSz: value = r32(g, 0xa8); break;
        case kMaliParamAfbcFeatures: value = r32(g, 0x4c); break;
        case kMaliParamThreadTlsAlloc: value = 0; break;
        case kMaliParamNrCoreGroups: value = 1; break;
        case kMaliParamCoherencyFeatures: value = 0; break;
        case kMaliParamSystemTimestamp: value = 0; break;
        case kMaliParamSystemTimestampFrequency: value = 0; break;
        case kMaliParamAllowedJmCtxPriorities: value = 0x7; break;
        case kMaliParamSelectedCoherency: value = 31; break;
        default: return kIOReturnBadArgument;
        }
    }
    out->value = value;
    return kIOReturnSuccess;
}

static const IOExternalMethodDispatch kMaliMethods[] = {
    { NativeMaliUserClient::sQuery, 0, sizeof(MaliAbiParams), 0, sizeof(MaliAbiParams) },
    { NativeMaliUserClient::sBoAlloc, 0, sizeof(MaliAbiBoAllocRequest), 0, sizeof(MaliAbiHandle) },
    { NativeMaliUserClient::sBoMap, 0, sizeof(MaliAbiBoMapRequest), 0, 0 },
    { NativeMaliUserClient::sBoFree, 0, sizeof(MaliAbiHandle), 0, 0 },
    { NativeMaliUserClient::sSubmit, 0, sizeof(MaliAbiSubmitRequest), 0, sizeof(MaliAbiSubmitReply) },
    { NativeMaliUserClient::sWaitFence, 0, sizeof(MaliAbiWaitRequest), 0, 0 },
    { NativeMaliUserClient::sQueryParam, 0, sizeof(MaliAbiQueryParamRequest), 0, sizeof(MaliAbiQueryParamReply) },
};

IOReturn NativeMaliUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *arguments, IOExternalMethodDispatch *, OSObject *target,
    void *)
{
    if (selector < kMaliAbiMethodQuery || selector > kMaliAbiMethodQueryParam) {
        return kIOReturnUnsupported;
    }
    return IOUserClient::externalMethod(selector, arguments,
        const_cast<IOExternalMethodDispatch *>(&kMaliMethods[selector - 1]),
        target, nullptr);
}

// The accelerator carries the original class name so the system Metal
// discovery finds it, and names the plugin that will implement Metal.
class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator)
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES)

IOReturn AGXAccelerator::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
    return native_mali_new_user_client(this, owningTask, securityID, type,
        properties, handler);
}

void native_mali_service_start(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    NativeMaliService *svc = new NativeMaliService;
    if (!svc) {
        kprintf("d2sv0 mp alloc fail\n");
        return;
    }
    if (!svc->init()) {
        kprintf("d2sv0 mp init fail\n");
        svc->release();
        return;
    }
    svc->setName("NativeMaliGPU");
    const bool up = svc->bringUp();
    kprintf("d2sv1 mp dev=%d\n", up ? 1 : 0);
    // Publication follows the working display pattern: attach to a provider
    // and start; IOService::start registers the service in the plane.
    IOService *home = IOService::getResourceService();
    bool attached = home && svc->attach(home);
    bool started = attached && svc->start(home);
    if (!started) {
        svc->registerService(kIOServiceSynchronous);
    }
    kprintf("d2sv2 mp a=%d s=%d reg=%d id=%llx\n", attached ? 1 : 0,
        started ? 1 : 0, svc->inPlane(gIOServicePlane) ? 1 : 0,
        svc->getRegistryEntryID());

    AGXAccelerator *acc = new AGXAccelerator;
    if (acc && acc->init()) {
        acc->setName("AGXAccelerator");
        acc->setProperty("MetalPluginName", "NativeMaliMetal");
        bool accAttached = home && acc->attach(home);
        bool accStarted = accAttached && acc->start(home);
        kprintf("d2sv3 mp acc a=%d s=%d reg=%d id=%llx\n", accAttached ? 1 : 0,
            accStarted ? 1 : 0, acc->inPlane(gIOServicePlane) ? 1 : 0,
            acc->getRegistryEntryID());
    } else {
        kprintf("d2sv3 mp acc alloc fail\n");
    }
}
