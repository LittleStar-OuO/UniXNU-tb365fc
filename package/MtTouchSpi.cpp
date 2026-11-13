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

static unsigned gHalfClockScale = 1;

void spinHalfClock(void) {
    uint64_t freq = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
    uint64_t ticks = (freq / 2000000u) * gHalfClockScale;   // ~500 kHz / scale
    if (!ticks) {
        ticks = 1;
    }
    unsigned guard = 0;
    do {
        __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < ticks && ++guard < 4000000u);
}

unsigned clockTicks(void) {
    uint64_t freq = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (unsigned)(freq / 2000000u);
}

static unsigned gCpol, gCpha;   // SPI mode under test
static unsigned gMiOnes;        // MISO samples seen high, i.e. wire activity
static unsigned gXferN;         // transfers issued in the current attempt
static bool gTrace;             // print the first transfers of the attempt
static bool gHoldCs;            // keep CS asserted across the whole attempt
static unsigned gRstMode;       // 0 pulse low-high, 1 held low, 2 pulse high-low, 3 slow release
static unsigned gPreWake;       // 0 none, 1 0xff flood with CS held, 2 INT low pulse

// Last sweep results, reprinted periodically so they stay on the overlay
// canvas between bursts of HID console spam.
static int gSweepR[8] = { -99, -99, -99, -99, -99, -99, -99, -99 };
static unsigned gSweepOnes[8];
static uint8_t gStatusId[6];
static unsigned gPadIdle[2];     // MISO and INT levels with the bus idle
static unsigned gWatch[2];       // INT and MISO samples high in the 0.4 s watch
static unsigned gPu[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };   // MISO with each bank pulled up
static unsigned gHx[2];          // Himax-style read scan: addresses that moved MISO, per phase
static uint8_t gIc[2][4];        // register 0x10000000 (IC id) per phase
static uint8_t gTg[2][4];        // register 0x900000F0 (trigger)
static uint8_t gRl[2][4];        // register 0x10007000 (reload status)
static uint8_t gFw[2][4];        // register 0x10007004 (fw version area)
static unsigned gWn;             // reload polls
static uint8_t gWf[4], gWl[4];   // first and last 0x10007000 sample of the poll
static uint8_t gBl[4][4];        // bootloader probes: 0x80000010, 0x80000024, 0x8000002C, 0x90000018
static uint8_t gFl[16];          // flash bytes 0..15 read through the plain AHB window
static uint8_t gF2[16];          // flash bytes 0..15 read through the SPI200 controller
static unsigned gRn;             // reload polls after the system reset
static uint8_t gRv[4];           // last 0x10007000 sample
static uint8_t gRs[4];           // 0x10007004 after the reload attempt
static uint8_t gRf[4];           // 0x10007F00 (flash reload enable) before
static uint8_t gR2[4];           // last 0x10007010 sample (second reload reg)

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Gpio *g = static_cast<Gpio *>(ctx);
    if (!tx) {
        return -1;
    }
    if (gTrace && (gXferN < 12 || (gXferN % 8) == 0) && gXferN < 64) {
        kprintf("tsp x%u l%u\n", gXferN, (unsigned)len);
    }
    gXferN++;
    const unsigned idle = gCpol ? 1u : 0u;
    g->setOut(kClk, idle);
    if (!gHoldCs) {
        g->setOut(kCs, 0);
    }
    spinHalfClock();
    for (size_t i = 0; i < len; i++) {
        uint8_t in = 0;
        for (int bit = 7; bit >= 0; bit--) {
            g->setOut(kMo, (tx[i] >> bit) & 1u);
            spinHalfClock();
            g->setOut(kClk, idle ^ 1u);
            spinHalfClock();
            if (!gCpha) {
                unsigned v = g->getIn(kMi);
                gMiOnes += v;
                in = (uint8_t)((in << 1) | v);
            }
            g->setOut(kClk, idle);
            spinHalfClock();
            if (gCpha) {
                unsigned v = g->getIn(kMi);
                gMiOnes += v;
                in = (uint8_t)((in << 1) | v);
            }
        }
        if (rx) {
            rx[i] = in;
        }
    }
    spinHalfClock();
    if (!gHoldCs) {
        g->setOut(kCs, 1);
    }
    return 0;
}

