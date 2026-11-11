// Minimal stage test: mount the pseudo filesystems, ask the bootloader for
// fastboot once, and reboot. Proves the LZ4 vendor ramdisk is used as initrd.
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef unsigned long size_t;

#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_lseek 62
#define SYS_mount 40
#define SYS_mkdirat 34
#define SYS_mknodat 33
#define SYS_nanosleep 101
#define SYS_reboot 142

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_RDWR 2

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}
static void sleep_ms(unsigned ms) { long ts[2] = { ms / 1000, (long)(ms % 1000) * 1000000L }; sys(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0); }
static int read_file(const char *path, char *buf, size_t cap)
{
    int fd = (int)sys(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return fd;
    long n = sys(SYS_read, fd, (long)buf, cap, 0, 0, 0);
    sys(SYS_close, fd, 0, 0, 0, 0, 0);
    return n <= 0 ? -1 : 0;
}
static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long crc = 0xffffffffUL;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
    }
    return crc ^ 0xffffffffUL;
}

static void bootloader_once(void)
{
    unsigned char block[4096];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = 0;
    const char cmd[] = "bootonce-bootloader";
    for (size_t i = 0; i < sizeof(cmd); i++) block[i] = (unsigned char)cmd[i];
    unsigned char *c = block + 2048;
    c[0] = '_'; c[1] = 'a';
    c[4] = 0x42; c[5] = 0x43; c[6] = 0x41; c[7] = 0x42;
    c[8] = 1; c[9] = 2; c[12] = 0xff;
    unsigned long crc = crc32_of(c, 28);
    for (int i = 0; i < 4; i++) c[28 + i] = (unsigned char)(crc >> (8 * i));

    char buf[64];
    if (read_file("/sys/block/sdc/dev", buf, sizeof(buf)) < 0) return;
    unsigned maj = 0, min = 0;
    const char *p = buf;
    while (*p >= '0' && *p <= '9') maj = maj * 10 + (unsigned)(*p++ - '0');
    if (*p == ':') p++;
    while (*p >= '0' && *p <= '9') min = min * 10 + (unsigned)(*p++ - '0');
    unsigned long dev = (min & 0xff) | ((unsigned long)(maj & 0xfff) << 8) | ((unsigned long)(min & ~0xffu) << 12);
    sys(SYS_mknodat, AT_FDCWD, (long)"/dev/sdc", 0060000 | 0666, dev, 0, 0);

    int fd = (int)sys(SYS_openat, AT_FDCWD, (long)"/dev/sdc", O_RDWR, 0, 0, 0);
    if (fd < 0) return;
    if (sys(SYS_lseek, fd, 8L * 4096L, 0, 0, 0, 0) < 0) { sys(SYS_close, fd, 0, 0, 0, 0, 0); return; }
    sys(SYS_write, fd, (long)block, sizeof(block), 0, 0, 0);
    sys(SYS_close, fd, 0, 0, 0, 0, 0);
    sys(SYS_reboot, 0xfee1dead, 672274793, 0x01234567, 0, 0, 0);
}

void _start(void)
{
    sys(SYS_mkdirat, AT_FDCWD, (long)"/proc", 0755, 0, 0, 0);
    sys(SYS_mkdirat, AT_FDCWD, (long)"/sys", 0755, 0, 0, 0);
    sys(SYS_mkdirat, AT_FDCWD, (long)"/dev", 0755, 0, 0, 0);
    sys(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0);
    sys(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0, 0);
    sys(SYS_mount, (long)"tmpfs", (long)"/dev", (long)"tmpfs", 0, 0, 0);
    sleep_ms(3000);
    bootloader_once();
    for (;;) sleep_ms(1000);
}
