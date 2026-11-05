// SPDX-License-Identifier: BSD-2-Clause-Patent
// Persistent kernel console ring in the ShengUfs log window. The header is
// format 1 of UniXNU-sheng's package/BootLog.c, which scripts/read-sheng-log.py
// decodes: magic SHLOG001, block 0 header, blocks 1..16 the message ring.
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

extern "C" int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why) __attribute__((weak_import));
extern "C" int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src) __attribute__((weak_import));
extern "C" int native_ufs_log_sync(void) __attribute__((weak_import));

static const uint32_t kBlock = 4096;
static const uint32_t kMsgBlocks = 16;                 // 64 KiB of console text
static const uint32_t kMsgBytes = kMsgBlocks * kBlock;

static char gRing[kMsgBytes];
static volatile uint32_t gRingWrite, gRingUsed;
static uint8_t gOut[kBlock + kMsgBytes];
static uint64_t gBootId, gFirstLba;
static uint32_t gWindowBlocks, gRounds, gErrors;

struct boot_log_header {
    char magic[8];
    uint32_t version, header_bytes;
    uint64_t boot_id, first_lba;
    uint32_t window_blocks;
    uint32_t msg_first, msg_blocks, msg_bytes;
    uint32_t slot_first, slot_blocks, slot_count, slots_written;
    uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
    char tag[32];
};
static_assert(sizeof(boot_log_header) == 120, "reader header layout");

extern "C" void native_boot_log_vput(const char *fmt, va_list ap)
{
    char line[240];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }
    if (line[n - 1] != '\n') {
        line[n++] = '\n';
    }
    for (int i = 0; i < n; i++) {
        gRing[gRingWrite] = line[i];
        gRingWrite = (gRingWrite + 1) % kMsgBytes;
        if (gRingUsed < kMsgBytes) {
            gRingUsed++;
        }
    }
}

static void boot_log_snapshot(void)
{
    uint32_t used = gRingUsed;
    uint32_t start = (gRingWrite + kMsgBytes - used) % kMsgBytes;
    uint8_t *msg = gOut + kBlock;
    for (uint32_t i = 0; i < used; i++) {
        msg[i] = gRing[(start + i) % kMsgBytes];
    }
    for (uint32_t i = used; i < kMsgBytes; i++) {
        msg[i] = 0;
    }
    uint64_t absolute = 0, nanos = 0;
    clock_get_uptime(&absolute);
    absolutetime_to_nanoseconds(absolute, &nanos);

    boot_log_header h = {};
    memcpy(h.magic, "SHLOG001", 8);
    h.version = 1;
    h.header_bytes = sizeof(h);
    h.boot_id = gBootId;
    h.first_lba = gFirstLba;
    h.window_blocks = gWindowBlocks;
    h.msg_first = 1;
    h.msg_blocks = kMsgBlocks;
    h.msg_bytes = used;
    h.uptime_s = (uint32_t)(nanos / 1000000000ULL);
    h.rounds = gRounds;
    h.write_errors = gErrors;
    h.state = 1;
    memcpy(h.tag, "tb365fc", 8);

    if (native_ufs_log_write(1, kMsgBlocks, msg) != 1) {
        gErrors++;
    }
    memcpy(gOut, &h, sizeof(h));
    if (native_ufs_log_write(0, 1, gOut) != 1) {
        gErrors++;
    }
    if (native_ufs_log_sync) {
        native_ufs_log_sync();
    }
    gRounds++;
}

static void boot_log_thread(void *, wait_result_t)
{
    uint64_t first = 0, blocks = 0;
    const char *why = "";
    bool opened = false;
    for (int i = 0; i < 240; i++) {
        if (native_ufs_log_open && native_ufs_log_open(&first, &blocks, &why)) {
            opened = true;
            break;
        }
        IOSleep(500);
    }
    if (!opened || blocks == 0) {
        return;
    }
    gFirstLba = first;
    gWindowBlocks = (uint32_t)blocks;
    gBootId = mach_absolute_time();
    for (;;) {
        IOSleep(10 * 1000);
        boot_log_snapshot();
    }
}

extern "C" void native_boot_log_start(void)
{
    uint32_t on = 0;
    if (!PE_parse_boot_argn("sheng-log", &on, sizeof(on)) || on != 1) {
        return;
    }
    if (!native_ufs_log_open || !native_ufs_log_write) {
        return;
    }
    static bool started;
    if (started) {
        return;
    }
    started = true;
    thread_t thread;
    if (kernel_thread_start(boot_log_thread, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}
