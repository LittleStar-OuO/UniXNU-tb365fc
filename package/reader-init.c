// TB365FC USB log reader: pid 1 from a slot-b initramfs. Loads the stock
// Android modules, brings up the configfs ACM gadget and streams the ShengUfs
// log window (last 32 MiB of userdata) as base64, then writes the BCB
// "boot-fastboot" command and reboots to the bootloader.
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef unsigned long size_t;

#define SYS_openat 56
#define SYS_close 57
#define SYS_getdents64 61
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_mount 40
#define SYS_mkdirat 34
#define SYS_mknodat 33
#define SYS_symlinkat 36
#define SYS_nanosleep 101
#define SYS_reboot 142
#define SYS_finit_module 273
#define SYS_exit 93

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_NONBLOCK 0x800
#define S_IFBLK 0060000
#define S_IFCHR 0020000
#define MKNOD_MODE(m, t) ((m) | (t))

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

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static void memzero(void *p, size_t n) { unsigned char *c = p; while (n--) *c++ = 0; }

static int open_r(const char *p) { return (int)sys(SYS_openat, AT_FDCWD, (long)p, O_RDONLY, 0, 0, 0); }
static int open_w(const char *p, int flags) { return (int)sys(SYS_openat, AT_FDCWD, (long)p, flags | O_WRONLY, 0644, 0, 0); }
static int open_rw(const char *p) { return (int)sys(SYS_openat, AT_FDCWD, (long)p, O_RDWR, 0, 0, 0); }
static long write_fd(int fd, const void *b, size_t n) { return sys(SYS_write, fd, (long)b, n, 0, 0, 0); }
static long read_fd(int fd, void *b, size_t n) { return sys(SYS_read, fd, (long)b, n, 0, 0, 0); }
static void close_fd(int fd) { sys(SYS_close, fd, 0, 0, 0, 0, 0); }

static int write_file(const char *path, const char *text)
{
    int fd = open_w(path, 0);
    if (fd < 0) return fd;
    long r = write_fd(fd, text, slen(text));
    close_fd(fd);
    return r < 0 ? (int)r : 0;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    int fd = open_r(path);
    if (fd < 0) return fd;
    long n = read_fd(fd, buf, cap - 1);
    close_fd(fd);
    if (n < 0) return (int)n;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return (int)n;
}