void busDelay(void *, unsigned ms) {
    if (ms <= 50) {   // protocol waits stay off the scheduler
        uint64_t freq = 0, start = 0, now = 0;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
        uint64_t ticks = (freq ? freq : 24000000u) * ms / 1000u + 1;
        do {
            __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
        } while (now - start < ticks);
    } else {
        IOSleep(ms);
    }
}

void hardwareReset(Gpio *g) {
    g->setOut(kRst, 0);
    IOSleep(10);
    g->setOut(kRst, 1);
    IOSleep(30);
}

unsigned pinWordBits(Gpio *g, unsigned pin) {
    return (g->rd(0x100 + 0x10 * (pin / 32)) >> (pin % 32)) & 1u;
}

void pinMuxGpioWord(Gpio *g, unsigned pin) {
    unsigned reg = 0x300 + 0x10 * (pin / 8);
    unsigned shift = (pin % 8) * 4;
    unsigned v = g->rd(reg);
    v &= ~(0xFu << shift);
    g->wr(reg, v);
}

// The NT36532 is the TDDI: the touch core is powered with the panel. The
// stock panel node lists pm-enable 34 and bias 124/151; the display is already
// running under the firmware framebuffer, so only those supplies are touched.
// Reset (86) and backlight (21/25) stay untouched to keep the visible output.
void panelPowerOn(Gpio *g) {
    const unsigned pins[] = { 34, 124, 151 };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        pinMuxGpioWord(g, pins[i]);
        g->setDirOut(pins[i]);
        g->setOut(pins[i], 1);
    }
    busDelay(nullptr, 30);
}

}

// Everything proven safe: supplies, pin mux, input enables and MI sampling.
// No bus traffic happens here; the identify attempts run later from a thread.
static void runPrep(void) {
    Gpio g;
    if (!g.open()) {
        kprintf("tsp no gpio map\n");
        return;
    }
    kprintf("tsp clk=%u\n", clockTicks());
    kprintf("tsp pp do34=%u do21=%u do25=%u do86=%u do124=%u do151=%u\n",
        pinWordBits(&g, 34), pinWordBits(&g, 21), pinWordBits(&g, 25),
        pinWordBits(&g, 86), pinWordBits(&g, 124), pinWordBits(&g, 151));
    panelPowerOn(&g);
    kprintf("tsp pp on do34=%u do124=%u do151=%u\n",
        pinWordBits(&g, 34), pinWordBits(&g, 124), pinWordBits(&g, 151));
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
    g.close();
}

static void runPadChecks(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    kprintf("tsp pd mi go\n");
    g.setDirOut(kMi);
    g.setOut(kMi, 1);
    unsigned miOut1 = g.getIn(kMi);
    g.setOut(kMi, 0);
    unsigned miOut0 = g.getIn(kMi);
    g.setDirIn(kMi);
    g.setOut(kMi, 1);
    kprintf("tsp pd rst go\n");
    unsigned rstHi = (g.rd(0x210) >> 11) & 1u;
    g.setOut(kRst, 0);
    unsigned rstLo = (g.rd(0x210) >> 11) & 1u;
    g.setOut(kRst, 1);
    kprintf("TSP pad mi=%u%u rst=%u%u\n", miOut1, miOut0, rstHi, rstLo);
    gPadIdle[0] = g.getIn(kMi);
    gPadIdle[1] = (g.rd(0x200) >> 4) & 1u;
    kprintf("TSP idle mi=%u int=%u cs=%u\n", gPadIdle[0], gPadIdle[1], g.getIn(kCs));
    g.close();
}

// Sample the IRQ line and MISO for about 0.4 s after a reset: a live bootloader
// often pulses its interrupt or drives MISO.
static void runWatch(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    hardwareReset(&g);
    unsigned intOnes = 0, miOnes = 0;
    for (int i = 0; i < 400; i++) {
        intOnes += (g.rd(0x200) >> 4) & 1u;
        miOnes += g.getIn(kMi);
        busDelay(nullptr, 1);
    }
    gWatch[0] = intOnes;
    gWatch[1] = miOnes;
    kprintf("TSP watch int=%u mi=%u\n", intOnes, miOnes);
    g.close();
}

