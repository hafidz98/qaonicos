/*
 * fstest.c - Program userspace penguji ramfs (Fase 9).
 *
 * Bare-metal, tanpa libc. Di-link di FSTEST_PROG_VA (0x10028000) via
 * fstest.ld, di-embed sebagai blob -> fstest_img, dimuat kernel ke
 * task_user sebelum thread fstest dijadwalkan.
 *
 * Alur uji:
 *   1. open("/halo.txt", O_CREAT|O_RDWR) -> fd >= 3
 *   2. write(fd, "isi rahasia") -> 11 byte
 *   3. close, open(O_RDONLY), read -> verifikasi byte-exact
 *   4. ls -> "halo.txt" muncul di daftar
 *   5. delete("/halo.txt") -> ls lagi -> daftar kosong
 *   6. negatif: open tanpa O_CREAT -> -1; read fd tertutup -> -1;
 *      write fd liar -> -1
 *   7. cetak "FS TESTS PASSED", buat sentinel "/.fs_done" (gerbang
 *      halt kernel di report()), lalu sys_exit.
 */
#include "usys.h"

static int sys_write(unsigned fd, const char *buf, unsigned len)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #20\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(fd), "r"(buf), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return ret;
}

static int sys_open(const char *path, unsigned flags)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r7, #30\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(path), "r"(flags)
        : "r0", "r1", "r7", "lr", "memory", "cc");
    return ret;
}

static int sys_read(unsigned fd, char *buf, unsigned len)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #31\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(fd), "r"(buf), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return ret;
}

static int sys_close(unsigned fd)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r7, #32\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(fd)
        : "r0", "r7", "lr", "memory", "cc");
    return ret;
}

static int sys_ls(char *buf, unsigned max)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r7, #33\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(buf), "r"(max)
        : "r0", "r1", "r7", "lr", "memory", "cc");
    return ret;
}

static int sys_delete(const char *path)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r7, #34\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(path)
        : "r0", "r7", "lr", "memory", "cc");
    return ret;
}

static void sys_exit(void)
{
    __asm__ volatile(
        "mov r7, #22\n\t"
        "svc #0"
        ::: "r7", "lr", "memory", "cc");
    for (;;) { }   /* tak tercapai */
}

static void put(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    sys_write(1u, s, n);
}

/* memcmp mini (tanpa libc). */
static int mcmp(const char *a, const char *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 1;
    return 0;
}

/* 1 bila `needle` muncul di buf[0..len). */
static int contains(const char *buf, unsigned len, const char *needle)
{
    unsigned nl = 0, i, j;
    while (needle[nl])
        nl++;
    if (nl == 0 || nl > len)
        return 0;
    for (i = 0; i + nl <= len; i++) {
        for (j = 0; j < nl; j++)
            if (buf[i + j] != needle[j])
                break;
        if (j == nl)
            return 1;
    }
    return 0;
}

__attribute__((section(".text.start")))
void _start(void)
{
    int fd, r, fails = 0;
    char buf[256];
    unsigned i;

    put("[fs] ramfs test mulai\n");
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = 0;

    /* 1-2. create + write. */
    fd = sys_open("/halo.txt", O_CREAT | O_RDWR);
    if (fd < 3) {
        put("FAIL: open(O_CREAT|O_RDWR)\n");
        fails++;
    } else {
        r = sys_write((unsigned)fd, "isi rahasia", 11);
        if (r != 11) {
            put("FAIL: write 11 byte\n");
            fails++;
        }
        if (sys_close((unsigned)fd) != 0) {
            put("FAIL: close\n");
            fails++;
        }
    }

    /* 3. read back byte-exact. */
    fd = sys_open("/halo.txt", O_RDONLY);
    if (fd < 3) {
        put("FAIL: open(O_RDONLY)\n");
        fails++;
    } else {
        r = sys_read((unsigned)fd, buf, sizeof(buf));
        if (r != 11 || mcmp(buf, "isi rahasia", 11) != 0) {
            put("FAIL: read tidak byte-exact\n");
            fails++;
        } else {
            put("read ok: isi cocok\n");
        }
        sys_close((unsigned)fd);
    }

    /* 4. ls memuat halo.txt. */
    r = sys_ls(buf, sizeof(buf));
    if (r < 1 || !contains(buf, sizeof(buf), "halo.txt")) {
        put("FAIL: ls tidak memuat halo.txt\n");
        fails++;
    } else {
        put("ls ok\n");
    }

    /* 5. delete -> ls kosong. */
    if (sys_delete("/halo.txt") != 0) {
        put("FAIL: delete\n");
        fails++;
    } else {
        for (i = 0; i < sizeof(buf); i++)
            buf[i] = 0;   /* jangan baca sisa "halo.txt" dari ls sebelumnya */
        r = sys_ls(buf, sizeof(buf));
        if (r != 0 || contains(buf, sizeof(buf), "halo.txt")) {
            put("FAIL: file masih ada sesudah delete\n");
            fails++;
        } else {
            put("delete ok\n");
        }
    }

    /* 6. Kasus negatif. */
    if (sys_open("/takada.txt", O_RDONLY) != -1) {
        put("FAIL: open tanpa O_CREAT harus -1\n");
        fails++;
    }
    if (sys_read(3u, buf, 16) != -1) {
        /* fd 3 sudah di-close di atas -> harus -1 (fd basi). */
        put("FAIL: read fd tertutup harus -1\n");
        fails++;
    }
    if (sys_write(99u, "x", 1) != -1) {
        put("FAIL: write fd liar harus -1\n");
        fails++;
    }
    put("negatif ok\n");

    if (fails == 0)
        put("FS TESTS PASSED\n");
    else
        put("FS TESTS FAILED\n");

    /* Sentinel untuk gerbang halt kernel (report()): hanya dibuat bila
     * semua verifikasi lolos. */
    if (fails == 0) {
        fd = sys_open("/.fs_done", O_CREAT | O_RDWR);
        if (fd >= 3)
            sys_close((unsigned)fd);
    }

    sys_exit();
    for (;;) { }
}
