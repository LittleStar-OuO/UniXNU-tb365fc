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
#include "himax_fw_data.h"

extern "C" {
// package/TouchState.cpp: coherent finger set for the HID digitizer.
void native_touch_publish(unsigned count, const int *ids, const int *xs, const int *ys);
extern volatile uint32_t native_touch_sink_ready;
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
static unsigned gHalfTicks;      // direct half-period override (CNTFRQ ticks)

void spinHalfClock(void) {
    uint64_t freq = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
    uint64_t ticks = gHalfTicks ? gHalfTicks
        : (freq / 2000000u) * gHalfClockScale;   // ~500 kHz / scale
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
static unsigned gRxAt;           // 0 first edge (legacy), 1 fall edge, 2 fall edge plus spin
static unsigned gShift = 23;     // reply stream offset in frame bits (probed at boot)
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
static uint32_t gSpi[40];         // SPI3 hardware-controller probe words
static uint8_t gHid[8];          // Himax pipe 0xD0 read over the hardware SPI
static uint8_t gRb[12];          // raw rx bytes of the 0x13 and 0x0D readback
static uint8_t gPipeRaw[8];      // un-realigned rx bytes of the last pipe read
static volatile unsigned gTlm[16];   // last touch-loop telemetry, printed in the summary
static volatile unsigned gTkCnt;     // completed telemetry ticks
static volatile unsigned gAb[6];     // 0x90060000 probe: {sumA,sumB,sumC, cc, cC, firstA}
static volatile unsigned gRnz;       // main-loop stack reads that were non-zero
static unsigned gT2;             // one-shot post-boot register experiments
static unsigned gIntEdges;       // GPIO4 transitions seen by the touch loop
static uint8_t gEq[4];           // 0x90000048 after the reload-to-active command
static unsigned gEqN;            // reload-to-active polls
static uint8_t gFs[4][4];        // fw status after reload: 0x10007000, 0x10007004, 0x10007010, 0x90000018
static unsigned gDw[24];         // fw download tallies
static uint8_t gDrb[16];         // flash readback at 0 after the download
static uint8_t gDr2[16];         // flash readback at 0x1000 after the download

static int (*gHwXfer)(const uint8_t *, uint8_t *, unsigned);

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    if (gHwXfer) {
        return gHwXfer(tx, rx, (unsigned)len);
    }
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
            if (!gCpha && gRxAt == 0) {
                unsigned v = g->getIn(kMi);
                gMiOnes += v;
                in = (uint8_t)((in << 1) | v);
            }
            g->setOut(kClk, idle);
            spinHalfClock();
            if (gRxAt == 1 || (gCpha && gRxAt == 0)) {
                unsigned v = g->getIn(kMi);
                gMiOnes += v;
                in = (uint8_t)((in << 1) | v);
            } else if (gRxAt == 2) {
                spinHalfClock();
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
    uint8_t tx[20];
    uint8_t rx[20];
    tx[0] = 0xF3;
    tx[1] = 0x08;
    for (unsigned i = 2; i < len + 4; i++) {
        tx[i] = 0;
    }
    busXfer(g, tx, rx, len + 4);
    // The reply stream lags the clock by one bit: data bit 0 arrives on the
    // last bit of the dummy byte (frame bit 23), not at the byte boundary.
    for (unsigned i = 0; i < len; i++) {
        unsigned b = 0;
        for (unsigned k = 0; k < 8; k++) {
            unsigned pos = (i * 8 + k) + gShift;
            b = (b << 1) | ((rx[pos >> 3] >> (7 - (pos & 7))) & 1u);
        }
        out[i] = (uint8_t)b;
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
    static uint8_t tx[164];
    static uint8_t rx[164];
    tx[0] = 0xF3;
    tx[1] = 0x08;
    for (unsigned i = 2; i < len + 4; i++) {
        tx[i] = 0;
    }
    busXfer(g, tx, rx, len + 4);
    for (unsigned i = 0; i < len; i++) {
        unsigned b = 0;
        for (unsigned k = 0; k < 8; k++) {
            unsigned pos = (i * 8 + k) + gShift;
            b = (b << 1) | ((rx[pos >> 3] >> (7 - (pos & 7))) & 1u);
        }
        out[i] = (uint8_t)b;
    }
}

static void hxRegWrite(Gpio *g, uint32_t addr, const uint8_t *data, unsigned len) {
    if (len > 2048) {
        len = 2048;
    }
    hxCmd(g, 0x13, 0x31);
    hxCmd(g, 0x0D, (addr != 0x8000002Cu && len > 4) ? 0x11 : 0x10);
    static uint8_t tx[2064];
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
    for (int i = 0; i < 6; i++) {
        gRb[i] = rx[1 + i];
    }
    tx[1] = 0x0D;
    busXfer(&g, tx, rx, 7);
    gRf[2] = rx[3];
    gRf[3] = rx[4];
    for (int i = 0; i < 6; i++) {
        gRb[6 + i] = rx[1 + i];
    }
    kprintf("TSP rb %02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x\n",
        gRb[0], gRb[1], gRb[2], gRb[3], gRb[4], gRb[5],
        gRb[6], gRb[7], gRb[8], gRb[9], gRb[10], gRb[11]);
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

// The vendor starts the touch firmware with an explicit reload command: write
// 0xEC to bootloader register 0x90000048 and poll it back until the chip
// answers 0x01EC. Reset first, issue the command, then look for the FW-side
// handshake 0xA33A at 0x10007000.
static void runReloadActive(void) {
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
    gCpol = 0;
    gCpha = 0;
    gMiOnes = 0;
    gXferN = 0;
    gTrace = false;
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    static const uint8_t ec[4] = { 0xEC, 0, 0, 0 };
    gEqN = 0;
    for (unsigned i = 0; i < 6; i++) {
        hxRegWrite(&g, 0x90000048u, ec, 4);
        busDelay(nullptr, 2);
        hxRegRead(&g, 0x90000048u, gEq, 4);
        gEqN = i + 1;
        if (gEq[0] == 0xEC && gEq[1] == 0x01) {
            break;
        }
        busDelay(nullptr, 3);
    }
    kprintf("TSP eq n=%u v=%02x%02x%02x%02x\n", gEqN, gEq[0], gEq[1], gEq[2], gEq[3]);
    for (unsigned i = 0; i < 25; i++) {
        uint8_t v[4];
        hxRegRead(&g, 0x10007000u, v, 4);
        for (int k = 0; k < 4; k++) {
            gFs[0][k] = v[k];
        }
        if (v[0] == 0xA3 && v[1] == 0x3A) {
            break;
        }
        busDelay(nullptr, 10);
    }
    hxRegRead(&g, 0x10007004u, gFs[1], 4);
    hxRegRead(&g, 0x10007010u, gFs[2], 4);
    hxRegRead(&g, 0x90000018u, gFs[3], 4);
    kprintf("TSP fs %02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x\n",
        gFs[0][0], gFs[0][1], gFs[0][2], gFs[0][3],
        gFs[1][0], gFs[1][1], gFs[1][2], gFs[1][3],
        gFs[2][0], gFs[2][1], gFs[2][2], gFs[2][3],
        gFs[3][0], gFs[3][1], gFs[3][2], gFs[3][3]);
    uint8_t tx[8];
    uint8_t rx[8];
    tx[0] = 0xF3;
    tx[1] = 0xD0;
    for (int i = 2; i < 7; i++) {
        tx[i] = 0;
    }
    busXfer(&g, tx, rx, 7);
    for (int i = 0; i < 4; i++) {
        gHid[i] = rx[3 + i];
    }
    g.close();
}

static void hxRegWriteU32(Gpio *g, uint32_t addr, uint32_t val) {
    uint8_t v[4] = { (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
    hxRegWrite(g, addr, v, 4);
}

// Vendor himax_mcu_wait_wip: poll status with control 0x42000003 and command
// 0x05; the controller reports idle as bit0 == 0 in the first status byte.
static bool hxWaitReady(Gpio *g, unsigned poll_ms) {
    hxRegWriteU32(g, 0x80000010u, 0x00020780u);
    for (unsigned i = 1; i <= 100; i++) {
        uint8_t v[4] = { 0xff, 0xff, 0xff, 0xff };
        hxRegWriteU32(g, 0x80000020u, 0x42000003u);
        hxRegWriteU32(g, 0x80000024u, 0x00000005u);
        busDelay(nullptr, 1);
        hxRegRead(g, 0x8000002Cu, v, 4);
        gDw[7] = v[0];
        if ((v[0] & 1) == 0) {
            return true;
        }
        busDelay(nullptr, poll_ms);
    }
    return false;
}

// Vendor page-read through the SPI200 controller (command 0x3B, transfer
// control 0x694002FF) as the factory FW header read used before.
static void hxFlashRead(Gpio *g, uint32_t addr, uint8_t *out, unsigned len) {
    hxRegWriteU32(g, 0x80000010u, 0x00020780u);
    hxRegWriteU32(g, 0x80000020u, 0x694002FFu);
    hxRegWriteU32(g, 0x80000028u, addr);
    hxRegWriteU32(g, 0x80000024u, 0x0000003Bu);
    busDelay(nullptr, 5);
    while (len) {
        unsigned n = len > 96 ? 96 : len;
        hxRegReadN(g, 0x8000002Cu, out, n);
        out += n;
        len -= n;
    }
}

// Vendor page program: WREN 0x06, gate on WEL (bit1 of the status read with
// control 0x42000000 and command 0x05), control 0x610FF000, page-aligned byte
// address, 16+240 bytes through the 0x8000002C data port, program 0x02.
// The gate is retried because the controller occasionally drops the WREN.
static bool hxProgramPage(Gpio *g, unsigned off) {
    uint8_t st[4] = { 0, 0, 0, 0 };
    bool gate = false;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        hxRegWriteU32(g, 0x80000020u, 0x47000000u);
        hxRegWriteU32(g, 0x80000024u, 0x00000006u);
        hxRegWriteU32(g, 0x80000020u, 0x42000000u);
        hxRegWriteU32(g, 0x80000024u, 0x00000005u);
        busDelay(nullptr, 1);
        hxRegRead(g, 0x8000002Cu, st, 4);
        if (st[0] & 0x02) {
            gate = true;
            break;
        }
        if (!gDw[6]) {
            gDw[6] = 0x100u | st[0];
        }
    }
    if (!gate) {
        return false;
    }
    hxRegWriteU32(g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(g, 0x80000020u, 0x610FF000u);
    hxRegWriteU32(g, 0x80000028u, off & ~0xFFu);
    hxRegWrite(g, 0x8000002Cu, gFwImg + off, 16);
    hxRegWriteU32(g, 0x80000024u, 0x00000002u);
    hxRegWrite(g, 0x8000002Cu, gFwImg + off + 16, 240);
    hxWaitReady(g, 1);
    return true;
}

static unsigned hxReadU32(Gpio *g, uint32_t addr) {
    uint8_t v[4] = { 0, 0, 0, 0 };
    hxRegRead(g, addr, v, 4);
    return (unsigned)v[0] | ((unsigned)v[1] << 8) | ((unsigned)v[2] << 16) | ((unsigned)v[3] << 24);
}

// Pure probe on a fresh bootloader: does the plain flash window still read the
// factory image, which controller writes actually land, and does WREN ever
// set WEL? No erase, no page program.
static void runFlashDiag(void) {
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
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    uint8_t w[16];
    hxRegReadN(&g, 0x00000000u, w, 16);
    gDw[0] = (unsigned)w[0] | ((unsigned)w[1] << 8) | ((unsigned)w[2] << 16) | ((unsigned)w[3] << 24);
    kprintf("TSP fi base %02x%02x%02x%02x%02x%02x%02x%02x\n",
        w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    hxRegWriteU32(&g, 0x80000010u, 0x00020780u);
    gDw[4] = hxReadU32(&g, 0x80000010u);
    hxRegReadN(&g, 0x00000000u, w, 16);
    gDw[6] = (unsigned)w[0] | ((unsigned)w[1] << 8) | ((unsigned)w[2] << 16) | ((unsigned)w[3] << 24);
    kprintf("TSP fi cfg10 %08x w=%02x%02x%02x%02x\n", gDw[4], w[0], w[1], w[2], w[3]);
    hxRegWriteU32(&g, 0x80000040u, 0x00000201u);
    gDw[5] = hxReadU32(&g, 0x80000040u);
    hxRegReadN(&g, 0x00000000u, w, 16);
    gDw[1] = (unsigned)w[0] | ((unsigned)w[1] << 8) | ((unsigned)w[2] << 16) | ((unsigned)w[3] << 24);
    kprintf("TSP fi cfg40 %08x w=%02x%02x%02x%02x\n", gDw[5], w[0], w[1], w[2], w[3]);
    hxRegWriteU32(&g, 0x80000040u, 0x00000000u);
    hxRegReadN(&g, 0x00000000u, w, 16);
    gDw[2] = (unsigned)w[0] | ((unsigned)w[1] << 8) | ((unsigned)w[2] << 16) | ((unsigned)w[3] << 24);
    kprintf("TSP fi r40 w=%02x%02x%02x%02x\n", w[0], w[1], w[2], w[3]);
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000020u, 0x42000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    uint8_t s[4] = { 0xff, 0xff, 0xff, 0xff };
    busDelay(nullptr, 1);
    hxRegRead(&g, 0x8000002Cu, s, 4);
    gDw[3] = 0x100u | s[0];
    kprintf("TSP fi wren %02x\n", s[0]);
    hxRegWriteU32(&g, 0x80000020u, 0x42000003u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    s[0] = 0xff;
    busDelay(nullptr, 1);
    hxRegRead(&g, 0x8000002Cu, s, 4);
    gDw[7] = s[0];
    kprintf("TSP fi sr2 %02x\n", s[0]);
    // Mid-image window reads: the factory image carried "HxDS user:" at
    // 0x2A0 and "HX83122-B" at 0x3D0, so these tell whether the flash still
    // holds the stock content or was blanked by the erase attempts.
    static const unsigned spots[] = { 0x2A0, 0x3D0, 0x1000, 0x10000, 0x20000, 0x30000, 0x3F000 };
    for (unsigned i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        uint8_t m[8];
        hxRegReadN(&g, spots[i], m, 8);
        gDw[12 + i] = (unsigned)m[0] | ((unsigned)m[1] << 8) | ((unsigned)m[2] << 16) | ((unsigned)m[3] << 24);
        kprintf("TSP w%05x %02x%02x%02x%02x%02x%02x%02x%02x\n", spots[i],
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
    }
    // SRAM window write test: is the firmware SRAM reachable through the
    // same register path the vendor uses to push images?
    hxRegWriteU32(&g, 0x10000000u, 0x12345678u);
    gDw[10] = hxReadU32(&g, 0x10000000u);
    hxRegWriteU32(&g, 0x100004F4u, 0x00000003u);
    gDw[11] = hxReadU32(&g, 0x100004F4u);
    gDw[19] = hxReadU32(&g, 0x10007000u);
    kprintf("TSP fi sram %08x t %08x h %08x\n", gDw[10], gDw[11], gDw[19]);
    // Vendor SRAM channel: push firmware section 0x0f00 (4096 B at file
    // offset 0x25000) to 0x10000F00 in 2 KB chunks, then read it back.
    for (unsigned off = 0; off < 4096; off += 2048) {
        hxRegWrite(&g, 0x10000F00u + off, gFwImg + 0x25000 + off, 2048);
        busDelay(nullptr, 10);
    }
    unsigned smis = 0;
    unsigned smfirst = 0xffffffffu;
    for (unsigned off = 0; off < 4096; off += 128) {
        uint8_t rb[128];
        hxRegReadN(&g, 0x10000F00u + off, rb, 128);
        for (unsigned i = 0; i < 128; i++) {
            if (rb[i] != gFwImg[0x25000 + off + i]) {
                smis++;
                if (smfirst == 0xffffffffu) {
                    smfirst = off + i;
                }
            }
        }
    }
    gDw[20] = smis;
    gDw[21] = smfirst;
    kprintf("TSP s0 mx=%u mo=%x\n", smis, smfirst);
    g.close();
}

// Zero-Flash bring-up: write the SRAM-bound firmware sections to
// 0x10000000|offset, then run the vendor power-on sequence
// (0x100006EC=0, 0x10000694=1, 0x100006C0=0, poll 0x72C0) and look for the
// FW handshake 0xA33A at 0x10007000.
static void runSramDownload(void) {
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
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    // Vendor fw-update entry: system reset 0x90000018 <- 0x55.
    hxRegWriteU32(&g, 0x90000018u, 0x00000055u);
    busDelay(nullptr, 30);
    // SRAM parameter writes decoded from the fw table full-address rows:
    // {0x10000000<-0x21502, 0x10000100<-0x21505, 0x10000200<-0x21507,
    //  0x10000300<-0x21544 (HX83122-B), 0x10000600<-0x21600}.
    static const unsigned prm[][2] = {
        { 0x10000000u, 0x21502u },
        { 0x10000100u, 0x21505u },
        { 0x10000200u, 0x21507u },
        { 0x10000300u, 0x21544u },
        { 0x10000600u, 0x21600u },
    };
    unsigned segs = 0, bytes = 0;
    for (unsigned s = 0; s < sizeof(prm) / sizeof(prm[0]); s++) {
        hxRegWrite(&g, prm[s][0], gFwImg + prm[s][1], 16);
        busDelay(nullptr, 10);
        segs++;
        bytes += 16;
        kprintf("TSP zf p%08x n16\n", prm[s][0]);
    }
    gDw[0] = segs;
    gDw[1] = bytes;
    // Verify the panel-id write at 0x10000300 end to end.
    unsigned smis = 0;
    {
        uint8_t rb[16];
        hxRegReadN(&g, 0x10000300u, rb, 16);
        for (unsigned i = 0; i < 16; i++) {
            if (rb[i] != gFwImg[0x21544 + i]) {
                smis++;
            }
        }
    }
    gDw[2] = smis;
    // Vendor power_on_init: idle reg, then 0x100006C0 sense handshake.
    hxRegWriteU32(&g, 0x100006ECu, 0x00000000u);
    hxRegWriteU32(&g, 0x10000694u, 0x00000001u);
    hxRegWriteU32(&g, 0x100006C0u, 0x00000000u);
    gDw[3] = 0;
    uint8_t v[4] = { 0, 0, 0, 0 };
    for (unsigned i = 1; i <= 30; i++) {
        hxRegRead(&g, 0x100006C0u, v, 4);
        if (v[0] == 0xC0 && v[1] == 0x72) {
            gDw[3] = i;
            break;
        }
        busDelay(nullptr, 2);
    }
    gDw[4] = (unsigned)v[0] | ((unsigned)v[1] << 8);
    gDw[5] = 0;
    for (unsigned i = 1; i <= 30; i++) {
        uint8_t w4[4];
        hxRegRead(&g, 0x10007000u, w4, 4);
        gDw[6] = (unsigned)w4[0] | ((unsigned)w4[1] << 8);
        if (w4[0] == 0xA3 && w4[1] == 0x3A) {
            gDw[5] = i;
            break;
        }
        busDelay(nullptr, 5);
    }
    gDw[7] = hxReadU32(&g, 0x10000000u);
    kprintf("TSP zf segs=%u by=%u mx=%u p=%u c6=%04x a=%u h=%04x\n",
        gDw[0], gDw[1], gDw[2], gDw[3], gDw[4], gDw[5], gDw[6]);
    g.close();
}

// Zero-Flash partition table, resolved from the vendor descriptor:
//   The descriptor rows at 0x60 carry map code TP_CONFIG_TABLE (0x00000A00)
//   and point at file offset 0x20500. Its +12 byte is pnum = 13.
//   Row 0 is the FW entity: 129024 B from file 0x500 to SRAM 0x20000000.
//   Rows 1..12 are DSRAM sections at 0x10000000..0x10006BE8.
// The vendor packs every section into one contiguous image based at the lowest
// SRAM address and writes that image once. Writing each section to
// 0x10000000|offset instead makes later sections overwrite earlier ones, which
// is what the earlier SRAM experiments hit.
static unsigned gZf[24];         // pn, fs, cs, fw crc, cfg crc, rl, c6, up, cs state, status word, [12..15] cmp, [16..23] probes
static unsigned gP[8];           // SRAM write-probe matrices: {misIS,fstIS,misF0,fstF0} x {pre,post} reset
static volatile unsigned gZfPhase;   // last completed step, readable on one console line
static unsigned gQ[8];           // size-sweep mismatch counts
static uint8_t gPat8k[8192];
static unsigned gR[8];           // ISRAM/DSRAM 8K probe buckets
static volatile bool gZfDone;
static volatile unsigned gTxFrames, gTxReports, gTxLastN, gTxSink;
static volatile unsigned gLab1[9];   // fw_ver,cfg,vendor,cus,proj,fw_state,dbg,mode488,psl
static volatile unsigned gLab2[8];   // pre/re-arm stack poll counts
static volatile unsigned gLab4[8];   // stylus regs, DSRAM window dwords at tick
static volatile unsigned gLab5[8];   // live DSRAM sweep stats at tick

static uint32_t zfWord(unsigned index, unsigned field) {
    const uint8_t *r = gFwImg + 0x20500u + index * 16u + field * 4u;
    return (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
}

static void zfWrite(Gpio *g, uint32_t addr, const uint8_t *data, unsigned len) {
    for (unsigned off = 0; off < len; off += 512) {
        unsigned n = len - off > 512 ? 512 : len - off;
        hxRegWrite(g, addr + off, data + off, n);
        IOSleep(5);
    }
}

// Compare sampled 64-byte windows; the full image readback would take longer
// than the boot experiment allows.
static unsigned gZfFirst;        // first differing byte offset of the last compare run
static unsigned zfCompare(Gpio *g, uint32_t addr, const uint8_t *ref, unsigned len, unsigned step) {
    unsigned bad = 0;
    uint8_t rb[64];
    gZfFirst = 0xFFFFFFFFu;
    for (unsigned off = 0; off < len; off += step) {
        unsigned n = len - off > 64 ? 64 : len - off;
        hxRegReadN(g, addr + off, rb, n);
        for (unsigned i = 0; i < n; i++) {
            if (rb[i] != ref[off + i]) {
                bad++;
                if (gZfFirst == 0xFFFFFFFFu) {
                    gZfFirst = off + i;
                }
            }
        }
        IOSleep(1);
    }
    return bad;
}

// Vendor himax_mcu_calc_crc_by_hw: point the reload engine at an SRAM range,
// kick it with (words | 0x99 << 16), wait for status bit0 to fall, then read
// the residue. Zero means the chip holds exactly the bytes just written.
static unsigned hxSramCrc(Gpio *g, uint32_t addr, uint32_t len) {
    hxRegWriteU32(g, 0x80050020u, addr);
    const uint32_t words = len / 4;
    uint8_t cmd[4] = { (uint8_t)words, (uint8_t)(words >> 8), 0x99, 0x00 };
    hxRegWrite(g, 0x80050028u, cmd, 4);
    for (unsigned i = 0; i < 100; i++) {
        uint8_t st[4] = { 0, 0, 0, 0 };
        hxRegRead(g, 0x80050000u, st, 4);
        if ((st[0] & 1u) == 0) {
            uint8_t r[4] = { 0, 0, 0, 0 };
            hxRegRead(g, 0x80050018u, r, 4);
            return (unsigned)r[0] | ((unsigned)r[1] << 8) | ((unsigned)r[2] << 16) | ((unsigned)r[3] << 24);
        }
        IOSleep(2);
    }
    return 0xFFFFFFFFu;
}

static void runZfDownload(void) {
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
    gZfPhase = 1;

    const unsigned pnum = gFwImg[0x20500u + 12u];
    gZf[0] = pnum;
    if (pnum < 2 || pnum > 32) {
        kprintf("TSP zf2 pnum=%u invalid\n", pnum);
        gZfPhase = 0;
        gZfDone = true;
        g.close();
        return;
    }
    const uint32_t fwAddr = zfWord(0, 0), fwSize = zfWord(0, 1), fwOff = zfWord(0, 2);
    uint32_t base = 0xFFFFFFFFu, end = 0;
    for (unsigned i = 1; i < pnum; i++) {
        const uint32_t addr = zfWord(i, 0), size = zfWord(i, 1);
        if (addr < base) {
            base = addr;
        }
        if (addr + size > end) {
            end = addr + size;
        }
    }
    uint32_t cfgSize = end > base ? end - base : 0;
    cfgSize = (cfgSize + 15u) & ~15u;
    if (fwAddr < 0x20000000u || fwOff + fwSize > sizeof(gFwImg) || base < 0x10000000u
        || cfgSize == 0 || cfgSize > 0x10000u) {
        kprintf("TSP zf2 layout fw=%08x/%u@%x cfg=%08x/%u bad\n", fwAddr, fwSize, fwOff, base, cfgSize);
        gZfPhase = 0;
        gZfDone = true;
        g.close();
        return;
    }
    gZf[1] = fwSize;
    gZf[2] = cfgSize;

    gZfPhase = 2;
    // Vendor interface_on: a dummy AHB read wakes the bus, then the burst pair.
    uint8_t wake[4] = { 0, 0, 0, 0 };
    kprintf("zf w1 wake\n");
    hxRegRead(&g, 0x10000000u, wake, 4);
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    hxRegWriteU32(&g, 0x20000040u, 0xA5A5C33Cu);
    gZf[20] = hxReadU32(&g, 0x20000040u);
    kprintf("zf w1b t4=%08x\n", gZf[20]);
    zfWrite(&g, 0x20000000u, gFwImg + 0x25000, 0x1000);
    gP[0] = zfCompare(&g, 0x20000000u, gFwImg + 0x25000, 0x1000, 256);
    gP[1] = gZfFirst;
    zfWrite(&g, 0x10000F00u, gFwImg + 0x25000, 0x1000);
    gP[2] = zfCompare(&g, 0x10000F00u, gFwImg + 0x25000, 0x1000, 256);
    gP[3] = gZfFirst;
    kprintf("zf w1c pre is=%u/%x f0=%u/%x\n", gP[0], gP[1], gP[2], gP[3]);

    // himax_mcu_firmware_update_0f opens with a system reset, then parks the
    // running FW: DATA_FW_STOP, then the safe-mode password with a verified
    // 0x0C central state and a TCON reset, exactly like hx83102e_sense_off.
    kprintf("zf w2 reset\n");
    gZfPhase = 3;
    hxRegWriteU32(&g, 0x90000018u, 0x00000055u);
    busDelay(nullptr, 30);
    kprintf("zf w3 safe\n");
    gZfPhase = 4;
    hxRegWriteU32(&g, 0x9000005Cu, 0x000000A5u);
    busDelay(nullptr, 10);
    unsigned smOk = 0, smN = 0, smCs = 0;
    for (unsigned i = 0; i < 6; i++) {
        uint8_t off[4] = { 0xF2, 0x31, 0x27, 0x95 };
        busXfer(&g, off, nullptr, 4);
        busDelay(nullptr, 10);
        uint8_t cs[4] = { 0, 0, 0, 0 };
        hxRegRead(&g, 0x900000A8u, cs, 4);
        smCs = cs[0];
        smN = i + 1;
        if (cs[0] == 0x0C) {
            smOk = 1;
            break;
        }
        g.setOut(kRst, 0);
        busDelay(nullptr, 2);
        g.setOut(kRst, 1);
        busDelay(nullptr, 12);
    }
    if (smOk) {
        hxRegWriteU32(&g, 0x80020004u, 0x00000000u);
        busDelay(nullptr, 10);
    }
    gZf[8] = smOk ? (0x100u | smN) : smN;
    kprintf("zf w4 sm=%u/%u cs=%02x\n", smOk, smN, smCs);
    gZfPhase = 6;

    // FW entity first, exactly like the vendor: write, then HW CRC (3 tries).
    for (unsigned i = 0; i < 3; i++) {
        kprintf("zf w6 fw%u\n", i);
        gZfPhase = 7;
        zfWrite(&g, fwAddr, gFwImg + fwOff, fwSize);
        kprintf("zf w7 crc%u\n", i);
        gZfPhase = 8;
        gZf[3] = hxSramCrc(&g, fwAddr, fwSize);
        if (gZf[3] == 0) {
            break;
        }
    }
    if (gZf[3] == 0xFFFFFFFFu) {
        kprintf("zf w8 cmp\n");
        gZf[3] = zfCompare(&g, fwAddr, gFwImg + fwOff, fwSize, 4096);
    }
    gZf[12] = zfCompare(&g, fwAddr, gFwImg + fwOff, fwSize, 8192);
    gZf[13] = gZfFirst;
    gZf[18] = hxReadU32(&g, fwAddr);
    kprintf("zf w8b fw0=%08x\n", gZf[18]);
    gZfPhase = 9;
    kprintf("zf w9 cfga %u\n", cfgSize);

    uint8_t *cfg = static_cast<uint8_t *>(IOMalloc(cfgSize));
    if (!cfg) {
        kprintf("TSP zf2 cfg alloc fail\n");
        gZfPhase = 0;
        gZfDone = true;
        g.close();
        return;
    }
    memset(cfg, 0, cfgSize);
    for (unsigned i = 1; i < pnum; i++) {
        const uint32_t addr = zfWord(i, 0), size = zfWord(i, 1), off2 = zfWord(i, 2);
        if (size && off2 + size <= sizeof(gFwImg) && addr >= base && addr + size <= end) {
            memcpy(cfg + (addr - base), gFwImg + off2, size);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        kprintf("zf wa cfg%u\n", i);
        gZfPhase = 10;
        zfWrite(&g, base, cfg, cfgSize);
        kprintf("zf wb cfgc%u\n", i);
        gZfPhase = 11;
        gZf[4] = hxSramCrc(&g, base, cfgSize);
        if (gZf[4] == 0) {
            break;
        }
    }
    if (gZf[4] == 0xFFFFFFFFu) {
        gZf[4] = zfCompare(&g, base, cfg, cfgSize, 1024);
    }
    gZf[14] = zfCompare(&g, base, cfg, cfgSize, 2048);
    gZf[15] = gZfFirst;
    gZf[19] = hxReadU32(&g, base);
    kprintf("zf wc2 cf0=%08x\n", gZf[19]);
    IOFree(cfg, cfgSize);
    gZfPhase = 12;
    kprintf("zf wc tail\n");

    // hx_dis_rload_0f(0): ZF builds disable the flash reload path with 0x9AA9.
    hxRegWriteU32(&g, 0x10000100u, 0x00009AA9u);
    // himax_mcu_power_on_init: rawout select, sorting mode, one frame, then a
    // cleared reload-done register before sense_on.
    hxRegWriteU32(&g, 0x100006ECu, 0x00000000u);
    hxRegWriteU32(&g, 0x10000104u, 0x00000000u);
    hxRegWriteU32(&g, 0x10000694u, 0x00000001u);
    hxRegWriteU32(&g, 0x100006C0u, 0x00000000u);
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    hxRegWriteU32(&g, 0x9000005Cu, 0x00000000u);
    busDelay(nullptr, 10);
    gZfPhase = 5;

    // ic_reset(0) in this vendor build is the cmd reset of hx83122_system_reset:
    // ctrl_fw clear, safe-mode pulse 0x31 <- 27 95, 20 ms, 0x31 <- 00, 10 ms,
    // then wait for FLAG_RESET_EVENT at 0x900000E4 to read 00 02. The reset
    // pin is not part of this build's path.
    kprintf("zf wd rst\n");
    gZfPhase = 13;
    hxRegWriteU32(&g, 0x9000005Cu, 0x00000000u);
    busDelay(nullptr, 10);
    unsigned rsOk = 0, rsN = 0, rsV = 0;
    for (unsigned i = 0; i < 6; i++) {
        uint8_t off[4] = { 0xF2, 0x31, 0x27, 0x95 };
        busXfer(&g, off, nullptr, 4);
        busDelay(nullptr, 20);
        uint8_t off0[3] = { 0xF2, 0x31, 0x00 };
        busXfer(&g, off0, nullptr, 3);
        busDelay(nullptr, 10);
        uint8_t fe[4] = { 0, 0, 0, 0 };
        hxRegRead(&g, 0x900000E4u, fe, 4);
        rsV = (unsigned)fe[0] | ((unsigned)fe[1] << 8);
        rsN = i + 1;
        if (fe[0] == 0x00 && fe[1] == 0x02) {
            rsOk = 1;
            break;
        }
    }
    kprintf("zf wd2 rs=%u/%u v=%04x\n", rsOk, rsN, rsV);
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);

    // sense_on tail: reload_to_active. Writing 0xEC and reading back EC 01 is
    // the actual "jump to SRAM firmware" trigger the vendor polls for.
    gZfPhase = 14;
    kprintf("zf we ra\n");
    unsigned rlOk = 0, rlLast = 0, rlVal = 0;
    {
        uint8_t v[4] = { 0, 0, 0, 0 };
        for (rlLast = 1; rlLast <= 20; rlLast++) {
            hxRegWriteU32(&g, 0x90000048u, 0x000000ECu);
            busDelay(nullptr, 1);
            hxRegRead(&g, 0x90000048u, v, 4);
            rlVal = (unsigned)v[0] | ((unsigned)v[1] << 8);
            if (v[0] == 0xEC && v[1] == 0x01) {
                rlOk = 1;
                break;
            }
        }
    }
    gZf[22] = rlOk ? (rlLast | 0x100u) : rlLast;
    gZf[23] = rlVal;

    // power_on_init waits up to 30 x 10 ms for the FW to publish C0 72 00 00.
    gZfPhase = 15;
    kprintf("zf wf c6\n");
    unsigned reload = 0;
    unsigned c6 = 0;
    {
        uint8_t v[4] = { 0, 0, 0, 0 };
        for (unsigned i = 0; i < 30; i++) {
            hxRegRead(&g, 0x100006C0u, v, 4);
            c6 = (unsigned)v[0] | ((unsigned)v[1] << 8);
            gZf[16] = (unsigned)v[0] | ((unsigned)v[1] << 8) | ((unsigned)v[2] << 16) | ((unsigned)v[3] << 24);
            if (v[0] == 0xC0 && v[1] == 0x72 && v[2] == 0 && v[3] == 0) {
                reload = 1;
                break;
            }
            IOSleep(10);
        }
    }
    gZfPhase = 15;
    kprintf("zf wf c6=%04x\n", c6);

    // Vendor tail after the update: _reload_disable(0) then a second full
    // power_on_init. Clearing 0x10000100 back to the reload-enable value and
    // re-running sense_on is what the stock driver does before the FW scans.
    unsigned reload2 = 0, c62 = 0, rl2Ok = 0, rl2N = 0, rs2Ok = 0, rs2N = 0;
    if (reload) {
        hxRegWriteU32(&g, 0x10000100u, 0x00000000u);
        hxRegWriteU32(&g, 0x100006ECu, 0x00000000u);
        hxRegWriteU32(&g, 0x10000104u, 0x00000000u);
        hxRegWriteU32(&g, 0x10000694u, 0x00000001u);
        hxRegWriteU32(&g, 0x100006C0u, 0x00000000u);
        hxRegWriteU32(&g, 0x9000005Cu, 0x00000000u);
        busDelay(nullptr, 10);
        // ux2/xun sense_on order: whole-chip PIN reset (RST low 5 ms, high
        // 5 ms) so the TDDI TCON/AFE restart too, then reload to active.
        g.setOut(kRst, 0);
        busDelay(nullptr, 5000);
        g.setOut(kRst, 1);
        busDelay(nullptr, 5000);
        rs2Ok = 2;
        rs2N = 1;
        hxCmd(&g, 0x13, 0x31);
        hxCmd(&g, 0x0D, 0x10);
        {
            uint8_t v[4] = { 0, 0, 0, 0 };
            for (rl2N = 1; rl2N <= 20; rl2N++) {
                hxRegWriteU32(&g, 0x90000048u, 0x000000ECu);
                busDelay(nullptr, 1);
                hxRegRead(&g, 0x90000048u, v, 4);
                if (v[0] == 0xEC && v[1] == 0x01) {
                    rl2Ok = 1;
                    break;
                }
            }
        }
        if (!rl2Ok) {
            // Fall back to the vendor cmd reset (password pulses).
            for (unsigned i = 0; i < 6; i++) {
                uint8_t off[4] = { 0xF2, 0x31, 0x27, 0x95 };
                busXfer(&g, off, nullptr, 4);
                busDelay(nullptr, 20);
                uint8_t off0[3] = { 0xF2, 0x31, 0x00 };
                busXfer(&g, off0, nullptr, 3);
                busDelay(nullptr, 10);
                uint8_t fe[4] = { 0, 0, 0, 0 };
                hxRegRead(&g, 0x900000E4u, fe, 4);
                rs2N = i + 1;
                if (fe[0] == 0x00 && fe[1] == 0x02) {
                    rs2Ok = 1;
                    break;
                }
            }
            hxCmd(&g, 0x13, 0x31);
            hxCmd(&g, 0x0D, 0x10);
            {
                uint8_t v[4] = { 0, 0, 0, 0 };
                for (rl2N = 1; rl2N <= 20; rl2N++) {
                    hxRegWriteU32(&g, 0x90000048u, 0x000000ECu);
                    busDelay(nullptr, 1);
                    hxRegRead(&g, 0x90000048u, v, 4);
                    if (v[0] == 0xEC && v[1] == 0x01) {
                        rl2Ok = 1;
                        break;
                    }
                }
            }
        }
        {
            uint8_t v[4] = { 0, 0, 0, 0 };
            for (unsigned i = 0; i < 30; i++) {
                hxRegRead(&g, 0x100006C0u, v, 4);
                c62 = (unsigned)v[0] | ((unsigned)v[1] << 8);
                if (v[0] == 0xC0 && v[1] == 0x72 && v[2] == 0 && v[3] == 0) {
                    reload2 = 1;
                    break;
                }
                IOSleep(10);
            }
        }
        kprintf("zf wf2 rs=%u/%u rl=%u/%u c6=%04x\n", rs2Ok, rs2N, rl2Ok, rl2N, c62);
        gZf[18] = rs2Ok ? (rs2N | 0x100u) : rs2N;
        gZf[19] = (rl2Ok ? 0x100u : 0) | rl2N;
        gZf[20] = (reload2 ? 0x10000u : 0) | c62;
        if (reload2) {
            c6 = c62;
            reload = 2;
        }
    }
    gZf[17] = hxReadU32(&g, 0x900000A8u);
    gZfPhase = 16;
    kprintf("zf wg a8=%08x\n", gZf[17]);
    gZf[21] = hxReadU32(&g, 0x10007000u);
    gZfPhase = 17;
    kprintf("zf wh h=%08x\n", gZf[21]);
    gZf[5] = reload;
    gZf[6] = c6;
    gZfPhase = 18;

    if (reload) {
        uint8_t v[4] = { 0, 0, 0, 0 };
        hxRegRead(&g, 0x10007000u, v, 4);
        gZf[9] = (unsigned)v[0] | ((unsigned)v[1] << 8) | ((unsigned)v[2] << 16) | ((unsigned)v[3] << 24);
        hxRegRead(&g, 0x100004F4u, v, 4);
        gZf[11] = v[0];
        hxRegRead(&g, 0x100004F8u, v, 4);
        gZf[10] = v[0];
        gZf[7] = 1;
    }
    kprintf("TSP zr sm=%u/%u rs=%u/%u/%04x rl=%u/%u/%08x c6=%04x a8=%08x hd=%08x\n",
        smOk, smN, rsOk, rsN, rsV, rlOk, rlLast, rlVal, c6, gZf[17], gZf[21]);
    kprintf("TSP zf2 pn=%u fs=%u cs=%u fw=%08x cf=%08x rl=%u c6=%04x up=%u\n",
        gZf[0], gZf[1], gZf[2], gZf[3], gZf[4], gZf[5], gZf[6], gZf[7]);
    kprintf("zf wf done up=%u cm=%u/%08x cm2=%u/%08x\n", gZf[7], gZf[12], gZf[13], gZf[14], gZf[15]);
    gZfDone = true;
    g.close();
}

// The vendor reads one 128-byte event-stack frame with pipe command 0x30:
// [F3][0x30][0] then 128 bytes, data from rx[3]. Each finger is four bytes of
// big-endian x/y, followed by one width byte per finger; the count is at
// touch_info_size - 4, where touch_info_size = (max_pt + raw_cnt_max + 2) * 4.
static void hxPipeRead(Gpio *g, uint8_t pipe, uint8_t *out, unsigned len) {
    if (len > 128) {
        len = 128;
    }
    static uint8_t tx[160];
    static uint8_t rx[160];
    tx[0] = 0xF3;
    tx[1] = pipe;
    for (unsigned i = 2; i < len + 3; i++) {
        tx[i] = 0;
    }
    busXfer(g, tx, rx, len + 3);
    for (unsigned i = 0; i < 8; i++) {
        gPipeRaw[i] = rx[2 + i];
    }
    // Same one-bit reply lag as the register path: data bit 0 sits on the last
    // bit of the dummy byte (frame bit 23).
    for (unsigned i = 0; i < len; i++) {
        unsigned b = 0;
        for (unsigned k = 0; k < 8; k++) {
            unsigned pos = (i * 8 + k) + gShift;
            b = (b << 1) | ((rx[pos >> 3] >> (7 - (pos & 7))) & 1u);
        }
        out[i] = (uint8_t)b;
    }
}

// Raw touch space is 16000 x 25600; the display finger set is 30480 x 20320.
// touch-rotate selects the panel orientation: 0 portrait, 1 90 deg clockwise
// (the default landscape orientation), 2 upside down, 3 90 deg counter-clockwise.
static void mapPoint(unsigned rotate, unsigned x, unsigned y, int *dx, int *dy) {
    unsigned a = x, b = y;
    switch (rotate & 3u) {
    case 0: a = x; b = y; break;
    case 1: a = 25600u - y; b = x; break;
    case 2: a = 16000u - x; b = 25600u - y; break;
    default: a = y; b = 16000u - x; break;
    }
    const unsigned spanX = (rotate & 1u) ? 25600u : 16000u;
    const unsigned spanY = (rotate & 1u) ? 16000u : 25600u;
    *dx = (int)((unsigned long long)a * 30480u / spanX);
    *dy = (int)((unsigned long long)b * 20320u / spanY);
}

static void hwProbe(void);

static void himaxTouchThread(void *, wait_result_t) {
    for (unsigned i = 0; i < 120 && !gZfDone; i++) {
        IOSleep(1000);
    }
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
    g.setOut(kRst, 1);
    gCpol = 0;
    gCpha = 0;

    uint8_t v[4] = { 0, 0, 0, 0 };
    // Stock DTS: spi-cpha, driver forces SPI_MODE_3. Probe (CPOL/CPHA,
    // sampling position, reply shift) until the FW handshake reads C0 72.
    unsigned md = 0;
    {
        uint8_t t[4] = { 0, 0, 0, 0 };
        static const unsigned cps[2][2] = { { 0, 0 }, { 1, 1 } };
        static const unsigned ats[3] = { 0, 1, 2 };
        for (unsigned m = 0; m < 2 && !md; m++) {
            for (unsigned a = 0; a < 3 && !md; a++) {
                for (unsigned sh = 23; sh <= 24 && !md; sh++) {
                    gCpol = cps[m][0];
                    gCpha = cps[m][1];
                    gRxAt = ats[a];
                    gShift = sh;
                    hxRegRead(&g, 0x100006C0u, t, 4);
                    if (t[0] == 0xC0 && t[1] == 0x72) {
                        md = 1 + m * 10 + a * 2 + (sh - 23);
                    }
                }
                if (!md) {
                    gShift = 23;
                }
            }
        }
        if (!md) {
            gCpol = 0;
            gCpha = 0;
            gRxAt = 0;
            gShift = 23;
        }
        kprintf("TSP md m=%u\n", md);
    }
    unsigned up = 0;
    // The vendor power_on_init polls this register for the reload-done
    // signature after the IC reset; 0x72C0 is the only proof the FW runs.
    for (unsigned i = 0; i < 10; i++) {
        hxRegRead(&g, 0x100006C0u, v, 4);
        if (v[0] == 0xC0 && v[1] == 0x72) {
            up = 1;
            break;
        }
        busDelay(nullptr, 2000);
    }
    if (!up) {
        uint8_t a8p[4] = { 0, 0, 0, 0 }, ptp[4] = { 0, 0, 0, 0 };
        hxRegRead(&g, 0x900000A8u, a8p, 4);
        hxRegRead(&g, 0x100004F8u, ptp, 4);
        if (a8p[0] == 0x05 || ptp[0] == 10) {
            up = 2;
        }
    }
    if (!up) {
        uint8_t a8[4] = { 0, 0, 0, 0 }, f0[4] = { 0, 0, 0, 0 }, f4[4] = { 0, 0, 0, 0 };
        hxRegRead(&g, 0x900000A8u, a8, 4);
        hxRegRead(&g, 0x10007000u, f0, 4);
        hxRegRead(&g, 0x10007004u, f4, 4);
        uint8_t raw[16];
        hxPipeRead(&g, 0x30, raw, 16);
        kprintf("TSP tx nh %02x%02x a8=%08x f0=%08x f4=%08x p0=%02x%02x%02x%02x%02x%02x%02x%02x\n",
            v[0], v[1],
            (unsigned)a8[0] | ((unsigned)a8[1] << 8) | ((unsigned)a8[2] << 16) | ((unsigned)a8[3] << 24),
            (unsigned)f0[0] | ((unsigned)f0[1] << 8) | ((unsigned)f0[2] << 16) | ((unsigned)f0[3] << 24),
            (unsigned)f4[0] | ((unsigned)f4[1] << 8) | ((unsigned)f4[2] << 16) | ((unsigned)f4[3] << 24),
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
        // Keep watching the event stack without publishing: if a stock FW is
        // alive the count byte moves on touch and the gTx counters carry that
        // into the heartbeat line.
        kprintf("TSP tx nh fallthrough\n");
    }
    hxRegRead(&g, 0x100004F8u, v, 4);
    unsigned maxPt = v[0] ? v[0] : 10;
    if (maxPt > 10) {
        maxPt = 10;
    }
    unsigned rotate = 1;
    PE_parse_boot_argn("touch-rotate", &rotate, sizeof(rotate));
    gTxSink = native_touch_sink_ready;
    kprintf("TSP tx up maxpt=%u rot=%u sink=%u\n", maxPt, rotate & 3u, native_touch_sink_ready);

    hwProbe();

    // diag334: the stack is FW-fed, so the slow 500 kHz bit clock may be
    // outside its service window. Raise the clock while proving register
    // round-trips still work, then run the stack tests at that speed.
    {
        static const unsigned ladder[3] = { 6, 8, 4 };
        unsigned ok = 0, used = 0;
        for (unsigned s = 0; s < 3 && !ok; s++) {
            gHalfTicks = ladder[s];
            uint8_t pat[4] = { 0x5A, 0xA5, 0x3C, 0xC3 };
            uint8_t rb[4] = { 0, 0, 0, 0 };
            hxRegWrite(&g, 0x20000040u, pat, 4);
            hxRegReadN(&g, 0x20000040u, rb, 4);
            ok = rb[0] == 0x5A && rb[1] == 0xA5 && rb[2] == 0x3C && rb[3] == 0xC3;
            used = gHalfTicks;
        }
        if (!ok) {
            gHalfTicks = 0;
        }
        gTlm[5] = gHalfTicks;
        kprintf("TSP hs t=%u ok=%u\n", used, ok);
    }

    // diag331: pristine one-shot stack snapshots. Nothing touches pipe 0x30
    // before these reads; each snapshot samples the INT pin first, then pops
    // exactly one frame, so the first-ever stack access of the boot is clean.
    {
        gLab1[0] = hxReadU32(&g, 0x10000404u);   // fw_ver
        gLab1[1] = hxReadU32(&g, 0x10000484u);   // fw_cfg
        gLab1[2] = hxReadU32(&g, 0x10000400u);   // fw_vendor
        gLab1[3] = hxReadU32(&g, 0x10000408u);   // cus_info
        gLab1[4] = hxReadU32(&g, 0x10000414u);   // proj_info
        gLab1[5] = hxReadU32(&g, 0x900000F8u);   // fw_state
        gLab1[6] = hxReadU32(&g, 0x10000140u);   // fw_dbg_msg
        gLab1[7] = hxReadU32(&g, 0x10000488u);   // fw_mode_status/chk_irq_edge
        gLab1[8] = hxReadU32(&g, 0x900000A0u);   // psl
        gLab4[0] = hxReadU32(&g, 0x1000059Cu);   // info_def_stylus
        static const unsigned waits[4] = { 0, 5000, 15000, 25000 };
        for (unsigned dsm = 0; dsm < 4; dsm++) {
            if (waits[dsm]) {
                IOSleep(waits[dsm]);
            }
            unsigned lo = 0;
            for (unsigned t = 0; t < 200; t++) {
                if (!g.getIn(4)) {
                    lo++;
                }
            }
            uint8_t b[68];
            hxPipeRead(&g, 0x30, b, 68);
            unsigned sum = 0, nz = 0;
            for (unsigned k = 0; k < 68; k++) {
                sum += b[k];
                if (b[k]) {
                    nz++;
                }
            }
            unsigned w0 = ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) |
                ((unsigned)b[2] << 8) | b[3];
            unsigned w1 = ((unsigned)b[4] << 24) | ((unsigned)b[5] << 16) |
                ((unsigned)b[6] << 8) | b[7];
            gLab2[dsm] = w0;
            gLab4[1 + dsm] = nz | (lo << 8);
            kprintf("TSP os%u w=%08x%08x sum=%u nz=%u lo=%u\n",
                dsm, w0, w1, sum, nz, lo);
        }
        gLab4[5] = hxReadU32(&g, 0x10000700u);
        gT2 = 1;
        gTlm[11] = 1;
    }

    g.setMode(4, 0);
    g.setDirIn(4);
    int ids[10], xs[10], ys[10];
    unsigned frames = 0, reports = 0, contacts = 0;
    static unsigned lastInt;
    lastInt = g.getIn(4);
    {
        // One-shot read-latency probe plus breadcrumbs.
        uint64_t t0 = 0, t1 = 0;
        uint8_t lv[4] = { 0, 0, 0, 0 };
        __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
        hxRegRead(&g, 0x900000A8u, lv, 4);
        __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t1));
        uint64_t tf = 0;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(tf));
        kprintf("TSP ml t=%u/%u a8=%02x\n",
            tf ? (unsigned)((t1 - t0) * 1000000u / tf) : 0u, (unsigned)(t1 - t0), lv[0]);
    }
    const unsigned stylus = (hxReadU32(&g, 0x1000059Cu) >> 24) & 0xFFu;
    const unsigned infoSize = stylus ? 68u : 56u;
    kprintf("TSP sz styl=%u len=%u\n", stylus, infoSize);
    bool down = false;
    lastInt = g.getIn(4);
    for (;;) {
        uint8_t buf[128];
        for (unsigned k = 0; k < 128; k++) {
            buf[k] = 0;
        }
        // IRQ-gated pop: read one frame only while the INT pin is asserted
        // (active low), the way the vendor handler consumes the stack,
        // instead of hammering it between events.
        unsigned lvl = g.getIn(4);
        if (lvl != lastInt) {
            lastInt = lvl;
            gIntEdges++;
        }
        bool popped = false;
        if (lvl == 0) {
            unsigned s0 = 0;
            hxPipeRead(&g, 0x30, buf, infoSize);
            popped = true;
            for (unsigned k = 0; k < infoSize; k++) {
                s0 += buf[k];
            }
            if (!s0) {
                // diag333: the stack pipe yields all-zero frames, so also
                // fetch the DSRAM event window and use it when it carries data.
                uint8_t db[68];
                hxRegReadN(&g, 0x10000700u, db, 68);
                unsigned s1 = 0;
                for (unsigned k = 0; k < 68; k++) {
                    s1 += db[k];
                    buf[k] = db[k];
                }
                if (s1) {
                    gRnz++;
                }
            } else {
                gRnz++;
            }
        } else {
            busDelay(nullptr, 500);
        }
        // Mainline hx83112b/hx83102J event layout: the frame sums to 0 mod
        // 256, point count sits at len-16 (0xFF = none), x/y big endian.
        unsigned cks = 0;
        for (unsigned k = 0; k < infoSize; k++) {
            cks += buf[k];
        }
        unsigned count = 0;
        const uint8_t countByte = buf[infoSize - 16];
        if ((cks & 0xFF) == 0 && countByte != 0xFF) {
            const unsigned c = countByte & 0x0Fu;
            count = c <= maxPt ? c : 0;
        }
        // First frame that passes the full event validation: dump the raw 56
        // bytes once so the layout can be confirmed against the mainline one.
        static unsigned evDumped;
        if (!evDumped && (cks & 0xFF) == 0 && count > 0) {
            evDumped = 1;
            kprintf("TSP ev0 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                (unsigned)buf[0] | ((unsigned)buf[1] << 8) | ((unsigned)buf[2] << 16) | ((unsigned)buf[3] << 24),
                (unsigned)buf[4] | ((unsigned)buf[5] << 8) | ((unsigned)buf[6] << 16) | ((unsigned)buf[7] << 24),
                (unsigned)buf[8] | ((unsigned)buf[9] << 8) | ((unsigned)buf[10] << 16) | ((unsigned)buf[11] << 24),
                (unsigned)buf[12] | ((unsigned)buf[13] << 8) | ((unsigned)buf[14] << 16) | ((unsigned)buf[15] << 24),
                (unsigned)buf[16] | ((unsigned)buf[17] << 8) | ((unsigned)buf[18] << 16) | ((unsigned)buf[19] << 24),
                (unsigned)buf[20] | ((unsigned)buf[21] << 8) | ((unsigned)buf[22] << 16) | ((unsigned)buf[23] << 24),
                (unsigned)buf[24] | ((unsigned)buf[25] << 8) | ((unsigned)buf[26] << 16) | ((unsigned)buf[27] << 24),
                (unsigned)buf[28] | ((unsigned)buf[29] << 8) | ((unsigned)buf[30] << 16) | ((unsigned)buf[31] << 24),
                (unsigned)buf[32] | ((unsigned)buf[33] << 8) | ((unsigned)buf[34] << 16) | ((unsigned)buf[35] << 24),
                (unsigned)buf[36] | ((unsigned)buf[37] << 8) | ((unsigned)buf[38] << 16) | ((unsigned)buf[39] << 24),
                (unsigned)buf[40] | ((unsigned)buf[41] << 8) | ((unsigned)buf[42] << 16) | ((unsigned)buf[43] << 24),
                (unsigned)buf[44] | ((unsigned)buf[45] << 8) | ((unsigned)buf[46] << 16) | ((unsigned)buf[47] << 24),
                (unsigned)buf[48] | ((unsigned)buf[49] << 8) | ((unsigned)buf[50] << 16) | ((unsigned)buf[51] << 24),
                (unsigned)buf[52] | ((unsigned)buf[53] << 8) | ((unsigned)buf[54] << 16) | ((unsigned)buf[55] << 24),
                (unsigned)buf[56] | ((unsigned)buf[57] << 8) | ((unsigned)buf[58] << 16) | ((unsigned)buf[59] << 24),
                (unsigned)buf[60] | ((unsigned)buf[61] << 8) | ((unsigned)buf[62] << 16) | ((unsigned)buf[63] << 24));
        }
        unsigned n = 0;
        for (unsigned i = 0; i < count; i++) {
            const unsigned x = ((unsigned)buf[i * 4] << 8) | buf[i * 4 + 1];
            const unsigned y = ((unsigned)buf[i * 4 + 2] << 8) | buf[i * 4 + 3];
            if (x == 0 || y == 0 || x == 0xFFFFu || y == 0xFFFFu ||
                x > 16000u || y > 25600u) {
                continue;
            }
            ids[n] = (int)i;
            mapPoint(rotate, x, y, &xs[n], &ys[n]);
            n++;
        }
        if (n) {
            native_touch_publish(n, ids, xs, ys);
            reports++;
            if (!down) {
                contacts++;
                down = true;
            }
        } else if (down && popped) {
            native_touch_publish(0, ids, xs, ys);
            reports++;
            down = false;
        }
        if (frames < 3 || (frames % 1000) == 0) {
            kprintf("TSP tx f=%u n=%u cnt=%u p0=%d,%d raw=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                frames, n, count, n ? xs[0] : -1, n ? ys[0] : -1,
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        }
        frames++;
        gTxFrames = frames;
        gTxReports = reports;
        gTxLastN = n;
        gTxSink = native_touch_sink_ready;
        static unsigned tickAcc;
        if (++tickAcc >= 2000) {
            tickAcc = 0;
            gTkCnt++;
            gTlm[0] = frames;
            kprintf("TSP tk f=%u\n", frames);
            uint8_t ch[4] = { 0, 0, 0, 0 }, pt[4] = { 0, 0, 0, 0 }, ie[4] = { 0, 0, 0, 0 };
            uint8_t mk[4] = { 0, 0, 0, 0 }, a8[4] = { 0, 0, 0, 0 }, f0[4] = { 0, 0, 0, 0 };
            uint8_t id[4] = { 0, 0, 0, 0 }, buf2[128];
            hxRegRead(&g, 0x100004F4u, ch, 4);
            hxRegRead(&g, 0x100004F8u, pt, 4);
            hxRegRead(&g, 0x10000488u, ie, 4);
            hxRegRead(&g, 0x100004E8u, mk, 4);
            hxRegRead(&g, 0x900000A8u, a8, 4);
            hxRegRead(&g, 0x10007000u, f0, 4);
            hxPipeRead(&g, 0xD0, id, 4);
            unsigned sum = 0;
            for (unsigned i = 0; i < infoSize; i++) {
                sum += buf[i];
            }
            // Second read with the register prologue armed, in case the stack
            // pipe needs the continuous-burst mode re-sent.
            hxCmd(&g, 0x13, 0x31);
            hxCmd(&g, 0x0D, 0x10);
            hxPipeRead(&g, 0x30, buf2, infoSize);
            unsigned sum2 = 0;
            for (unsigned i = 0; i < infoSize; i++) {
                sum2 += buf2[i];
            }
            unsigned i1 = (g.rd(0x200) >> 4) & 1u;
            // A short stack read as a second experiment: if four bytes return
            // data where the full frame returns zeros, the length is the issue.
            uint8_t s4[8];
            hxPipeRead(&g, 0x30, s4, 4);
            // Fast burst: the stack may be a mailbox that empties quickly.
            unsigned nz = 0, maxSum = 0, firstW = 0, firstW2 = 0, excB = 0;
            unsigned cbW = 0, cbW2 = 0;
            unsigned edges = gIntEdges;
            {
                uint8_t b3[128];
                for (unsigned i = 0; i < 50; i++) {
                    hxPipeRead(&g, 0x30, b3, infoSize);
                    unsigned s = 0;
                    bool same = true;
                    for (unsigned k = 0; k < infoSize; k++) {
                        s += b3[k];
                        if (b3[k] != b3[0]) {
                            same = false;
                        }
                    }
                    if (s) {
                        nz++;
                        if (nz == 1) {
                            firstW = ((unsigned)b3[0] << 24) | ((unsigned)b3[1] << 16) |
                                ((unsigned)b3[2] << 8) | b3[3];
                            firstW2 = ((unsigned)b3[4] << 24) | ((unsigned)b3[5] << 16) |
                                ((unsigned)b3[6] << 8) | b3[7];
                        }
                        if (same && excB == 0) {
                            excB = b3[0] | 0x100u;
                        }
                        if (b3[maxPt * 4] != 0 && cbW == 0) {
                            cbW = ((unsigned)b3[0] << 24) | ((unsigned)b3[1] << 16) |
                                ((unsigned)b3[2] << 8) | b3[3];
                            cbW2 = ((unsigned)b3[4] << 24) | ((unsigned)b3[5] << 16) |
                                ((unsigned)b3[6] << 8) | b3[7];
                        }
                    }
                    if (s > maxSum) {
                        maxSum = s;
                    }
                }
            }
            // diag333: live sweep of the candidate DSRAM event windows, so a
            // real touch moves these statistics in the printed summary.
            {
                uint8_t w7[64];
                unsigned tnz = 0, td0 = 0, td1 = 0;
                for (unsigned c = 0; c < 3; c++) {
                    unsigned chunk = c < 2 ? 64u : 52u;
                    hxRegReadN(&g, 0x10000700u + c * 64u, w7, chunk);
                    for (unsigned k = 0; k < chunk; k++) {
                        if (w7[k]) {
                            tnz++;
                        }
                    }
                    if (c == 0) {
                        td0 = ((unsigned)w7[0] << 24) | ((unsigned)w7[1] << 16) |
                            ((unsigned)w7[2] << 8) | w7[3];
                        td1 = ((unsigned)w7[4] << 24) | ((unsigned)w7[5] << 16) |
                            ((unsigned)w7[6] << 8) | w7[7];
                    }
                }
                gLab5[0] = tnz;
                gLab5[1] = td0;
                gLab5[2] = td1;
                uint8_t w6[16];
                hxRegReadN(&g, 0x100006F0u, w6, 16);
                unsigned n6 = 0;
                for (unsigned k = 0; k < 16; k++) {
                    if (w6[k]) {
                        n6++;
                    }
                }
                gLab5[3] = n6;
                gLab5[4] = ((unsigned)w6[0] << 24) | ((unsigned)w6[1] << 16) |
                    ((unsigned)w6[2] << 8) | w6[3];
                uint8_t wa3[32];
                hxRegReadN(&g, 0x10000A00u, wa3, 32);
                unsigned na = 0;
                for (unsigned k = 0; k < 32; k++) {
                    if (wa3[k]) {
                        na++;
                    }
                }
                gLab5[5] = na;
                gLab5[6] = ((unsigned)wa3[0] << 24) | ((unsigned)wa3[1] << 16) |
                    ((unsigned)wa3[2] << 8) | wa3[3];
            }
            unsigned bestPipe = 0xFF, bestSum = 0;
            // Three-window probe of the 0x90060000 buffer: 0x00/0x40/0x80.
            {
                uint8_t wq[64];
                hxRegReadN(&g, 0x90060000u, wq, 56);
                unsigned sA = 0;
                for (unsigned i = 0; i < 56; i++) {
                    sA += wq[i];
                }
                unsigned cA = wq[52];
                unsigned kA = (sA & 0xFF) == 0 ? 1u : 0u;
                gAb[5] = ((unsigned)wq[0] << 24) | ((unsigned)wq[1] << 16) |
                    ((unsigned)wq[2] << 8) | wq[3];
                hxRegReadN(&g, 0x90060040u, wq, 56);
                unsigned sB = 0;
                for (unsigned i = 0; i < 56; i++) {
                    sB += wq[i];
                }
                unsigned cB = wq[52];
                unsigned kB = (sB & 0xFF) == 0 ? 1u : 0u;
                hxRegReadN(&g, 0x90060080u, wq, 56);
                unsigned sC = 0;
                for (unsigned i = 0; i < 56; i++) {
                    sC += wq[i];
                }
                unsigned cC = wq[52];
                unsigned kC = (sC & 0xFF) == 0 ? 1u : 0u;
                gAb[0] = sA;
                gAb[1] = sB;
                gAb[2] = sC;
                gAb[3] = (cA & 0xFF) | ((kA & 1u) << 8) | (((cB & 0xFF) | ((kB & 1u) << 8)) << 16);
                gAb[4] = cC | ((kC & 1u) << 8);
            }
            // Variant A: HX83100-official path, AHB window at 0x90060000.
            unsigned sumA = 0, firstA = 0;
            {
                uint8_t wa[128];
                hxCmd(&g, 0x13, 0x31);
                hxCmd(&g, 0x0D, 0x10);
                hxRegReadN(&g, 0x90060000u, wa, infoSize);
                for (unsigned i = 0; i < infoSize; i++) {
                    sumA += wa[i];
                }
                firstA = ((unsigned)wa[0] << 24) | ((unsigned)wa[1] << 16) |
                    ((unsigned)wa[2] << 8) | wa[3];
            }
            // Variant B: HX83112 path, burst off, pipe 0x30, burst on.
            unsigned sumB = 0, firstB = 0;
            {
                uint8_t wb[128];
                hxCmd(&g, 0x13, 0x00);
                hxCmd(&g, 0x0D, 0x00);
                hxPipeRead(&g, 0x30, wb, infoSize);
                hxCmd(&g, 0x13, 0x31);
                hxCmd(&g, 0x0D, 0x10);
                for (unsigned i = 0; i < infoSize; i++) {
                    sumB += wb[i];
                }
                firstB = ((unsigned)wb[0] << 24) | ((unsigned)wb[1] << 16) |
                    ((unsigned)wb[2] << 8) | wb[3];
            }
            // One-shot post-boot experiments from the vendor table: idle
            // disable (para 0x17 -> 0x10000488), PSL reset (0x900000A0), and
            // the 8-byte ctrl_fw zeroing the vendor reset uses.
            if (!gT2) {
                uint8_t mode[4] = { 0, 0, 0, 0 };
                hxRegRead(&g, 0x10000488u, mode, 4);
                unsigned modeOld = mode[0];
                mode[0] = 0x17;
                hxRegWrite(&g, 0x10000488u, mode, 4);
                hxRegWriteU32(&g, 0x900000A0u, 0x00000000u);
                uint8_t z8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                hxRegWrite(&g, 0x9000005Cu, z8, 8);
                hxCmd(&g, 0x13, 0x31);
                hxCmd(&g, 0x0D, 0x10);
                gT2 = 1;
                kprintf("TSP ti2 m=%02x\n", modeOld);
            }
            // The FW drops from 0x05 (running) to 0x01 on its own. Re-run the
            // sense_on tail once when that happens and see if it revives.
            if (gTlm[11] == 0 && a8[0] != 0x05 && a8[0] != 0x0C) {
                uint8_t ra8[4] = { 0, 0, 0, 0 };
                unsigned rl = 0;
                hxRegWriteU32(&g, 0x10000100u, 0x00009AA9u);
                hxRegWriteU32(&g, 0x100006ECu, 0x00000000u);
                hxRegWriteU32(&g, 0x10000104u, 0x00000000u);
                hxRegWriteU32(&g, 0x10000694u, 0x00000001u);
                hxRegWriteU32(&g, 0x100006C0u, 0x00000000u);
                hxRegWriteU32(&g, 0x9000005Cu, 0x00000000u);
                busDelay(nullptr, 10);
                for (unsigned k = 0; k < 6; k++) {
                    uint8_t off[4] = { 0xF2, 0x31, 0x27, 0x95 };
                    busXfer(&g, off, nullptr, 4);
                    busDelay(nullptr, 20);
                    uint8_t off0[3] = { 0xF2, 0x31, 0x00 };
                    busXfer(&g, off0, nullptr, 3);
                    busDelay(nullptr, 10);
                    uint8_t fe[4] = { 0, 0, 0, 0 };
                    hxRegRead(&g, 0x900000E4u, fe, 4);
                    if (fe[0] == 0x00 && fe[1] == 0x02) {
                        rl = 1;
                        break;
                    }
                }
                hxCmd(&g, 0x13, 0x31);
                hxCmd(&g, 0x0D, 0x10);
                {
                    uint8_t v2[4] = { 0, 0, 0, 0 };
                    for (unsigned k = 0; k < 20; k++) {
                        hxRegWriteU32(&g, 0x90000048u, 0x000000ECu);
                        busDelay(nullptr, 1);
                        hxRegRead(&g, 0x90000048u, v2, 4);
                        if (v2[0] == 0xEC && v2[1] == 0x01) {
                            rl |= 0x200u;
                            break;
                        }
                    }
                }
                hxRegRead(&g, 0x900000A8u, ra8, 4);
                gTlm[11] = 0x400u | (rl & 0x300u) | ra8[0];
                kprintf("TSP tk rl=%03x a8=%02x\n", gTlm[11], ra8[0]);
            }
            gTlm[0] = frames;
            gTlm[1] = count;
            gTlm[2] = sum;
            gTlm[3] = sum2;
            gTlm[4] = a8[0];
            gTlm[5] = ((unsigned)ch[2] << 8) | ch[3];
            gTlm[6] = pt[0];
            gTlm[7] = (maxSum << 16) | (nz & 0xFFFFu);
            gTlm[8] = (bestPipe << 16) | (bestSum > 0xFFFFu ? 0xFFFFu : bestSum);
            gTlm[13] = firstW;
            gTlm[14] = firstW2;
            gTlm[15] = excB;
            gTlm[9] = i1;
            gTlm[10] = cbW;
            gTlm[12] = edges;
            gTlm[5] = gCpol | (gCpha << 1) | (gRxAt << 2) | (gShift << 4);
            gTlm[13] = (sumA << 16) | (firstA & 0xFFFFu);
            gTlm[14] = (sumB << 16) | (firstB & 0xFFFFu);
            gTlm[15] = firstA;
            kprintf("TSP tq f=%u cb=%02x sm=%u/%u a8=%02x kh=%03x a8h=%08x rx=%u tx=%u pt=%u i1=%u\n",
                frames, count, sum, sum2, a8[0], gTlm[11], gTlm[12], ch[2], ch[3], pt[0], i1);
            kprintf("TSP tq2 raw=%02x%02x%02x%02x%02x%02x%02x%02x b=%02x%02x%02x%02x%02x%02x%02x%02x f0=%02x%02x%02x%02x\n",
                gPipeRaw[0], gPipeRaw[1], gPipeRaw[2], gPipeRaw[3],
                gPipeRaw[4], gPipeRaw[5], gPipeRaw[6], gPipeRaw[7],
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                f0[0], f0[1], f0[2], f0[3]);
        }
        IOSleep(1);
    }
}

static void startHimaxTouch(void) {
    thread_t thread = nullptr;
    if (kernel_thread_start(himaxTouchThread, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}

// Vendor-exact SPI200 flash flow with maximal status diagnostics: WREN under
// both status modes, block erase, an 0xFF page probe, an image page probe,
// window readbacks, then reset and the 0xA33A poll.
static void runFlashFlow(void) {
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
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    uint8_t st[4];
    gDw[0] = hxReadU32(&g, 0x80000010u);
    gDw[1] = hxReadU32(&g, 0x80000020u);
    gDw[2] = hxReadU32(&g, 0x80000024u);
    gDw[3] = hxReadU32(&g, 0x80000028u);
    gDw[4] = hxReadU32(&g, 0x8000002Cu);
    gDw[5] = hxReadU32(&g, 0x80000040u);
    kprintf("TSP ff d %08x/%08x/%08x/%08x/%08x/%08x\n",
        gDw[0], gDw[1], gDw[2], gDw[3], gDw[4], gDw[5]);
    // WREN then status under both modes.
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    busDelay(nullptr, 1);
    hxRegWriteU32(&g, 0x80000020u, 0x42000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    busDelay(nullptr, 1);
    hxRegRead(&g, 0x8000002Cu, st, 4);
    gDw[6] = 0x100u | st[0];
    hxRegWriteU32(&g, 0x80000020u, 0x42000003u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    busDelay(nullptr, 1);
    hxRegRead(&g, 0x8000002Cu, st, 4);
    gDw[7] = 0x100u | st[0];
    kprintf("TSP ff wren s0=%02x s3=%02x\n", gDw[6] & 0xFF, gDw[7] & 0xFF);
    // Block erase block 0, vendor sequence.
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000028u, 0x00000000u);
    hxRegWriteU32(&g, 0x80000020u, 0x67000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x000000D8u);
    busDelay(nullptr, 300);
    gDw[8] = 0;
    for (unsigned i = 0; i < 60; i++) {
        hxRegWriteU32(&g, 0x80000020u, 0x42000003u);
        hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
        busDelay(nullptr, 1);
        hxRegRead(&g, 0x8000002Cu, st, 4);
        if ((st[0] & 1) == 0) {
            gDw[8] = i + 1;
            break;
        }
        busDelay(nullptr, 10);
    }
    kprintf("TSP ff er %u s=%02x\n", gDw[8], st[0]);
    // 0xFF page probe at page 0 (offset 0).
    static uint8_t ffpage[256];
    for (unsigned i = 0; i < 256; i++) {
        ffpage[i] = 0xFF;
    }
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000020u, 0x610FF000u);
    hxRegWriteU32(&g, 0x80000028u, 0x00000000u);
    hxRegWrite(&g, 0x8000002Cu, ffpage, 16);
    hxRegWriteU32(&g, 0x80000024u, 0x00000002u);
    hxRegWrite(&g, 0x8000002Cu, ffpage + 16, 240);
    hxWaitReady(&g, 1);
    gDw[9] = hxReadU32(&g, 0x80000010u);   // 0x10 held no flash data; keep slot
    // Image page probe at page 1 (offset 0x100).
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000020u, 0x610FF000u);
    hxRegWriteU32(&g, 0x80000028u, 0x00000100u);
    hxRegWrite(&g, 0x8000002Cu, gFwImg + 0x100, 16);
    hxRegWriteU32(&g, 0x80000024u, 0x00000002u);
    hxRegWrite(&g, 0x8000002Cu, gFwImg + 0x110, 240);
    hxWaitReady(&g, 1);
    // Window readbacks of both pages.
    unsigned fmis = 0, imis = 0;
    uint8_t rb[16];
    hxRegReadN(&g, 0x00000000u, rb, 16);
    for (unsigned i = 0; i < 16; i++) {
        if (rb[i] != 0xFF) {
            fmis++;
        }
    }
    gDw[10] = fmis;
    gDw[11] = (unsigned)rb[0] | ((unsigned)rb[1] << 8) | ((unsigned)rb[2] << 16) | ((unsigned)rb[3] << 24);
    hxRegReadN(&g, 0x00000100u, rb, 16);
    for (unsigned i = 0; i < 16; i++) {
        if (rb[i] != gFwImg[0x100 + i]) {
            imis++;
        }
    }
    gDw[12] = imis;
    gDw[13] = (unsigned)rb[0] | ((unsigned)rb[1] << 8) | ((unsigned)rb[2] << 16) | ((unsigned)rb[3] << 24);
    // Reset and poll the FW handshake.
    hardwareReset(&g);
    busDelay(nullptr, 300);
    gDw[14] = 0;
    for (unsigned i = 1; i <= 25; i++) {
        uint8_t v[4];
        hxRegRead(&g, 0x10007000u, v, 4);
        gDw[15] = (unsigned)v[0] | ((unsigned)v[1] << 8);
        if (v[0] == 0xA3 && v[1] == 0x3A) {
            gDw[14] = i;
            break;
        }
        busDelay(nullptr, 10);
    }
    kprintf("TSP ff r d=%08x w=%02x/%02x e=%u s=%02x fm=%u fw=%08x im=%u iw=%08x a=%u h=%04x\n",
        gDw[0], gDw[6] & 0xFF, gDw[7] & 0xFF, gDw[8], st[0], gDw[10], gDw[11],
        gDw[12], gDw[13], gDw[14], gDw[15]);
    g.close();
}

// diag278: localize the dead SPI200 command channel. Write the transfer
// format first, then step single commands through both candidate entry
// registers (0x24 / 0x28) and sample the status port repeatedly, before and
// after the vendor power-on sequence.
static void runFlashFlow2(void) {
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
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    uint8_t st[4];
    // A: format word, then WREN via 0x24, sample status 4x.
    hxRegWriteU32(&g, 0x80000010u, 0x00020780u);
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000020u, 0x42000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    busDelay(nullptr, 2);
    hxRegRead(&g, 0x8000002Cu, st, 4);
    gDw[0] = 0x100u | st[0];
    // B: same status read again after a fresh format word (repeatability).
    hxRegWriteU32(&g, 0x80000010u, 0x00020780u);
    hxRegWriteU32(&g, 0x80000020u, 0x42000003u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    busDelay(nullptr, 2);
    hxRegRead(&g, 0x8000002Cu, st, 4);
    gDw[1] = 0x100u | st[0];
    // C: WREN through the address register instead of the command register.
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000028u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000020u, 0x42000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    busDelay(nullptr, 2);
    hxRegRead(&g, 0x8000002Cu, st, 4);
    gDw[2] = 0x100u | st[0];
    // D: vendor power-on sequence, then repeat the WREN/status probe.
    hxRegWriteU32(&g, 0x100006ECu, 0x00000000u);
    hxRegWriteU32(&g, 0x10000694u, 0x00000001u);
    hxRegWriteU32(&g, 0x100006C0u, 0x00000000u);
    busDelay(nullptr, 20);
    hxRegWriteU32(&g, 0x80000010u, 0x00020780u);
    hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
    hxRegWriteU32(&g, 0x80000020u, 0x42000000u);
    hxRegWriteU32(&g, 0x80000024u, 0x00000005u);
    busDelay(nullptr, 2);
    hxRegRead(&g, 0x8000002Cu, st, 4);
    gDw[3] = 0x100u | st[0];
    // E: window head check for reference.
    uint8_t rb[16];
    hxRegReadN(&g, 0x00000000u, rb, 16);
    gDw[4] = (unsigned)rb[0] | ((unsigned)rb[1] << 8) | ((unsigned)rb[2] << 16) | ((unsigned)rb[3] << 24);
    gDw[5] = hxReadU32(&g, 0x100006C0u);
    kprintf("TSP f2 a=%02x b=%02x c=%02x d=%02x w=%08x c6=%08x\n",
        gDw[0] & 0xFF, gDw[1] & 0xFF, gDw[2] & 0xFF, gDw[3] & 0xFF, gDw[4], gDw[5]);
    g.close();
}

// The vendor never lets the flash bootloader start the image by itself: every
// boot re-programmes the 261120-byte himax_boe_fw.bin through the SPI200
// controller (chip erase 0xC7, then 256-byte pages: WREN 0x06, control
// 0x610FF000, page word (off>>8)<<8, data port 0x8000002C, program 0x02), then
// resets the IC and polls 0xA33A at 0x10007000.
static void runFwDownload(void) {
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
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    hxRegWriteU32(&g, 0x80000040u, 0x00000201u);
    hxRegWriteU32(&g, 0x80000010u, 0x00020780u);
    gDw[0] = 1;
    for (unsigned blk = 0; blk < gFwImgLen; blk += 0x10000) {
        hxRegWriteU32(&g, 0x80000020u, 0x47000000u);
        hxRegWriteU32(&g, 0x80000024u, 0x00000006u);
        hxRegWriteU32(&g, 0x80000028u, blk);
        hxRegWriteU32(&g, 0x80000020u, 0x67000000u);
        hxRegWriteU32(&g, 0x80000024u, 0x000000D8u);
        busDelay(nullptr, 300);
        if (!hxWaitReady(&g, 100)) {
            gDw[0] = 2;
            break;
        }
    }
    {
        uint8_t eb[16];
        hxRegReadN(&g, 0x00000000u, eb, 16);
        kprintf("TSP dl er %02x%02x%02x%02x%02x%02x%02x%02x\n",
            eb[0], eb[1], eb[2], eb[3], eb[4], eb[5], eb[6], eb[7]);
    }
    // Single-page experiment: write page 0, then read it back immediately.
    bool p0ok = hxProgramPage(&g, 0);
    {
        uint8_t t[16];
        hxRegReadN(&g, 0x00000000u, t, 16);
        kprintf("TSP p0 %u %02x%02x%02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x%02x%02x\n",
            p0ok ? 1u : 0u, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
            t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
    }
    unsigned pages = 0, gate_fail = 0;
    kprintf("TSP dl erase=%u img=%u\n", gDw[0], gFwImgLen);
    for (unsigned off = 0; off < gFwImgLen; off += 256) {
        if (!hxProgramPage(&g, off)) {
            gate_fail++;
        }
        pages++;
        if ((pages % 256) == 0) {
            kprintf("TSP dl p=%u\n", pages);
        }
    }
    gDw[1] = pages;
    gDw[2] = gate_fail;
    // Read the whole image back through the plain flash window and compare
    // it with the source buffer; blank bytes are counted separately.
    uint32_t mism = 0, first_off = 0xffffffffu, zeros = 0, ffs = 0;
    for (unsigned off = 0; off < gFwImgLen; off += 96) {
        uint8_t rb[96];
        unsigned n = gFwImgLen - off > 96 ? 96 : gFwImgLen - off;
        hxRegReadN(&g, off, rb, n);
        for (unsigned i = 0; i < n; i++) {
            if (rb[i] == 0x00) {
                zeros++;
            } else if (rb[i] == 0xFF) {
                ffs++;
            }
            if (rb[i] != gFwImg[off + i]) {
                mism++;
                if (first_off == 0xffffffffu) {
                    first_off = off + i;
                }
            }
        }
    }
    gDw[3] = mism;
    gDw[4] = first_off;
    gDw[8] = zeros;
    gDw[9] = ffs;
    hxRegReadN(&g, 0x00000000u, gDrb, 16);
    hxRegReadN(&g, 0x00001000u, gDr2, 16);
    // Let the fresh image start: reset and watch for the FW handshake.
    hardwareReset(&g);
    busDelay(nullptr, 300);
    gDw[5] = 0;
    for (unsigned i = 0; i < 25; i++) {
        uint8_t v[4];
        hxRegRead(&g, 0x10007000u, v, 4);
        for (int k = 0; k < 4; k++) {
            gFs[0][k] = v[k];
        }
        if (v[0] == 0xA3 && v[1] == 0x3A) {
            gDw[5] = i + 1;
            break;
        }
        busDelay(nullptr, 10);
    }
    hxRegRead(&g, 0x10007004u, gFs[1], 4);
    hxRegRead(&g, 0x10007010u, gFs[2], 4);
    hxRegRead(&g, 0x90000048u, gFs[3], 4);
    gDw[10] = (unsigned)gFs[1][0] | ((unsigned)gFs[1][1] << 8);
    kprintf("TSP dl e=%u p=%u g=%u mx=%u mo=%08x a=%u st=%02x g0=%03x z=%u f=%u\n",
        gDw[0], gDw[1], gDw[2], gDw[3], gDw[4], gDw[5], gDw[7], gDw[6], gDw[8], gDw[9]);
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

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

// SPI3 hardware controller, following the x9 port (spi-mt65xx register set):
// SMC infra request for the bus, pinmux to function 2, software-CS FIFO
// transfers of up to 32 bytes per packet with CS held across packets.
static volatile uint32_t *s3;
static uint32_t s3cmd;

static void s3wr(unsigned off, uint32_t v) {
    s3[off / 4] = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t s3rd(unsigned off) { return s3[off / 4]; }

static int s3transfer(const uint8_t *tx, uint8_t *rx, unsigned length) {
    if (!length || length > 519) {
        return -1;
    }
    s3wr(0x18, s3cmd | (1u << 7));
    for (unsigned off = 0; off < length;) {
        unsigned n = length - off;
        if (n > 32) {
            n = 32;
        }
        s3wr(0x04, (s3rd(0x04) & 0xffu) | ((n - 1) << 16));
        s3wr(0x68, 0);
        for (unsigned i = 0; i < n; i += 4) {
            uint32_t v = 0;
            for (unsigned j = 0; j < 4 && i + j < n; j++) {
                v |= (uint32_t)(tx ? tx[off + i + j] : 0xff) << (j * 8);
            }
            s3wr(0x10, v);
        }
        s3wr(0x18, s3cmd | (1u << 7) | 1u);
        unsigned wait = 0;
        for (; wait < 5000 && !s3rd(0x20); wait++) {
            IODelay(1);
        }
        if (wait == 5000) {
            s3wr(0x18, s3cmd);
            return -2;
        }
        uint32_t status = s3rd(0x1c);
        if (status & 2u) {
            s3wr(0x18, s3cmd);
            return -3;
        }
        for (unsigned i = 0; i < n; i += 4) {
            uint32_t v = s3rd(0x14);
            for (unsigned j = 0; j < 4 && i + j < n; j++) {
                if (rx) {
                    rx[off + i + j] = (uint8_t)(v >> (j * 8));
                }
            }
        }
        off += n;
    }
    s3wr(0x18, s3cmd);
    return 0;
}

static volatile unsigned gHwDone;
static void hwProbe(void) {
    if (gHwDone) {
        return;
    }
    gHwDone = 1;
    vm_offset_t base = ml_io_map(0x11013000ULL, 0x1000);
    vm_offset_t inf = ml_io_map(0x10001000ULL, 0x1000);
    if (!base || !inf) {
        kprintf("TSP hw nomap\n");
        return;
    }
    s3 = reinterpret_cast<volatile uint32_t *>(base);
    volatile uint32_t *inao = reinterpret_cast<volatile uint32_t *>(inf);
    Gpio g;
    if (!g.open()) {
        kprintf("TSP hw nogpio\n");
        return;
    }
    unsigned modeBefore = g.rd(0x380);
    unsigned gateBefore = inao[0xa8 / 4];
    unsigned id0 = s3rd(0x00), st0 = s3rd(0x20);
    // ruby (ruby-ios-bringup/touch): the SPI3 gate is active-low; writing the
    // bit to the CLR register at INFRA_AO+0xa8 enables the clock.
    inao[0xa8 / 4] = 1u << 10;
    IODelay(5);
    unsigned gateAfter = inao[0xa8 / 4];
    unsigned id1 = s3rd(0x00), st1 = s3rd(0x20);
    // Pads 68..71 -> SPI3 function 2 (stock DTS: 0x4402 0x4502 0x4602 0x4702).
    g.wr(0x380, (modeBefore & 0x0000ffffu) | 0x22220000u);
    IODelay(2);
    unsigned modeAfter = g.rd(0x380);
    s3cmd = s3rd(0x18) & ~((1u << 7) | (1u << 6));
    s3wr(0x00, 0x003f003fu);
    s3wr(0x04, 63);
    s3wr(0x28, 0x001f001fu);
    s3wr(0x24, 1);
    s3wr(0x18, s3cmd);
    unsigned rb = 0, c6 = 0, ok = 0;
    gHwXfer = s3transfer;
    {
        uint8_t pat[4] = { 0x5A, 0xA5, 0x3C, 0xC3 }, got[4] = { 0, 0, 0, 0 };
        hxRegWrite(&g, 0x20000040u, pat, 4);
        hxRegReadN(&g, 0x20000040u, got, 4);
        rb = (unsigned)got[0] | ((unsigned)got[1] << 8) | ((unsigned)got[2] << 16) |
            ((unsigned)got[3] << 24);
        uint8_t c[4] = { 0, 0, 0, 0 };
        hxRegRead(&g, 0x100006C0u, c, 4);
        c6 = ((unsigned)c[0] << 8) | c[1];
        ok = (c[0] == 0xC0 && c[1] == 0x72) ? 1u : 0u;
    }
    gHwXfer = nullptr;
    // Hand the pads back to GPIO mode for the proven bit-banged path.
    g.wr(0x380, modeBefore);
    g.setDirOut(kCs);
    g.setOut(kCs, 1);
    g.close();
    gTlm[6] = ok;
    kprintf("TSP hw g=%08x/%08x s=%08x/%08x id=%08x/%08x p=%08x rb=%08x c6=%04x ok=%u\n",
        gateBefore, gateAfter, st0, st1, id0, id1, modeAfter, rb, c6, ok);
}

static void runSpiHimax(void) {
    Gpio g;
    vm_offset_t base = ml_io_map(0x11013000ULL, 0x1000);
    vm_offset_t inf = ml_io_map(0x10001000ULL, 0x1000);
    vm_offset_t top = ml_io_map(0x10000000ULL, 0x1000);
    if (!base || !inf || !top) {
        kprintf("TSP sp nomap\n");
        return;
    }
    s3 = reinterpret_cast<volatile uint32_t *>(base);
    volatile uint32_t *inao = reinterpret_cast<volatile uint32_t *>(inf);
    volatile uint32_t *tcg = reinterpret_cast<volatile uint32_t *>(top);
    // Register sweeps: is the block readable at all, and what state is present.
    for (int i = 0; i < 8; i++) {
        gSpi[i] = inao[i];          // INFRA_AO 0x00..0x1C
    }
    for (int i = 0; i < 8; i++) {
        gSpi[8 + i] = tcg[i * 4];   // TOPCKGEN 0x00..0x7C
    }
    gSpi[16] = tcg[0x198 / 4];
    gSpi[17] = tcg[0x19c / 4];
    gSpi[18] = s3rd(0x00);
    gSpi[19] = s3rd(0x18);
    // Try both candidate gate banks and the two top bits of each.
    inao[0x10 / 4] = 1u << 30;
    IODelay(2);
    gSpi[20] = inao[0x18 / 4];
    inao[0x20 / 4] = 1u << 30;
    IODelay(2);
    gSpi[21] = inao[0x28 / 4];
    inao[0x10 / 4] = 1u << 31;
    IODelay(2);
    gSpi[22] = inao[0x18 / 4];
    inao[0x20 / 4] = 1u << 31;
    IODelay(2);
    gSpi[23] = inao[0x28 / 4];
    // Also poke the clk_spi3 mux update bit next to its selector.
    uint32_t mux = tcg[0x198 / 4];
    tcg[0x198 / 4] = mux | (1u << 19);
    IODelay(2);
    gSpi[24] = tcg[0x198 / 4];
    gSpi[25] = s3rd(0x00);
    gSpi[26] = s3rd(0x18);
    gSpi[27] = s3rd(0x28);
    kprintf("TSP sp ia=%08x/%08x/%08x/%08x g1=%08x g2=%08x g3=%08x g4=%08x\n",
        gSpi[0], gSpi[1], gSpi[2], gSpi[3], gSpi[20], gSpi[21], gSpi[22], gSpi[23]);
    kprintf("TSP sp tc=%08x/%08x/%08x/%08x m=%08x/%08x s=%08x/%08x/%08x\n",
        gSpi[8], gSpi[9], gSpi[10], gSpi[11], gSpi[16], gSpi[24],
        gSpi[25], gSpi[26], gSpi[27]);
    if (!g.open()) {
        return;
    }
    g.wr(0x380, (g.rd(0x380) & 0x0000ffffu) | 0x22220000u);
    gSpi[28] = g.rd(0x380);
    uint32_t cmd = s3rd(0x18);
    s3cmd = cmd & ~((1u << 7) | (1u << 6));
    s3wr(0x00, 0x003f003fu);
    s3wr(0x04, 63);
    s3wr(0x28, 0x001f001fu);
    s3wr(0x24, 1);
    s3wr(0x18, s3cmd);
    uint8_t tx[8];
    uint8_t rx[8];
    tx[0] = 0xF3;
    tx[1] = 0xD0;
    for (int i = 2; i < 7; i++) {
        tx[i] = 0;
    }
    int rc = s3transfer(tx, rx, 7);
    for (int i = 0; i < 4; i++) {
        gHid[i] = rx[3 + i];
    }
    gSpi[29] = s3rd(0x18);
    gSpi[30] = s3rd(0x1c);
    gSpi[31] = s3rd(0x20);
    gSpi[32] = (uint32_t)rc;
    g.wr(0x380, g.rd(0x380) & 0x0000ffffu);
    g.close();
    kprintf("TSP sp p=%08x a=%08x/%08x/%08x rc=%u id=%02x%02x%02x%02x\n",
        gSpi[28], gSpi[29], gSpi[30], gSpi[31], gSpi[32],
        gHid[0], gHid[1], gHid[2], gHid[3]);
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

// Display bring-up state from the legacy display archive, for the console line.
extern "C" unsigned native_legacy_probe_dump(char *, unsigned);

// ZF phase state for the UFS heartbeat line (weak-imported there).
extern "C" unsigned tb365fc_zf_dump(char *out, unsigned cap) {
    return (unsigned)snprintf(out, cap, "zph=%u fw=%08x cf=%08x rl=%u c6=%04x up=%u w12=%u/%08x w14=%u/%08x t4=%08x fw0=%08x cf0=%08x c0=%08x a8=%08x hd=%08x",
        gZfPhase, gZf[3], gZf[4], gZf[5], gZf[6], gZf[7], gZf[12], gZf[13], gZf[14], gZf[15],
        gZf[20], gZf[18], gZf[19], gZf[16], gZf[17], gZf[21]);
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
    kprintf("TSP sp ia=%08x/%08x/%08x/%08x g1=%08x g2=%08x g3=%08x g4=%08x\n",
        gSpi[0], gSpi[1], gSpi[2], gSpi[3], gSpi[20], gSpi[21], gSpi[22], gSpi[23]);
    kprintf("TSP sp tc=%08x/%08x/%08x/%08x m=%08x/%08x s=%08x/%08x/%08x\n",
        gSpi[8], gSpi[9], gSpi[10], gSpi[11], gSpi[16], gSpi[24],
        gSpi[25], gSpi[26], gSpi[27]);
    kprintf("TSP sp p=%08x a=%08x/%08x/%08x rc=%u id=%02x%02x%02x%02x\n",
        gSpi[28], gSpi[29], gSpi[30], gSpi[31], gSpi[32],
        gHid[0], gHid[1], gHid[2], gHid[3]);
    kprintf("TSP rb %02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x\n",
        gRb[0], gRb[1], gRb[2], gRb[3], gRb[4], gRb[5],
        gRb[6], gRb[7], gRb[8], gRb[9], gRb[10], gRb[11]);
    kprintf("TSP eq n=%u v=%02x%02x%02x%02x s=%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x/%02x%02x%02x%02x\n",
        gEqN, gEq[0], gEq[1], gEq[2], gEq[3],
        gFs[0][0], gFs[0][1], gFs[0][2], gFs[0][3],
        gFs[1][0], gFs[1][1], gFs[1][2], gFs[1][3],
        gFs[2][0], gFs[2][1], gFs[2][2], gFs[2][3],
        gFs[3][0], gFs[3][1], gFs[3][2], gFs[3][3]);
    kprintf("TSP dl e=%u p=%u g=%u mx=%u mo=%08x a=%u st=%02x g0=%03x z=%u f=%u r0=%02x%02x%02x%02x r2=%02x%02x%02x%02x\n",
        gDw[0], gDw[1], gDw[2], gDw[3], gDw[4], gDw[5], gDw[7], gDw[6], gDw[8], gDw[9],
        gDrb[0], gDrb[1], gDrb[2], gDrb[3], gDr2[0], gDr2[1], gDr2[2], gDr2[3]);
    kprintf("TSP fi b=%08x x=%08x r=%08x w=%03x c10=%08x c40=%08x h10=%08x s=%02x\n",
        gDw[0], gDw[1], gDw[2], gDw[3], gDw[4], gDw[5], gDw[6], gDw[7]);
    kprintf("TSP w2 %08x %08x %08x %08x %08x %08x %08x sram=%08x t=%08x h=%08x\n",
        gDw[12], gDw[13], gDw[14], gDw[15], gDw[16], gDw[17], gDw[18],
        gDw[10], gDw[11], gDw[19]);
    kprintf("TSP s0 mx=%u mo=%08x\n", gDw[20], gDw[21]);
    kprintf("TSP zf segs=%u by=%u mx=%u p=%u c6=%04x a=%u h=%04x w0=%08x\n",
        gDw[0], gDw[1], gDw[2], gDw[3], gDw[4], gDw[5], gDw[6], gDw[7]);
    kprintf("TSP zf2 pn=%u fs=%u cs=%u fw=%08x cf=%08x rl=%u c6=%04x up=%u\n",
        gZf[0], gZf[1], gZf[2], gZf[3], gZf[4], gZf[5], gZf[6], gZf[7]);
    // Keep the last-printed lines the ones worth reading: the big-text console
    // only keeps the newest rows, so zf2 (CRC/reload state) and st (phase) go
    // last and za just before them.
    {
        char dg[96];
        dg[0] = 0;
        native_legacy_probe_dump(dg, sizeof dg);
        kprintf("TSP z3 t4=%08x fw0=%08x cf0=%08x c0=%08x a8=%08x hd=%08x w12=%u/%08x w14=%u/%08x\n",
            gZf[20], gZf[18], gZf[19], gZf[16], gZf[17], gZf[21],
            gZf[12], gZf[13], gZf[14], gZf[15]);
        kprintf("TSP z4 pre=%u/%08x,%u/%08x post=%u/%08x,%u/%08x\n",
            gP[0], gP[1], gP[2], gP[3], gP[4], gP[5], gP[6], gP[7]);
        kprintf("TSP z5 m=%u/%u/%u/%u/%u f=%08x/%08x\n",
            gQ[0], gQ[1], gQ[2], gQ[3], gQ[4], gQ[5], gQ[6]);
        kprintf("TSP za m0=%u/%08x m1=%u/%08x m2=%u/%08x\n",
            gR[0], gR[1], gR[2], gR[3], gR[4], gR[5]);
        kprintf("TSP zf2 pn=%u fs=%u cs=%u fw=%08x cf=%08x rl=%u c6=%04x up=%u ra=%x/%04x\n",
            gZf[0], gZf[1], gZf[2], gZf[3], gZf[4], gZf[5], gZf[6], gZf[7],
            gZf[22], gZf[23]);
        kprintf("TSP tq f=%u cb=%02x a8=%02x ie=%u m=%02x fb=%u/%u f8=%08x\n",
            gTlm[0], gTlm[1], gTlm[4], gTlm[12], gTlm[5],
            gTlm[7] & 0xFFFFu, gTlm[7] >> 16, gTlm[15]);
        kprintf("TSP ab s=%u/%u/%u c=%02x/%02x k=%u/%u c3=%02x k3=%u f8=%08x\n",
            gAb[0], gAb[1], gAb[2], gAb[3] & 0xFF, (gAb[3] >> 16) & 0xFF,
            (gAb[3] >> 8) & 1u, (gAb[3] >> 24) & 1u, gAb[4] & 0xFF, (gAb[4] >> 8) & 1u, gAb[5]);
        kprintf("TSP lab1 v=%08x cf=%08x vd=%08x cu=%08x pr=%08x fs=%08x dm=%08x me=%08x psl=%08x\n",
            gLab1[0], gLab1[1], gLab1[2], gLab1[3], gLab1[4], gLab1[5],
            gLab1[6], gLab1[7], gLab1[8]);
        kprintf("TSP lab2 s=%08x/%08x/%08x/%08x\n",
            gLab2[0], gLab2[1], gLab2[2], gLab2[3]);
        kprintf("TSP lab4 nz=%u/%u/%u/%u lo=%u/%u/%u/%u d7=%08x\n",
            gLab4[1] & 0xFF, gLab4[2] & 0xFF, gLab4[3] & 0xFF, gLab4[4] & 0xFF,
            gLab4[1] >> 8, gLab4[2] >> 8, gLab4[3] >> 8, gLab4[4] >> 8,
            gLab4[5]);
        kprintf("TSP st ph=%u fw=%08x cf=%08x rl=%u c6=%04x up=%u st=%04x mp=%u tx=%u/%u n=%u s=%u tk=%u nz=%u %s\n",
            gZfPhase, gZf[3], gZf[4], gZf[5], gZf[6], gZf[7], gZf[9], gZf[10],
            gTxFrames, gTxReports, gTxLastN, gTxSink, gTkCnt, gRnz, dg);
    }
}

static void zfThreadMain(void *, wait_result_t) {
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
    IOSleep(10);
    g.setOut(kRst, 1);
    IOSleep(200);
    gCpol = 0;
    gCpha = 0;
    gMiOnes = 0;
    gXferN = 0;
    gTrace = false;
    const uint32_t addr = 0x20000040u;
    static const uint8_t pat[16] = { 0x5A, 0xA5, 0x3C, 0xC3, 0x0F, 0xF0, 0x96, 0x69, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint8_t rb[16];
    unsigned mis;
    gRxAt = 0;
    hxCmd(&g, 0x13, 0x31);
    hxCmd(&g, 0x0D, 0x10);
    hxRegWrite(&g, addr, pat, 4);
    hxRegReadN(&g, addr, rb, 4);
    mis = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (rb[i] != pat[i]) {
            mis++;
        }
    }
    gR[0] = mis;
    gR[1] = (unsigned)rb[0] | ((unsigned)rb[1] << 8) | ((unsigned)rb[2] << 16) | ((unsigned)rb[3] << 24);
    IOSleep(5);
    hxRegWrite(&g, addr, pat, 16);
    hxRegReadN(&g, addr, rb, 16);
    mis = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (rb[i] != pat[i]) {
            mis++;
        }
    }
    gR[2] = mis;
    gR[3] = (unsigned)rb[0] | ((unsigned)rb[1] << 8) | ((unsigned)rb[2] << 16) | ((unsigned)rb[3] << 24);
    IOSleep(5);
    hxRegReadN(&g, addr, rb, 16);
    mis = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (rb[i] != pat[i]) {
            mis++;
        }
    }
    gR[4] = mis;
    gR[5] = (unsigned)rb[0] | ((unsigned)rb[1] << 8) | ((unsigned)rb[2] << 16) | ((unsigned)rb[3] << 24);
    kprintf("TSP za m0=%u/%08x m1=%u/%08x m2=%u/%08x\n",
        gR[0], gR[1], gR[2], gR[3], gR[4], gR[5]);
    g.close();
    if (gR[0] == 0) {
        runZfDownload();
    } else {
        gZfPhase = 31;
    }
}
static unsigned gStage;

// Timeline after the probe: baseline attempt, pad readback, a traced repeat,
// then every SPI mode. One experiment per step, one bracket print per phase.
static void lateLoop(void *, wait_result_t) {
    bool padDone = false;
    for (unsigned i = 0; ; i++) {
        IOSleep(10000);
        unsigned t = (i + 1) * 10;
        if (gStage == 4) {
            // Run the firmware download only after userland has settled: the
            // bit-banged transfer would otherwise starve the single core and
            // stall the display client open.
            if (t == 600) {
                unsigned zfOn = 0;
                PE_parse_boot_argn("zf", &zfOn, sizeof zfOn);
                if (zfOn == 1) {
                    kprintf("tsp b600 zf\n");
                    thread_t zt = nullptr;
                    if (kernel_thread_start(zfThreadMain, nullptr, &zt) == KERN_SUCCESS) {
                        thread_deallocate(zt);
                    }
                } else {
                    kprintf("tsp b600 zf off\n");
                }
            } else if (t == 610) {
                startHimaxTouch();
            } else if (t >= 60) {
                printSummary(t);
            }
            continue;
        }
        if (t == 20) {
            kprintf("tsp late go\n");
        } else if (t == 40) {
            kprintf("tsp b40\n");
            runFlashFlow2();
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
            runReloadActive();
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
        } else if (t == 360) {
            kprintf("tsp b360\n");
            runReloadActive();
        } else {
            printSummary(t);
        }
    }
}

extern "C" void native_touch_spi_probe(void) {
    unsigned stage = 0;
    int found = PE_parse_boot_argn("touch", &stage, sizeof(stage));
    kprintf("tsp enter found=%d stage=%u\n", found, stage);
    if (!found || (stage != 3 && stage != 4)) {
        return;
    }
    gStage = stage;
    runPrep();
    thread_t t = nullptr;
    if (kernel_thread_start(lateLoop, nullptr, &t) == KERN_SUCCESS) {
        thread_deallocate(t);
    }
}