// Pull bit 7 of every iocfg bank up in turn: MISO reads high on the bank that
// owns pin 71 only if the line floats (nothing drives it).
static void runPuProbe(void) {
    static const uint64_t banks[] = {
        0x11b10000ULL, 0x11c40000ULL, 0x11d10000ULL, 0x11d30000ULL,
        0x11d40000ULL, 0x11e30000ULL, 0x11eb0000ULL, 0x11ec0000ULL,
    };
    Gpio mg;
    if (!mg.open()) {
        return;
    }
    kprintf("TSP pu");
    for (unsigned i = 0; i < sizeof(banks) / sizeof(banks[0]); i++) {
        Gpio b;
        if (!b.openAt(banks[i])) {
            kprintf(" -");
            continue;
        }
        uint32_t sel = b.rd(0x040), en = b.rd(0x060);
        b.wr(0x040, sel | (1u << 7));
        b.wr(0x060, en | (1u << 7));
        busDelay(nullptr, 1);
        unsigned v = mg.getIn(kMi);
        gPu[i] = v;
        kprintf(" %u", v);
        b.wr(0x040, sel);
        b.wr(0x060, en);
        b.close();
    }
    kprintf("\n");
    mg.close();
}

// The DTBO carries two panel SKUs sharing these pins: tm,nt36532 (Novatek) and
// boe,hx83122b (Himax). The Himax bootloader answers the 0xF3 read command, so
// a MISO activity scan tells which controller is populated.
static void runHimaxProbe(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    g.setMode(kClk, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 0);
    busDelay(nullptr, 10);
    g.setOut(kRst, 1);
    busDelay(nullptr, 50);
    uint8_t tx[6];
    uint8_t rx[8];
    for (unsigned m = 0; m < 2; m++) {
        gCpol = 0;
        gCpha = m;
        gHalfClockScale = 1;
        gMiOnes = 0;
        gXferN = 0;
        unsigned hits = 0;
        for (unsigned a = 0; a < 256; a++) {
            tx[0] = 0xF3;
            tx[1] = (uint8_t)a;
            tx[2] = 0x00;
            unsigned before = gMiOnes;
            if (busXfer(&g, tx, rx, 3)) {
                break;
            }
            if (gMiOnes != before) {
                hits++;
            }
        }
        gHx[m] = hits;
        kprintf("TSP hx%u h=%u o=%u\n", m, hits, gMiOnes);
    }
    gCpha = 0;
    g.close();
}

// HX83122 register access over the pipe interface decoded from the vendor
// hx83122_register_read/write: 0x13 <- 0x31 (burst), 0x0D <- auto-increment
// flag, 0x00 <- 4-byte AHB address, 0x0C <- direction, 0x08 <- data.
static void hxCmd(Gpio *g, unsigned pipe, uint8_t v) {
    uint8_t tx[3] = { 0xF2, (uint8_t)pipe, v };
    busXfer(g, tx, nullptr, 3);
}
static void hxAddr(Gpio *g, uint32_t addr) {
    uint8_t tx[6] = { 0xF2, 0x00, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24) };
    busXfer(g, tx, nullptr, 6);
}
static void hxRegRead(Gpio *g, uint32_t addr, uint8_t *out, unsigned len) {
    if (len > 8) {
        len = 8;
    }
    hxCmd(g, 0x13, 0x31);
    hxCmd(g, 0x0D, (addr != 0x8000002Cu && len > 4) ? 0x11 : 0x10);
    hxAddr(g, addr);
    hxCmd(g, 0x0C, 0x00);
    uint8_t tx[16];
    uint8_t rx[16];
    tx[0] = 0xF3;
    tx[1] = 0x08;
    for (unsigned i = 2; i < len + 3; i++) {
        tx[i] = 0;
    }
    busXfer(g, tx, rx, len + 3);
    for (unsigned i = 0; i < len; i++) {
        out[i] = rx[3 + i];
    }
}

static void hxRegReadN(Gpio *g, uint32_t addr, uint8_t *out, unsigned len) {
    if (len > 128) {
        len = 128;
    }
    hxCmd(g, 0x13, 0x31);
    hxCmd(g, 0x0D, (addr != 0x8000002Cu && len > 4) ? 0x11 : 0x10);
    hxAddr(g, addr);
    hxCmd(g, 0x0C, 0x00);
    static uint8_t tx[160];
    static uint8_t rx[160];
    tx[0] = 0xF3;
    tx[1] = 0x08;
    for (unsigned i = 2; i < len + 3; i++) {
        tx[i] = 0;
    }
    busXfer(g, tx, rx, len + 3);
    for (unsigned i = 0; i < len; i++) {
        out[i] = rx[3 + i];
    }
}

