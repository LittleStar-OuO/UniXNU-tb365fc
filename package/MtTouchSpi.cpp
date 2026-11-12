// SPDX-License-Identifier: BSD-2-Clause-Patent
// MTK GPIO bit-bang transport for the Novatek NT36532 touch controller on
// spi3 pins (CLK 68, CS 69, MO 70, MI 71, reset 43). The register layout is
// the mainline "paris" pinctrl family: MODE at 0x300 + (pin/8)*0x10 with a
// 4-bit field per pin, DIR/DO/DI at 0x000/0x100/0x200 with one bit per pin in
// 32-pin banks. Only the identify stage of the official protocol runs here.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "nt36532/NvtProtocol.h"
}

namespace {

constexpr unsigned kClk = 68, kCs = 69, kMo = 70, kMi = 71, kRst = 43;

struct Gpio {
    IOMemoryDescriptor *desc;
    IOMemoryMap *map;
    volatile uint32_t *base;

    bool openAt(uint64_t pa) {
        desc = IOMemoryDescriptor::withPhysicalAddress(pa, 0x1000, kIODirectionNone);
        if (!desc) {
            return false;
        }
        map = desc->map(kIOMapAnywhere);
        if (!map) {
            return false;
        }
        base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
        return base != nullptr;
    }
    bool open() { return openAt(0x10005000ULL); }
    void close() {
        if (map) {
            map->release();
            map = nullptr;
        }
        if (desc) {
            desc->release();
            desc = nullptr;
        }
    }
    uint32_t rd(unsigned off) { return base[off / 4]; }
    void wr(unsigned off, uint32_t v) { base[off / 4] = v; }
    void rmw(unsigned off, uint32_t mask, uint32_t set) {
        uint32_t v = rd(off);
        v = (v & ~mask) | (set & mask);
        wr(off, v);
    }
    void setMode(unsigned pin, unsigned func) {
        unsigned bits = pin * 4;
        unsigned reg = 0x300 + 0x10 * (bits / 32);
        unsigned shift = bits % 32;
        rmw(reg, 0xFu << shift, (func & 0xFu) << shift);
    }
    void setDirOut(unsigned pin) { rmw(0x000 + 0x10 * (pin / 32), 1u << (pin % 32), 1u << (pin % 32)); }
    void setDirIn(unsigned pin) { rmw(0x000 + 0x10 * (pin / 32), 1u << (pin % 32), 0); }
    void setOut(unsigned pin, unsigned value) {
        rmw(0x100 + 0x10 * (pin / 32), 1u << (pin % 32), value ? (1u << (pin % 32)) : 0);
    }
    unsigned getIn(unsigned pin) { return (rd(0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u; }
};

void spinHalfClock(void) {
    uint64_t freq = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
    uint64_t ticks = freq / 2000000u;   // ~500 kHz
    if (!ticks) {
        ticks = 1;
    }
    do {
        __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < ticks);
}

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Gpio *g = static_cast<Gpio *>(ctx);
    if (!tx) {
        return -1;
    }
    g->setOut(kCs, 0);
    for (size_t i = 0; i < len; i++) {
        uint8_t in = 0;
        for (int bit = 7; bit >= 0; bit--) {
            g->setOut(kMo, (tx[i] >> bit) & 1u);
            spinHalfClock();
            g->setOut(kClk, 1);
            spinHalfClock();
            in = (uint8_t)((in << 1) | g->getIn(kMi));
            g->setOut(kClk, 0);
        }
        if (rx) {
            rx[i] = in;
        }
    }
    g->setOut(kCs, 1);
    return 0;
}

void busDelay(void *, unsigned ms) { IOSleep(ms); }

void hardwareReset(Gpio *g) {
    g->setOut(kRst, 0);
    IOSleep(10);
    g->setOut(kRst, 1);
    IOSleep(30);
}

}

static void runIdentify(void) {
    Gpio g;
    if (!g.open()) {
        kprintf("tsp no gpio map\n");
        return;
    }
    {
        unsigned mBefore = g.rd(0x380);
        g.wr(0x380, 0x00000000u);
        unsigned mZero = g.rd(0x380);
        g.wr(0x380, 0x5a5a5a5au);
        unsigned mPat = g.rd(0x380);
        g.wr(0x380, mBefore);
        kprintf("tsp mode probe before=%x zero=%x pat=%x\n", mBefore, mZero, mPat);
        kprintf("tsp scan300 %x %x %x %x %x %x %x %x\n",
            g.rd(0x300), g.rd(0x310), g.rd(0x320), g.rd(0x330),
            g.rd(0x340), g.rd(0x350), g.rd(0x360), g.rd(0x370));
        kprintf("tsp scan380 %x %x %x %x %x %x %x %x\n",
            g.rd(0x380), g.rd(0x390), g.rd(0x3a0), g.rd(0x3b0),
            g.rd(0x3c0), g.rd(0x3d0), g.rd(0x3e0), g.rd(0x3f0));
    }
    g.setMode(kClk, 0);
    unsigned mRmw = g.rd(0x380);
    g.wr(0x380, 0x00000111u);
    unsigned mWr = g.rd(0x380);
    kprintf("tsp mode set rmw=%x wr=%x\n", mRmw, mWr);
    g.wr(0x380, 0x00000000u);
    {
        unsigned w = g.rd(0x350);
        w &= ~(0xFu << 12);
        g.wr(0x350, w);
    }
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 1);
    // The MTK iocfg banks hold the input-enable and schmitt fields; without
    // them a GPIO input reads 0 forever. The pin-to-bank table is vendor data
    // we do not have yet, so enable every input path once (inputs are read
    // only; this cannot disturb other peripherals' outputs).
    {
        static const uint64_t banks[] = {
            0x11b10000ULL, 0x11c40000ULL, 0x11d10000ULL, 0x11d30000ULL,
            0x11d40000ULL, 0x11e30000ULL, 0x11eb0000ULL, 0x11ec0000ULL,
        };
        for (unsigned i = 0; i < sizeof(banks) / sizeof(banks[0]); i++) {
            Gpio b;
            if (!b.openAt(banks[i])) {
                continue;
            }
            b.wr(0x000, 0xffffffffu);
            b.wr(0x010, 0xffffffffu);
            b.close();
        }
    }
    g.setOut(kMo, 1);
    busDelay(nullptr, 1);
    unsigned miHigh = g.getIn(kMi);
    g.setOut(kMo, 0);
    busDelay(nullptr, 1);
    unsigned miLow = g.getIn(kMi);
    kprintf("tsp io mode380=%x m384=%x dir020=%x do120=%x di220=%x mi1=%u mi0=%u\n",
        g.rd(0x380), g.rd(0x384), g.rd(0x020), g.rd(0x120), g.rd(0x220), miHigh, miLow);
    kprintf("tsp gpio mode300=%x m304=%x dir=%x do=%x di=%x mi=%u\n",
        g.rd(0x300), g.rd(0x304), g.rd(0x000), g.rd(0x100), g.rd(0x200), g.getIn(kMi));

