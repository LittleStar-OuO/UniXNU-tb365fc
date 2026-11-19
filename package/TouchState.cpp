// SPDX-License-Identifier: BSD-2-Clause-Patent
// The finger set NativeHidInput publishes as its touch screen, written by the
// board touch producer under a sequence lock: the producer makes
// native_touch_seq odd, writes count, identifiers and positions (x 0..30480,
// y 0..20320, the vendor HX83122 report range), then makes it even again and
// calls native_touch_published.
//
// NativeHidInput sends a report when told a frame is out, not on a timer: it
// registers its sink here (native_touch_set_sink) and reads the snapshot
// through the sequence counter. The Himax runtime path publishes through
// native_touch_publish once its firmware is running; touch-selftest uses the
// same entry so the HID chain can be checked without the panel.
#include <IOKit/IOLib.h>
#include <stdint.h>

extern "C" {
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile uint32_t native_touch_sink_ready;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
void native_touch_set_sink(void (*sink)(void *), void *context);
void native_touch_published(void);
void native_touch_publish(unsigned count, const int *ids, const int *xs, const int *ys);
}

namespace {
void (*volatile gSink)(void *);
void *volatile gSinkContext;
IOLock *gPublishLock;
constexpr unsigned kFingers = 12;
}

void native_touch_set_sink(void (*sink)(void *), void *context) {
    gSink = nullptr;
    __asm__ volatile("dmb ish" ::: "memory");
    gSinkContext = context;
    __asm__ volatile("dmb ish" ::: "memory");
    gSink = sink;
    native_touch_sink_ready = sink ? 1 : 0;
}

void native_touch_published(void) {
    void (*sink)(void *) = gSink;
    __asm__ volatile("dmb ish" ::: "memory");
    if (sink) {
        sink(gSinkContext);
    }
}

// One coherent frame. Writers must exclude one another, or their increments can
// be lost and leave an odd sequence that the HID reader rejects for good.
void native_touch_publish(unsigned count, const int *ids, const int *xs, const int *ys) {
    if (count > kFingers) {
        count = kFingers;
    }
    if (!gPublishLock) {
        gPublishLock = IOLockAlloc();
    }
    if (gPublishLock) {
        IOLockLock(gPublishLock);
    }
    native_touch_seq = native_touch_seq + 1;   // odd: writing
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count; i++) {
        native_touch_ids[i] = ids ? ids[i] : int(i);
        native_touch_xs[i] = xs[i];
        native_touch_ys[i] = ys[i];
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;   // even: published
    if (gPublishLock) {
        IOLockUnlock(gPublishLock);
    }
    // The sink enters NativeHidInput's work loop. Never hold the writer lock
    // across this synchronous callback.
    native_touch_published();
}