static void hxRegWrite(Gpio *g, uint32_t addr, const uint8_t *data, unsigned len) {
    if (len > 16) {
        len = 16;
    }
    hxCmd(g, 0x13, 0x31);
    hxCmd(g, 0x0D, (addr != 0x8000002Cu && len > 4) ? 0x11 : 0x10);
    uint8_t tx[24];
    tx[0] = 0xF2;
    tx[1] = 0x00;
    tx[2] = (uint8_t)addr;
    tx[3] = (uint8_t)(addr >> 8);
    tx[4] = (uint8_t)(addr >> 16);
    tx[5] = (uint8_t)(addr >> 24);
    for (unsigned i = 0; i < len; i++) {
        tx[6 + i] = data[i];
    }
    busXfer(g, tx, nullptr, 6 + len);
}

// The vendor's IC check reads the register space through the direct 8-bit
// pipe form: tx = [0xF3, pipe, dummy, ...], data from rx[3]. Pipe 0xD0 holds
// the IC id (expect xx 2A/2B 12 83), pipe 0xF0 the trigger word.
static void runReload(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    g.setMode(kClk, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 0);
    busDelay(nullptr, 10);
    g.setOut(kRst, 1);
    busDelay(nullptr, 200);
    gCpol = 0;
    gCpha = 0;
    gMiOnes = 0;
    gXferN = 0;
    gTrace = false;
    uint8_t tx[8];
    uint8_t rx[8];
    // interface_on: enable continuous burst and address auto-increment, then
    // verify by reading both control pipes back (vendor expects 31 and 10).
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    tx[0] = 0xF3;
    tx[1] = 0x13;
    for (int i = 2; i < 7; i++) {
        tx[i] = 0;
    }
    busXfer(&g, tx, rx, 7);
    gRf[0] = rx[3];
    gRf[1] = rx[4];
    tx[1] = 0x0D;
    busXfer(&g, tx, rx, 7);
    gRf[2] = rx[3];
    gRf[3] = rx[4];
    tx[1] = 0xD0;
    for (int i = 2; i < 7; i++) {
        tx[i] = 0;
    }
    busXfer(&g, tx, rx, 7);
    for (int i = 0; i < 4; i++) {
        gRv[i] = rx[3 + i];
    }
    tx[1] = 0xF0;
    busXfer(&g, tx, rx, 7);
    for (int i = 0; i < 4; i++) {
        gR2[i] = rx[3 + i];
    }
    tx[1] = 0xD2;
    busXfer(&g, tx, rx, 7);
    for (int i = 0; i < 4; i++) {
        gRs[i] = rx[3 + i];
    }
    gRn = 1;
    kprintf("TSP pd %02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x\n",
        gRv[0], gRv[1], gRv[2], gRv[3], gR2[0], gR2[1], gR2[2], gR2[3],
        gRs[0], gRs[1], gRs[2], gRs[3], gRf[0], gRf[1], gRf[2], gRf[3]);
    g.close();
}

// Validate the register write/read path by reading the start of the touch
// firmware flash: the factory image begins 00 00 00 00 00 00 00 00 00 04 ..
static void runFlashProbe(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    g.setMode(kClk, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 0);
    busDelay(nullptr, 10);
    g.setOut(kRst, 1);
    busDelay(nullptr, 200);
    gCpol = 0;
    gCpha = 0;
    gMiOnes = 0;
    gXferN = 0;
    gTrace = false;
    hxRegReadN(&g, 0x00000000u, gFl, 16);
    kprintf("TSP fl %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        gFl[0], gFl[1], gFl[2], gFl[3], gFl[4], gFl[5], gFl[6], gFl[7],
        gFl[8], gFl[9], gFl[10], gFl[11], gFl[12], gFl[13], gFl[14], gFl[15]);
    {
        uint8_t v[4];
        v[0] = 0x80; v[1] = 0x07; v[2] = 0x02; v[3] = 0x00;   // 0x00020780 transfer format
        hxRegWrite(&g, 0x80000010u, v, 4);
        v[0] = 0xFF; v[1] = 0x02; v[2] = 0x40; v[3] = 0x69;   // 0x694002FF page read control
        hxRegWrite(&g, 0x80000020u, v, 4);
        v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 0;               // flash address 0
        hxRegWrite(&g, 0x80000028u, v, 4);
        v[0] = 0x3B; v[1] = 0; v[2] = 0; v[3] = 0;            // read command
        hxRegWrite(&g, 0x80000024u, v, 4);
        busDelay(nullptr, 5);
    }
    hxRegReadN(&g, 0x8000002Cu, gF2, 16);
    kprintf("TSP f2 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        gF2[0], gF2[1], gF2[2], gF2[3], gF2[4], gF2[5], gF2[6], gF2[7],
        gF2[8], gF2[9], gF2[10], gF2[11], gF2[12], gF2[13], gF2[14], gF2[15]);
    g.close();
}