static void sleep_ms(unsigned ms)
{
    struct { long sec; long nsec; } ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    sys(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
}

static void puts_fd(int fd, const char *s) { write_fd(fd, s, slen(s)); }
static void puts_num(int fd, unsigned long v, unsigned base)
{
    char tmp[32];
    int i = 0;
    if (!v) { write_fd(fd, "0", 1); return; }
    while (v) { unsigned d = v % base; tmp[i++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; }
    while (i--) write_fd(fd, &tmp[i], 1);
}
static void puts_hex(int fd, unsigned long v) { puts_num(fd, v, 16); }

static int mount_fs(const char *src, const char *tgt, const char *type, unsigned long flags)
{
    return (int)sys(SYS_mount, (long)src, (long)tgt, (long)type, flags, 0, 0);
}
static int make_dir(const char *p) { return (int)sys(SYS_mkdirat, AT_FDCWD, (long)p, 0755, 0, 0, 0); }
static int make_node(const char *p, int mode, unsigned major, unsigned minor)
{
    unsigned long dev = (minor & 0xff) | ((unsigned long)(major & 0xfff) << 8) | ((unsigned long)(minor & ~0xffu) << 12);
    return (int)sys(SYS_mknodat, AT_FDCWD, (long)p, mode, dev, 0, 0);
}
static int make_link(const char *tgt, const char *p) { return (int)sys(SYS_symlinkat, (long)tgt, AT_FDCWD, (long)p, 0, 0, 0); }

static long read_block_dev(const char *sysdir, unsigned *major, unsigned *minor)
{
    char path[128], buf[64];
    size_t n = slen(sysdir);
    if (n > 100) return -1;
    for (size_t i = 0; i <= n; i++) path[i] = sysdir[i];
    const char suffix[] = "/dev";
    for (size_t i = 0; i < sizeof(suffix); i++) path[n + i] = suffix[i];
    int r = read_file(path, buf, sizeof(buf));
    if (r <= 0) return -1;
    char *p = buf;
    unsigned long mj = 0, mn = 0;
    while (*p >= '0' && *p <= '9') mj = mj * 10 + (unsigned long)(*p++ - '0');
    if (*p != ':') return -1;
    p++;
    while (*p >= '0' && *p <= '9') mn = mn * 10 + (unsigned long)(*p++ - '0');
    *major = (unsigned)mj; *minor = (unsigned)mn;
    return 0;
}

#define MODDIR "/modules"

static int load_modules(char *failbuf, size_t failcap, unsigned *loaded, unsigned *failed)
{
    char list[8192];
    int n = read_file(MODDIR "/modules.load", list, sizeof(list));
    if (n <= 0) return -1;
    char path[160];
    size_t at = 0;
    *loaded = 0; *failed = 0;
    while (at < (size_t)n) {
        size_t start = at;
        while (at < (size_t)n && list[at] != '\n') at++;
        if (at > start) {
            size_t len = at - start;
            if (len < sizeof(path) - 16) {
                for (size_t i = 0; i < len; i++) path[i] = list[start + i];
                path[len] = 0;
                char full[200];
                size_t a = 0;
                for (size_t i = 0; i < sizeof(MODDIR) - 1; i++) full[a++] = MODDIR[i];
                full[a++] = '/';
                for (size_t i = 0; i <= len; i++) full[a + i] = path[i];
                int fd = open_r(full);
                if (fd >= 0) {
                    long r = sys(SYS_finit_module, fd, (long)"", 0, 0, 0, 0);
                    close_fd(fd);
                    if (r < 0) {
                        (*failed)++;
                        size_t used = slen(failbuf);
                        if (used + len + 20 < failcap) {
                            for (size_t i = 0; i < len; i++) failbuf[used + i] = path[i];
                            used += len;
                            failbuf[used++] = ':';
                            long err = -r;
                            char t[12]; int k = 0;
                            if (!err) t[k++] = '0';
                            while (err) { t[k++] = '0' + (err % 10); err /= 10; }
                            while (k--) failbuf[used++] = t[k];
                            failbuf[used++] = ' ';
                            failbuf[used] = 0;
                        }
                    } else {
                        (*loaded)++;
                    }
                } else {
                    (*failed)++;
                }
            }
        }
        at++;
    }
    return 0;
}

#define G1 "/config/usb_gadget/g1"

static char g_udc[64];

static int read_dir_names(const char *dir, char names[][64], int max)
{
    int fd = (int)sys(SYS_openat, AT_FDCWD, (long)dir, O_RDONLY | 0x4000 /* O_DIRECTORY */, 0, 0, 0);
    if (fd < 0) return -1;
    char buf[2048];
    long n = sys(SYS_getdents64, fd, (long)buf, sizeof(buf), 0, 0, 0);
    close_fd(fd);
    if (n <= 0) return -1;
    int count = 0;
    for (long off = 0; off < n;) {
        // linux_dirent64: ino(8) off(8) reclen(2) type(1) name[]
        const unsigned char *d = (const unsigned char *)buf + off;
        unsigned short reclen;
        for (size_t i = 0; i < sizeof(reclen); i++) ((unsigned char *)&reclen)[i] = d[16 + i];
        const char *name = (const char *)d + 19;
        if (!reclen) break;
        if (name[0] && name[0] != '.' && count < max) {
            size_t i = 0;
            while (name[i] && i + 1 < 64) { names[count][i] = name[i]; i++; }
            names[count][i] = 0;
            count++;
        }
        off += reclen;
    }
    return count;
}

static void force_device_role(void)
{
    static char names[8][64];
    int n = read_dir_names("/sys/class/usb_role", names, 8);
    for (int i = 0; i < n; i++) {
        char path[160];
        const char prefix[] = "/sys/class/usb_role/";
        size_t a = 0;
        for (size_t k = 0; k < sizeof(prefix) - 1; k++) path[a++] = prefix[k];
        for (size_t k = 0; names[i][k]; k++) path[a++] = names[i][k];
        const char suffix[] = "/role";
        for (size_t k = 0; k < sizeof(suffix); k++) path[a + k] = suffix[k];
        write_file(path, "device");
    }
}

static int gadget_setup(void)
{
    make_dir("/sys/kernel/config");
    mount_fs("none", "/sys/kernel/config", "configfs", 0);

    make_dir(G1);
    write_file(G1 "/idVendor", "0x1209");
    write_file(G1 "/idProduct", "0x0001");
    write_file(G1 "/bcdUSB", "0x0200");
    write_file(G1 "/bcdDevice", "0x0100");
    make_dir(G1 "/strings/0x409");
    write_file(G1 "/strings/0x409/serialnumber", "tb365fc-reader");
    write_file(G1 "/strings/0x409/manufacturer", "UniXNU");
    write_file(G1 "/strings/0x409/product", "TB365FC log reader");
    make_dir(G1 "/configs/c.1");
    make_dir(G1 "/configs/c.1/strings/0x409");
    write_file(G1 "/configs/c.1/strings/0x409/configuration", "log");
    make_dir(G1 "/functions/acm.gs0");
    make_link(G1 "/functions/acm.gs0", G1 "/configs/c.1/acm.gs0");

    for (int i = 0; i < 300; i++) {
        static char names[4][64];
        if (read_dir_names("/sys/class/udc", names, 4) > 0) {
            size_t k = 0;
            while (names[0][k] && k + 1 < sizeof(g_udc)) { g_udc[k] = names[0][k]; k++; }
            g_udc[k] = 0;
            break;
        }
        if ((i % 25) == 24) force_device_role();
        sleep_ms(200);
    }
    if (!g_udc[0]) return -1;
    if (write_file(G1 "/UDC", g_udc) < 0) return -1;
    return 0;
}

static int open_ttygs(unsigned *major, unsigned *minor)
{
    for (int i = 0; i < 100; i++) {
        if (read_block_dev("/sys/class/tty/ttyGS0", major, minor) == 0) {
            make_node("/dev/ttyGS0", MKNOD_MODE(S_IFCHR, 0666), *major, *minor);
            int fd = open_rw("/dev/ttyGS0");
            if (fd >= 0) return fd;
        }
        sleep_ms(200);
    }
    return -1;
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

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int serial_write_all(int fd, const void *data, size_t n, unsigned timeout_ms)
{
    const unsigned char *p = data;
    size_t done = 0;
    unsigned waited = 0;
    while (done < n && waited < timeout_ms) {
        long r = write_fd(fd, p + done, n - done);
        if (r > 0) { done += (size_t)r; waited = 0; continue; }
        sleep_ms(50);
        waited += 50;
    }
    return done == n ? 0 : -1;
}

static int stream_base64(int fd, const unsigned char *data, size_t n)
{
    char line[77];
    size_t i = 0;
    while (i + 3 <= n) {
        unsigned v = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8) | data[i + 2];
        line[0] = b64tab[(v >> 18) & 63];
        line[1] = b64tab[(v >> 12) & 63];
        line[2] = b64tab[(v >> 6) & 63];
        line[3] = b64tab[v & 63];
        line[4] = 0;
        if (serial_write_all(fd, line, 4, 30000) < 0) return -1;
        i += 3;
        if ((i / 3) % 19 == 0) {
            if (serial_write_all(fd, "\n", 1, 30000) < 0) return -1;
        }
    }
    if (i < n) {
        unsigned v = (unsigned)data[i] << 16;
        int rem = (int)(n - i);
        if (rem > 1) v |= (unsigned)data[i + 1] << 8;
        line[0] = b64tab[(v >> 18) & 63];
        line[1] = b64tab[(v >> 12) & 63];
        line[2] = rem > 1 ? b64tab[(v >> 6) & 63] : '=';
        line[3] = '=';
        line[4] = 0;
        if (serial_write_all(fd, line, 4, 30000) < 0) return -1;
    }
    return 0;
}

static unsigned char g_window[1 << 20];

// userdata partition (LUN2 device sdc, GPT index 69): start 0x34c800, last 0x1dc8ff7.
// The ShengUfs writer reserves the last 8192 4 KiB blocks of that partition.
#define WINDOW_LBA (0x1dc8ff7UL + 1 - 8192)
#define WINDOW_BYTES (1UL << 20)

static int read_window(unsigned char *out)
{
    unsigned maj = 8, min = 0;
    if (read_block_dev("/sys/block/sdc", &maj, &min) < 0) return -1;
    make_node("/dev/sdc", MKNOD_MODE(S_IFBLK, 0666), maj, min);
    int fd = open_r("/dev/sdc");
    if (fd < 0) return -1;
    long off = (long)(WINDOW_LBA * 4096UL);
    if (sys(SYS_lseek, fd, off, 0 /* SEEK_SET */, 0, 0, 0) < 0) { close_fd(fd); return -1; }
    size_t done = 0;
    while (done < WINDOW_BYTES) {
        long r = read_fd(fd, out + done, WINDOW_BYTES - done);
        if (r <= 0) break;
        done += (size_t)r;
    }
    close_fd(fd);
    return done == WINDOW_BYTES ? 0 : -1;
}

static void write_bcb_and_reboot(void)
{
    unsigned char block[4096];
    memzero(block, sizeof(block));
    // One-shot bootloader command: the next LK boot stops in fastboot, where
    // the host restores boot_a/init_boot_a/vendor_boot_a.
    const char cmd[] = "bootonce-bootloader";
    for (size_t i = 0; i < sizeof(cmd); i++) block[i] = (unsigned char)cmd[i];
    unsigned char *c = block + 2048;
    c[0] = '_'; c[1] = 'a';
    c[4] = 0x42; c[5] = 0x43; c[6] = 0x41; c[7] = 0x42;
    c[8] = 1; c[9] = 2;
    c[12] = 0xff;
    unsigned long crc = crc32_of(c, 28);
    for (int i = 0; i < 4; i++) c[28 + i] = (unsigned char)(crc >> (8 * i));
    unsigned maj = 8, min = 0;
    if (read_block_dev("/sys/block/sdc", &maj, &min) < 0) return;
    make_node("/dev/sdc", MKNOD_MODE(S_IFBLK, 0666), maj, min);
    int fd = open_rw("/dev/sdc");
    if (fd < 0) return;
    if (sys(SYS_lseek, fd, 8L * 4096L, 0, 0, 0, 0) < 0) { close_fd(fd); return; }
    write_fd(fd, block, sizeof(block));
    close_fd(fd);
    sys(SYS_reboot, 0xfee1dead, 672274793, 0x01234567, 0, 0, 0);
}

void _start(void)
{
    make_dir("/proc");
    make_dir("/sys");
    make_dir("/dev");
    make_dir("/tmp");
    mount_fs("proc", "/proc", "proc", 0);
    mount_fs("sysfs", "/sys", "sysfs", 0);
    mount_fs("tmpfs", "/dev", "tmpfs", 0);

    unsigned loaded = 0, failed = 0;
    static char failbuf[2048];
    failbuf[0] = 0;
    load_modules(failbuf, sizeof(failbuf), &loaded, &failed);

    int tty = -1;
    if (gadget_setup() == 0) {
        unsigned tm = 0, tn = 0;
        tty = open_ttygs(&tm, &tn);
    }
    if (tty < 0) {
        // No host channel: return to the bootloader so the host can restore.
        write_bcb_and_reboot();
        for (;;) sleep_ms(1000);
    }

    puts_fd(tty, "TBLOG1 mods_ok=");
    puts_num(tty, loaded, 10);
    puts_fd(tty, " mods_fail=");
    puts_num(tty, failed, 10);
    puts_fd(tty, " fail=");
    puts_fd(tty, failbuf);
    puts_fd(tty, "\n");

    if (read_window(g_window) == 0) {
        unsigned long crc = crc32_of(g_window, WINDOW_BYTES);
        puts_fd(tty, "WIN ");
        puts_num(tty, WINDOW_BYTES, 10);
        puts_fd(tty, " crc=");
        puts_hex(tty, crc);
        puts_fd(tty, "\n");
        stream_base64(tty, g_window, WINDOW_BYTES);
    } else {
        puts_fd(tty, "WIN read-failed\n");
    }
    puts_fd(tty, "\nEND\n");
    sleep_ms(3000);
    write_bcb_and_reboot();
    for (;;) sleep_ms(1000);
}