    uint8_t *scratch = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 16));
    uint8_t *rxbuf = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 16));
    if (!scratch || !rxbuf) {
        kprintf("tsp no buffers\n");
        if (scratch) { IOFree(scratch, NVT_READ_MAX + 16); }
        if (rxbuf) { IOFree(rxbuf, NVT_READ_MAX + 16); }
        g.close();
        return;
    }
    hardwareReset(&g);
    struct nvt_bus bus = { &g, busXfer, busDelay };
    struct nvt_chip chip = {};
    chip.bus = &bus;
    chip.scratch = scratch;
    chip.rxbuf = rxbuf;
    int r = nvt_identify(&chip);
    kprintf("tsp identify r=%d id=%02x %02x %02x %02x %02x %02x cas=%d map=%s\n", r,
        chip.id[0], chip.id[1], chip.id[2], chip.id[3], chip.id[4], chip.id[5],
        chip.cascade, chip.map ? chip.map->name : "-");
    kprintf("tsp gpio after di=%x mi=%u\n", g.rd(0x200), g.getIn(kMi));
    IOFree(scratch, NVT_READ_MAX + 16);
    IOFree(rxbuf, NVT_READ_MAX + 16);
    g.close();
}

static void lateIdentify(void *arg, wait_result_t) {
    unsigned seconds = (unsigned)(uintptr_t)arg;
    IOSleep(seconds * 1000);
    kprintf("tsp late +%us\n", seconds);
    runIdentify();
}

extern "C" void native_touch_spi_probe(void) {
    unsigned stage = 0;
    int found = PE_parse_boot_argn("touch", &stage, sizeof(stage));
    kprintf("tsp enter found=%d stage=%u\n", found, stage);
    if (!found || stage != 3) {
        return;
    }
    runIdentify();
    thread_t t1 = nullptr;
    if (kernel_thread_start(lateIdentify, (void *)(uintptr_t)360, &t1) == KERN_SUCCESS) {
        thread_deallocate(t1);
    }
    thread_t t2 = nullptr;
    if (kernel_thread_start(lateIdentify, (void *)(uintptr_t)420, &t2) == KERN_SUCCESS) {
        thread_deallocate(t2);
    }
}