static void runHimaxId(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    g.setMode(kClk, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 0);
    busDelay(nullptr, 10);
    g.setOut(kRst, 1);
    busDelay(nullptr, 50);
    // The chip re-loads its firmware from flash after a reset; the vendor waits
    // for the reload register to settle. Poll it before reading anything else.
    gCpol = 0;
    gCpha = 0;
    gMiOnes = 0;
    gXferN = 0;
    gTrace = false;
    gWn = 0;
    for (unsigned i = 0; i < 30; i++) {
        uint8_t v[4];
        hxRegRead(&g, 0x10007000u, v, 4);
        if (i == 0) {
            for (int k = 0; k < 4; k++) {
                gWf[k] = v[k];
            }
        }
        for (int k = 0; k < 4; k++) {
            gWl[k] = v[k];
        }
        gWn = i + 1;
        if (v[1] == 0x3A && v[0] == 0xA3) {   // vendor: reload OK, 0xA33A
            break;
        }
        busDelay(nullptr, 100);
    }
    for (unsigned m = 0; m < 2; m++) {
        gCpol = 0;
        gCpha = m;
        gMiOnes = 0;
        gXferN = 0;
        gTrace = false;
        hxRegRead(&g, 0x10000000u, gIc[m], 4);
        kprintf("TSP ic%u=%02x%02x%02x%02x o=%u\n", m, gIc[m][0], gIc[m][1], gIc[m][2], gIc[m][3], gMiOnes);
        hxRegRead(&g, 0x900000F0u, gTg[m], 4);
        kprintf("TSP tr%u=%02x%02x%02x%02x\n", m, gTg[m][0], gTg[m][1], gTg[m][2], gTg[m][3]);
        hxRegRead(&g, 0x10007000u, gRl[m], 4);
        kprintf("TSP rl%u=%02x%02x%02x%02x\n", m, gRl[m][0], gRl[m][1], gRl[m][2], gRl[m][3]);
        hxRegRead(&g, 0x10007004u, gFw[m], 4);
        kprintf("TSP fw%u=%02x%02x%02x%02x\n", m, gFw[m][0], gFw[m][1], gFw[m][2], gFw[m][3]);
        if (m == 0) {
            hxRegRead(&g, 0x80000010u, gBl[0], 4);
            hxRegRead(&g, 0x80000024u, gBl[1], 4);
            hxRegRead(&g, 0x8000002Cu, gBl[2], 4);
            hxRegRead(&g, 0x90000018u, gBl[3], 4);
        }
    }
    gCpha = 0;
    g.close();
}

// Dump the bytes the chip returns on addresses that reacted, so the IC and its
// bootloader register map can be identified.
static void runHimaxDump(void) {
    Gpio g;
    if (!g.open()) {
        return;
    }
    g.setMode(kClk, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, 0);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 0);
    busDelay(nullptr, 10);
    g.setOut(kRst, 1);
    busDelay(nullptr, 50);
    uint8_t tx[8];
    uint8_t rx[8];
    for (unsigned m = 0; m < 2; m++) {
        gCpol = 0;
        gCpha = m;
        gMiOnes = 0;
        gXferN = 0;
        gTrace = false;
        for (unsigned a = 0; a < 256; a++) {
            tx[0] = 0xF3;
            tx[1] = (uint8_t)a;
            for (int i = 2; i < 7; i++) {
                tx[i] = 0;
            }
            if (busXfer(&g, tx, rx, 7)) {
                break;
            }
            unsigned any = (unsigned)rx[3] | rx[4] | rx[5] | rx[6];
            if (any) {
                kprintf("TSP d%u a%02x=%02x%02x%02x%02x\n", m, a, rx[3], rx[4], rx[5], rx[6]);
            }
        }
    }
    gCpha = 0;
    g.close();
}

