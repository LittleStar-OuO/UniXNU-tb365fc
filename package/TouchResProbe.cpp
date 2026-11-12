// SPDX-License-Identifier: BSD-2-Clause-Patent
// Touch resource inspection: the input adaptation's first stage. Reads the
// SPI3 controller window, the GPIO/IOCFG banks and the EINT instance that the
// stock device tree assigns to the Novatek NT36532 node, without changing any
// state. The dump prints once at the late display probe and once more from a
// delayed thread, when the boot console has gone quiet, so the snapshot can be
// read back from the panel or the persistent BootLog ring.
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

namespace {

struct ResMap {
    IOMemoryDescriptor *desc;
    IOMemoryMap *map;
    volatile uint32_t *base;
    ResMap() : desc(nullptr), map(nullptr), base(nullptr) {}
    bool open(uint64_t pa, uint64_t len) {
        desc = IOMemoryDescriptor::withPhysicalAddress(pa, len, kIODirectionNone);
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
    ~ResMap() {
        if (map) {
            map->release();
        }
        if (desc) {
            desc->release();
        }
    }
};

void dumpWords(const char *tag, volatile uint32_t *p, unsigned first, unsigned words) {
    char line[160];
    for (unsigned w = 0; w < words; w += 8) {
        int n = snprintf(line, sizeof(line), "%s %03x:", tag, (first + w) * 4);
        for (unsigned i = 0; i < 8 && w + i < words; i++) {
            n += snprintf(line + n, sizeof(line) - n, " %08x", p[first + w + i]);
        }
        kprintf("%s\n", line);
    }
}

uint32_t iocfgRead(volatile uint32_t *bank, unsigned pin) {
    return bank[((pin >> 3) * 4) + (pin & 7)];
}

void dumpAll(void) {
    kprintf("trs begin\n");
    {
        ResMap spi;
        if (spi.open(0x11013000ULL, 0x100)) {
            dumpWords("trs spi", spi.base, 0, 16);
        } else {
            kprintf("trs spi no map\n");
        }
    }
    {
        ResMap gpio;
        if (gpio.open(0x10005000ULL, 0x1000)) {
            dumpWords("trs gpio", gpio.base, 0, 32);
        } else {
            kprintf("trs gpio no map\n");
        }
    }
    {
        static const struct {
            const char *name;
            uint64_t pa;
        } banks[] = {
            { "lm", 0x11b10000ULL }, { "rb", 0x11c40000ULL }, { "bl", 0x11d10000ULL },
            { "bm", 0x11d30000ULL }, { "br", 0x11d40000ULL }, { "lt", 0x11e30000ULL },
            { "rm", 0x11eb0000ULL }, { "rt", 0x11ec0000ULL },
        };
        static const unsigned pins[] = { 4, 43, 68, 69, 70, 71 };
        for (unsigned b = 0; b < sizeof(banks) / sizeof(banks[0]); b++) {
            ResMap m;
            if (!m.open(banks[b].pa, 0x400)) {
                kprintf("trs iocfg %s no map\n", banks[b].name);
                continue;
            }
            char line[160];
            int n = snprintf(line, sizeof(line), "trs iocfg %s", banks[b].name);
            for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
                n += snprintf(line + n, sizeof(line) - n, " p%u=%08x", pins[i], iocfgRead(m.base, pins[i]));
            }
            kprintf("%s\n", line);
        }
    }
    {
        ResMap eint;
        if (eint.open(0x11de0000ULL, 0x1000)) {
            dumpWords("trs eint2", eint.base, 0, 12);
        } else {
            kprintf("trs eint2 no map\n");
        }
    }
    kprintf("trs end\n");
}

void delayedDump(void *arg, wait_result_t) {
    unsigned seconds = 420;
    if (arg) {
        seconds = (unsigned)(uintptr_t)arg;
    }
    IOSleep(seconds * 1000);
    kprintf("trs late +%us\n", seconds);
    dumpAll();
}

}

extern "C" void native_touch_res_probe(void) {
    unsigned on = 0;
    if (!PE_parse_boot_argn("touch-res", &on, sizeof(on)) || on != 1) {
        return;
    }
    static bool started;
    if (started) {
        return;
    }
    started = true;
    dumpAll();
    unsigned delay = 0;
    if (!PE_parse_boot_argn("touch-res-delay", &delay, sizeof(delay)) || delay < 60) {
        delay = 420;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(delayedDump, (void *)(uintptr_t)delay, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
    thread_t again = nullptr;
    if (kernel_thread_start(delayedDump, (void *)(uintptr_t)(delay + 360), &again) == KERN_SUCCESS) {
        thread_deallocate(again);
    }
}
