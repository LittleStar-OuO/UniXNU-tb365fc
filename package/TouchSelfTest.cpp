// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synthetic touch frame source for the TB365FC (boot argument
// touch-selftest=<seconds>, default off). It pushes one rightward swipe
// through the same finger set and notification NativeHidInput uses for the
// real panel, so the OS-side touch chain (digitizer service, report
// descriptor, coordinate scaling and backboardd delivery) can be checked
// before the Himax firmware runs.
//
// Coordinates use the producer range NativeHidInput expects: x 0..30480,
// y 0..20320 over the 3048 x 2032 panel. On the first home screen page a
// rightward swipe opens the widget view, which is visible in a photo.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate(thread_t);

extern "C" {
void native_touch_publish(unsigned count, const int *ids, const int *xs, const int *ys);
extern volatile uint32_t native_touch_hid_reports;
extern volatile uint32_t native_touch_sink_ready;
extern volatile uint32_t native_touch_edge_stats[4];
extern volatile uint32_t native_hid_input_events[8];
extern volatile int native_hid_input_results[8];
extern volatile uint32_t native_legacy_transfer_calls;
// HidInputProbe.cpp: 1 asks NativeHidInput for one Home press and release.
extern volatile uint32_t native_hid_input_command;
void tb365fc_touch_selftest_start(void);
}

namespace {

constexpr int kXMax = 30480;
constexpr int kYMax = 20320;

void publishPoint(bool down, int x, int y) {
    const int id = 0;
    native_touch_publish(down ? 1 : 0, &id, &x, &y);
}

void logStatus(const char *tag) {
    kprintf("tst %s reports=%u res6=%d ev=%u/%u/%u/%u/%u/%u/%u/%u edge=%u/%u/%u/%u\n",
        tag, native_touch_hid_reports, native_hid_input_results[6],
        native_hid_input_events[0], native_hid_input_events[1],
        native_hid_input_events[2], native_hid_input_events[3],
        native_hid_input_events[4], native_hid_input_events[5],
        native_hid_input_events[6], native_hid_input_events[7],
        native_touch_edge_stats[0], native_touch_edge_stats[1],
        native_touch_edge_stats[2], native_touch_edge_stats[3]);
}

void selftestThread(void *arg, wait_result_t) {
    const unsigned seconds = unsigned(uintptr_t(arg));
    // The delay is measured from the first presented frames, not from the late
    // probe: iPadOS needs many minutes of userland boot before the home screen
    // accepts input.
    for (unsigned waited = 0; native_legacy_transfer_calls < 2 && waited < 600; waited++) {
        IOSleep(1000);
    }
    for (unsigned waited = 0; !native_touch_sink_ready && waited < 120; waited++) {
        IOSleep(1000);
    }
    IOSleep(seconds * 1000);
    kprintf("tst go after %us sink=%u frames=%u\n", seconds, native_touch_sink_ready,
        native_legacy_transfer_calls);
    // Wake the panel first: a sleeping or locked home screen ignores touches.
    native_hid_input_command = 1;
    IOSleep(5000);
    logStatus("before");
    const int y = kYMax / 2;
    const int first = kXMax / 20;
    const int last = kXMax - first;
    int x = first;
    publishPoint(true, x, y);
    for (unsigned i = 0; i < 40; i++) {
        x += (last - first) / 40;
        IOSleep(20);
        publishPoint(true, x, y);
    }
    publishPoint(false, x, y);
    IOSleep(1000);
    logStatus("done");
}

}  // namespace

extern "C" void tb365fc_touch_selftest_start(void) {
    unsigned seconds = 0;
    if (!PE_parse_boot_argn("touch-selftest", &seconds, sizeof(seconds)) || seconds < 5) {
        return;
    }
    static bool started;
    if (started) {
        return;
    }
    started = true;
    thread_t thread = nullptr;
    if (kernel_thread_start(selftestThread, reinterpret_cast<void *>(uintptr_t(seconds)), &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}