// One identify attempt, bracketed, with the first transfers traced when asked.
// A hang therefore leaves the last step it reached on the console canvas.
static void runOneMode(unsigned n, unsigned cpol, unsigned cpha, unsigned slow, bool trace) {
    Gpio g;
    if (!g.open()) {
        kprintf("tsp n%u no gpio\n", n);
        return;
    }
    g.setMode(kClk, 0);
    g.setDirOut(kClk);
    g.setDirOut(kCs);
    g.setDirOut(kMo);
    g.setDirIn(kMi);
    g.setDirOut(kRst);
    g.setOut(kClk, cpol ? 1u : 0u);
    g.setOut(kCs, 1);
    g.setOut(kMo, 1);
    g.setOut(kRst, 1);
    uint8_t *scratch = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 16));
    uint8_t *rxbuf = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 16));
    if (!scratch || !rxbuf) {
        kprintf("tsp n%u no buffers\n", n);
        if (scratch) { IOFree(scratch, NVT_READ_MAX + 16); }
        if (rxbuf) { IOFree(rxbuf, NVT_READ_MAX + 16); }
        g.close();
        return;
    }
    gCpol = cpol;
    gCpha = cpha;
    gHalfClockScale = slow;
    gMiOnes = 0;
    gXferN = 0;
    gTrace = trace;
    struct nvt_bus bus = { &g, busXfer, busDelay };
    kprintf("tsp n%u rst\n", n);
    if (gRstMode == 1) {
        g.setOut(kRst, 0);
        busDelay(nullptr, 40);
    } else if (gRstMode == 2) {
        g.setOut(kRst, 1);
        busDelay(nullptr, 10);
        g.setOut(kRst, 0);
        busDelay(nullptr, 40);
    } else if (gRstMode == 3) {
        g.setOut(kRst, 0);
        busDelay(nullptr, 10);
        g.setOut(kRst, 1);
        busDelay(nullptr, 100);
    } else {
        hardwareReset(&g);
    }
    if (gPreWake == 2) {
        pinMuxGpioWord(&g, 4);
        g.setDirOut(4);
        g.setOut(4, 0);
        busDelay(nullptr, 10);
        g.setDirIn(4);
        g.setOut(4, 1);
        busDelay(nullptr, 5);
        kprintf("tsp n%u wkp\n", n);
    }
    if (gPreWake == 1) {
        uint8_t ff[8];
        for (int i = 0; i < 8; i++) {
            ff[i] = 0xff;
        }
        busXfer(&g, ff, nullptr, 8);
        busDelay(nullptr, 5);
        gXferN = 0;
        kprintf("tsp n%u wki\n", n);
    }
    kprintf("tsp n%u go\n", n);
    struct nvt_chip chip = {};
    chip.bus = &bus;
    chip.scratch = scratch;
    chip.rxbuf = rxbuf;
    if (gHoldCs) {
        g.setOut(kCs, 0);
        busDelay(nullptr, 1);
    }
    int r = nvt_identify(&chip);
    if (gHoldCs) {
        g.setOut(kCs, 1);
    }
    if (gRstMode != 0) {
        g.setOut(kRst, 1);
    }
    gTrace = false;
    gHalfClockScale = 1;
    kprintf("tsp n%u bk rc=%d one=%u x=%u\n", n, r, gMiOnes, gXferN);
    gSweepR[n] = r;
    gSweepOnes[n] = gMiOnes;
    if (r == 0) {
        for (int i = 0; i < 6; i++) {
            gStatusId[i] = chip.id[i];
        }
        kprintf("TSP hit n%u id=%02x%02x%02x%02x%02x%02x\n", n,
            chip.id[0], chip.id[1], chip.id[2], chip.id[3], chip.id[4], chip.id[5]);
    }
    IOFree(scratch, NVT_READ_MAX + 16);
    IOFree(rxbuf, NVT_READ_MAX + 16);
    g.close();
}

