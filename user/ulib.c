/*
 * ulib.c - Implementasi ulib.h (Fase 10).
 *
 * Pola inline asm disalin dari hello.c/fstest.c (sudah terbukti):
 * r7 = nomor syscall, "lr" masuk clobber karena svc menimpanya
 * (pelajaran P6 Fase 2). Nomor di bawah = SYS_* di usys.h.
 */
#include "ulib.h"

int u_write(unsigned fd, const char *buf, unsigned len)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #20\n\t"          /* SYS_WRITE */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(fd), "r"(buf), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return ret;
}

int u_open(const char *path, unsigned flags)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r7, #30\n\t"          /* SYS_OPEN */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(path), "r"(flags)
        : "r0", "r1", "r7", "lr", "memory", "cc");
    return ret;
}

int u_read(unsigned fd, char *buf, unsigned len)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #31\n\t"          /* SYS_READ */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(fd), "r"(buf), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return ret;
}

int u_close(unsigned fd)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r7, #32\n\t"          /* SYS_CLOSE */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(fd)
        : "r0", "r7", "lr", "memory", "cc");
    return ret;
}

int u_ls(char *buf, unsigned max)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r7, #33\n\t"          /* SYS_LS */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(buf), "r"(max)
        : "r0", "r1", "r7", "lr", "memory", "cc");
    return ret;
}

void u_yield(void)
{
    __asm__ volatile(
        "mov r7, #21\n\t"          /* SYS_YIELD */
        "svc #0"
        ::: "r7", "lr", "memory", "cc");
}

void u_exit(void)
{
    __asm__ volatile(
        "mov r7, #22\n\t"          /* SYS_EXIT */
        "svc #0"
        ::: "r7", "lr", "memory", "cc");
    for (;;) { }   /* tak tercapai */
}

void u_put(const char *s)
{
    u_write(1u, s, u_strlen(s));
}

unsigned u_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

int u_mcmp(const char *a, const char *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 1;
    return 0;
}

int u_contains(const char *buf, unsigned len, const char *needle)
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

int u_wait_file(const char *path)
{
    unsigned long t;
    int fd;
    for (t = 0; t < ULIB_POLL_MAX; t++) {
        fd = u_open(path, O_RDONLY);
        if (fd >= 0) {
            u_close((unsigned)fd);
            return 1;
        }
        if ((t & 0xFFu) == 0u)
            u_yield();
    }
    return 0;
}

int u_touch(const char *path)
{
    int fd = u_open(path, O_CREAT | O_RDWR);
    if (fd < 0)
        return 0;
    u_close((unsigned)fd);
    return 1;
}

/* Fase 14: GPIO. */
int u_gpio_set(unsigned pin, unsigned val)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r7, #40\n\t"          /* SYS_GPIO_SET */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(pin), "r"(val)
        : "r0", "r1", "r7", "lr", "memory", "cc");
    return ret;
}

int u_gpio_get(unsigned pin)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r7, #41\n\t"          /* SYS_GPIO_GET */
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(pin)
        : "r0", "r7", "lr", "memory", "cc");
    return ret;
}
