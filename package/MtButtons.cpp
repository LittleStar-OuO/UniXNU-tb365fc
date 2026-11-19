// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB365FC physical buttons for NativeHidInput (boot argument buttons=1),
// published as the Consumer Power, Volume Increment and Volume Decrement bits
// of report 2 in drivers/ShengHidInput/NativeHidInput.cpp.
//
// The board facts come from the stock DTBO and the UEFI keypad driver that
// already runs on this tablet (uefi-tb365fc Platforms/Lenovo/tb365fcPkg
// Library/KeypadDeviceLib):
//
//   Volume Up    MediaTek KPD matrix bit 0 at 0x1C00E000. A released key
//                reads 1, a pressed key reads 0. KP_MEM1 is 0x04,
//                KP_DEBOUNCE 0x18 (0x400 ms) and KP_EN 0x24.
//   Volume Down  MT6377 TOPSTATUS bit 3 (HOME), held low while pressed.
//   Power        MT6377 TOPSTATUS bit 1 (PWRKEY), held low while pressed.
//
// TOPSTATUS is register 0x22 behind the MT6835 PMIC wrapper at 0x1C804000.
// The wrapper uses the SPMI (WACS2) path with arbitration capabilities:
// command 0x880, data-valid clear 0x8A4, read data 0x8A8, write data 0x884,
// result 0x894, slave id 4, FSM state (0x8A8 >> 1) & 7, idle 0, result ready 6.
// The same sequence is what PmicWrapperDxe in the UEFI firmware uses.
//
// The idle TOPSTATUS is sampled once; only a bit that idles high and then
// drops is reported, so an absent key can never read as held.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate(thread_t);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

extern "C" {
// package/TouchState.cpp owns the touch sink; this file owns the button one.
void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context);
// [0] stage (1 started, 2 KPD ready, 4 PMIC ready, negative: failure step),
// [1] polls, [2] published reports, [3] current mask, [4] last TOPSTATUS.
volatile int32_t tb365fc_buttons_stats[5];
}