// Both phases and the reload-pattern poll, reprinted so a photo shows them.
static void printSummary(unsigned) {
    kprintf("TSP0 ic=%02x%02x%02x%02x tg=%02x%02x%02x%02x rl=%02x%02x%02x%02x fw=%02x%02x%02x%02x\n",
        gIc[0][0], gIc[0][1], gIc[0][2], gIc[0][3],
        gTg[0][0], gTg[0][1], gTg[0][2], gTg[0][3],
        gRl[0][0], gRl[0][1], gRl[0][2], gRl[0][3],
        gFw[0][0], gFw[0][1], gFw[0][2], gFw[0][3]);
    kprintf("TSP1 ic=%02x%02x%02x%02x tg=%02x%02x%02x%02x rl=%02x%02x%02x%02x fw=%02x%02x%02x%02x\n",
        gIc[1][0], gIc[1][1], gIc[1][2], gIc[1][3],
        gTg[1][0], gTg[1][1], gTg[1][2], gTg[1][3],
        gRl[1][0], gRl[1][1], gRl[1][2], gRl[1][3],
        gFw[1][0], gFw[1][1], gFw[1][2], gFw[1][3]);
    kprintf("TSP wr n=%u f=%02x%02x%02x%02x l=%02x%02x%02x%02x\n", gWn,
        gWf[0], gWf[1], gWf[2], gWf[3], gWl[0], gWl[1], gWl[2], gWl[3]);
    kprintf("TSP bl %02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x\n",
        gBl[0][0], gBl[0][1], gBl[0][2], gBl[0][3],
        gBl[1][0], gBl[1][1], gBl[1][2], gBl[1][3],
        gBl[2][0], gBl[2][1], gBl[2][2], gBl[2][3],
        gBl[3][0], gBl[3][1], gBl[3][2], gBl[3][3]);
    kprintf("TSP fl %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        gFl[0], gFl[1], gFl[2], gFl[3], gFl[4], gFl[5], gFl[6], gFl[7],
        gFl[8], gFl[9], gFl[10], gFl[11], gFl[12], gFl[13], gFl[14], gFl[15]);
    kprintf("TSP f2 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        gF2[0], gF2[1], gF2[2], gF2[3], gF2[4], gF2[5], gF2[6], gF2[7],
        gF2[8], gF2[9], gF2[10], gF2[11], gF2[12], gF2[13], gF2[14], gF2[15]);
    kprintf("TSP rs n=%u f0=%02x%02x%02x%02x v=%02x%02x%02x%02x w=%02x%02x%02x%02x fw=%02x%02x%02x%02x\n", gRn,
        gRf[0], gRf[1], gRf[2], gRf[3], gRv[0], gRv[1], gRv[2], gRv[3],
        gR2[0], gR2[1], gR2[2], gR2[3], gRs[0], gRs[1], gRs[2], gRs[3]);
}

// Timeline after the probe: baseline attempt, pad readback, a traced repeat,
// then every SPI mode. One experiment per step, one bracket print per phase.
static void lateLoop(void *, wait_result_t) {
    bool padDone = false;
    for (unsigned i = 0; ; i++) {
        IOSleep(10000);
        unsigned t = (i + 1) * 10;
        if (t == 20) {
            kprintf("tsp late go\n");
        } else if (t == 30) {
            runOneMode(0, 0, 0, 1, false);
        } else if (t == 60 && !padDone) {
            padDone = true;
            runPadChecks();
        } else if (t == 90) {
            runWatch();
        } else if (t == 120) {
            kprintf("tsp b120\n");
            runHimaxId();
        } else if (t == 150) {
            kprintf("tsp b150\n");
            runReload();
        } else if (t == 180) {
            kprintf("tsp b180\n");
            gPreWake = 1;
            runOneMode(2, 0, 0, 1, false);
            gPreWake = 0;
        } else if (t == 210) {
            kprintf("tsp b210\n");
            gRstMode = 3;
            runOneMode(3, 0, 0, 1, false);
            gRstMode = 0;
        } else if (t == 240) {
            kprintf("tsp b240\n");
            runFlashProbe();
        } else if (t == 270) {
            kprintf("tsp b270\n");
            runHimaxProbe();
        } else if (t == 300) {
            kprintf("tsp b300\n");
            runHimaxId();
        } else if (t == 330) {
            kprintf("tsp b330\n");
            runFlashProbe();
        } else {
            printSummary(t);
        }
    }
}

extern "C" void native_touch_spi_probe(void) {
    unsigned stage = 0;
    int found = PE_parse_boot_argn("touch", &stage, sizeof(stage));
    kprintf("tsp enter found=%d stage=%u\n", found, stage);
    if (!found || stage != 3) {
        return;
    }
    runPrep();
    thread_t t = nullptr;
    if (kernel_thread_start(lateLoop, nullptr, &t) == KERN_SUCCESS) {
        thread_deallocate(t);
    }
}
