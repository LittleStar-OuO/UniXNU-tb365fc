// SPDX-License-Identifier: BSD-2-Clause-Patent
// MTK GPIO bit-bang transport for the Novatek NT36532 touch controller on
// spi3 pins (CLK 68, CS 69, MO 70, MI 71, reset 43). The register layout is
// the mainline "paris" pinctrl family: MODE at 0x300 + (pin/8)*0x10 with a
// 4-bit field per pin, DIR/DO/DI at 0x000/0x100/0x200 with one bit per pin in
// 32-pin banks. Only the identify stage of the official protocol runs here.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
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

    bool open() {
        desc = IOMemoryDescriptor::withPhysicalAddress(0x10005000ULL, 0x1000, kIODirectionNone);
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

extern "C" void native_touch_spi_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("touch", &stage, sizeof(stage)) || stage != 3) {
        return;
    }
    Gpio g;
    if (!g.open()) {
        kprintf("tsp no gpio map\n");
        return;
    }
    g.setMode(kClk, 0);
    g.setMode(kCs, 0);
    g.setMode(kMo, 0);
    g.setMode(kMi, 0);
    g.setMode(kRst, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 1);
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