namespace {

constexpr uint64_t kKeypadBase = 0x1C00E000ULL;
constexpr uint32_t kKpMem1 = 0x04;
constexpr uint32_t kKpDebounce = 0x18;
constexpr uint32_t kKpEnable = 0x24;
constexpr uint16_t kKpDebounceValue = 0x0400;

constexpr uint64_t kPmicBase = 0x1C804000ULL;
constexpr uint32_t kPwrapCmd = 0x880;
constexpr uint32_t kPwrapVldClear = 0x8A4;
constexpr uint32_t kPwrapReadData = 0x8A8;
constexpr uint32_t kPwrapResult = 0x894;
constexpr uint32_t kPwrapSlave = 4;
constexpr uint32_t kSpmiRead = 0xC0000000U;
constexpr uint32_t kPwrapIdle = 0;
constexpr uint32_t kPwrapResultReady = 6;

constexpr uint16_t kTopStatus = 0x22;
constexpr uint16_t kTopPowerKey = 1u << 1;
constexpr uint16_t kTopHomeKey = 1u << 3;

// Report 2 bits (NativeHidInput): bit 0 Menu/Home, 1 Power, 2 Volume up, 3 Volume down.
constexpr uint32_t kReportPower = 1u << 1;
constexpr uint32_t kReportVolumeUp = 1u << 2;
constexpr uint32_t kReportVolumeDown = 1u << 3;

volatile uint8_t *gKeypad;
volatile uint8_t *gPmic;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;
volatile uint32_t gMask;
bool gPmicUsable;
uint16_t gPmicBaseline;
bool gPmicBaselineValid;
unsigned gPmicFailures;

uint16_t keypadRead(unsigned offset) {
    uint16_t value = *reinterpret_cast<volatile uint16_t *>(gKeypad + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

void keypadWrite(unsigned offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t *>(gKeypad + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

uint32_t pwrapRead(unsigned offset) {
    return *reinterpret_cast<volatile uint32_t *>(gPmic + offset);
}

void pwrapWrite(unsigned offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(gPmic + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

bool pwrapWait(unsigned fsm) {
    for (unsigned n = 0; n < 1000; n++) {
        if (((pwrapRead(kPwrapReadData) >> 1) & 7) == fsm) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

// One single-byte SPMI register read through the WACS2 arbiter.
bool pmicRead(uint16_t address, uint16_t *value) {
    if (!gPmicUsable) {
        return false;
    }
    if (((pwrapRead(kPwrapReadData) >> 1) & 7) == kPwrapResultReady) {
        pwrapWrite(kPwrapVldClear, 1);   // drop a transaction the bootloader left open
    }
    if (!pwrapWait(kPwrapIdle)) {
        return false;
    }
    pwrapWrite(kPwrapCmd, kSpmiRead | (kPwrapSlave << 24) | (address & 0xFFFF));
    if (!pwrapWait(kPwrapResultReady)) {
        pwrapWrite(kPwrapVldClear, 1);
        return false;
    }
    *value = uint16_t(pwrapRead(kPwrapResult) & 0xFFFF);
    pwrapWrite(kPwrapVldClear, 1);
    return true;
}

uint16_t pmicTopStatus() {
    uint16_t status = 0;
    if (!pmicRead(kTopStatus, &status)) {
        if (++gPmicFailures >= 50) {
            gPmicUsable = false;
        }
        return 0xFFFF;
    }
    gPmicFailures = 0;
    return status;
}

uint32_t pressedMask(uint16_t topStatus) {
    uint32_t mask = 0;
    // Volume Up is KPD matrix bit 0 (released reads 1).
    if (gKeypad && (keypadRead(kKpMem1) & 1u) == 0) {
        mask |= kReportVolumeUp;
    }
    if (topStatus != 0xFFFF && gPmicBaselineValid) {
        if ((gPmicBaseline & kTopPowerKey) && !(topStatus & kTopPowerKey)) {
            mask |= kReportPower;
        }
        if ((gPmicBaseline & kTopHomeKey) && !(topStatus & kTopHomeKey)) {
            mask |= kReportVolumeDown;
        }
    }
    return mask;
}

void publish(uint32_t mask) {
    gMask = mask;
    tb365fc_buttons_stats[3] = int32_t(mask);
    void (*sink)(void *, uint32_t) = gSink;
    __asm__ volatile("dmb ish" ::: "memory");
    if (sink) {
        sink(gSinkContext, mask);
        tb365fc_buttons_stats[2] = tb365fc_buttons_stats[2] + 1;
    }
}

void buttonsThread(void *, wait_result_t) {
    gKeypad = reinterpret_cast<volatile uint8_t *>(ml_io_map(kKeypadBase, 0x1000));
    gPmic = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPmicBase, 0x1000));
    if (!gKeypad && !gPmic) {
        tb365fc_buttons_stats[0] = -1;
        kprintf("btn map failed kpd=%p pmic=%p\n", (void *)gKeypad, (void *)gPmic);
        return;
    }
    tb365fc_buttons_stats[0] = 1;
    if (gKeypad) {
        keypadWrite(kKpDebounce, kKpDebounceValue);
        keypadWrite(kKpEnable, 1);
        tb365fc_buttons_stats[0] = tb365fc_buttons_stats[0] | 2;
    }
    gPmicUsable = gPmic != nullptr;
    if (gPmicUsable) {
        // Confirm the wrapper answers before treating a missing PMIC as a key.
        uint16_t probe = 0;
        if (pmicRead(kTopStatus, &probe)) {
            gPmicBaseline = probe;
            gPmicBaselineValid = true;
            tb365fc_buttons_stats[0] = tb365fc_buttons_stats[0] | 4;
            tb365fc_buttons_stats[4] = int32_t(probe);
            kprintf("btn top=%04x kpd=%04x stage=%d\n", probe,
                gKeypad ? keypadRead(kKpMem1) : 0xFFFF, tb365fc_buttons_stats[0]);
        } else {
            kprintf("btn pmic no answer, volume up only\n");
        }
    }
    uint32_t previous = 0, candidate = 0, stable = 0;
    unsigned logged = 0;
    for (;;) {
        const uint16_t top = gPmicUsable ? pmicTopStatus() : 0xFFFF;
        const uint32_t mask = pressedMask(top);
        tb365fc_buttons_stats[1] = tb365fc_buttons_stats[1] + 1;
        if (mask == candidate) {
            stable++;
        } else {
            candidate = mask;
            stable = 0;
        }
        if (stable >= 2 && candidate != previous) {
            previous = candidate;
            publish(candidate);
            if (logged < 32) {
                logged++;
                kprintf("btn %s%s%s\n",
                    (candidate & kReportPower) ? "pwr " : "",
                    (candidate & kReportVolumeUp) ? "vol+ " : "",
                    (candidate & kReportVolumeDown) ? "vol- " : "");
            }
        }
        IOSleep(10);
    }
}

}  // namespace

// NativeHidInput registers its sink here once its IOHIDDevice has started.
// A key already held at that moment is re-sent so the first report is not lost.
void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSink = nullptr;
    __asm__ volatile("dmb ish" ::: "memory");
    gSinkContext = context;
    __asm__ volatile("dmb ish" ::: "memory");
    gSink = sink;
    if (sink) {
        sink(context, gMask);
        tb365fc_buttons_stats[2] = tb365fc_buttons_stats[2] + 1;
    }
}

extern "C" void tb365fc_buttons_start(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    static bool started;
    if (started) {
        return;
    }
    started = true;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        tb365fc_buttons_stats[0] = -2;
    }
}
