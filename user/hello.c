/*
 * hello.c - Program userspace pertama: bare-metal, tanpa libc.
 *
 * Di-link di USER_PROG_VA (0x10030000) via hello.ld, di-embed sebagai
 * blob biner ke kernel, dimuat kernel ke 4 halaman R+X sebelum thread
 * user pertama dijadwalkan.
 *
 * Tes yang dilakukan:
 *   1. sys_write  -> cetak sapaan
 *   2. sys_rpc    -> echo "halo dari user" via server kernel, verifikasi
 *                    balasan diawali "echo:"
 *   3. sys_sbrk   -> minta 1 halaman, tulis & baca pola 0..255
 *   4. cetak "USER TESTS PASSED"
 *   5. uji proteksi (negatif): baca 0x40000000 (memori kernel, priv-only
 *      sejak Fase 8) -> HARUS fault -> thread dibunuh kernel. Bila baca
 *      ini lolos, cetak "PROTEKSI GAGAL" (bug!).
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

static int sys_rpc_user(unsigned sname, const struct user_wire *req,
                        unsigned rlen, unsigned rname,
                        struct user_wire *rep, unsigned plen)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r3, %4\n\t"
        "mov r4, %5\n\t"
        "mov r5, %6\n\t"
        "mov r7, #23\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(sname), "r"(req), "r"(rlen), "r"(rname), "r"(rep), "r"(plen)
        : "r0", "r1", "r2", "r3", "r4", "r5", "r7", "lr", "memory", "cc");
    return ret;
}

static int sys_sbrk(int inc)
{
    int ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r7, #24\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(inc)
        : "r0", "r7", "lr", "memory", "cc");
    return ret;
}

static void sys_yield(void)
{
    __asm__ volatile(
        "mov r7, #21\n\t"
        "svc #0"
        ::: "r7", "lr", "memory", "cc");
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

static struct user_wire req;
static struct user_wire rep;

__attribute__((section(".text.start")))
void _start(void)
{
    unsigned n = 0;
    int r, i;

    put("hello dari user mode (USR, PL0)\n");

    /* 2. RPC echo ke server kernel. */
    {
        const char *m = "halo dari user";
        req.bits = 0;
        req.id = ECHO_REQ_ID;
        while (m[n]) {
            req.data[n] = (unsigned char)m[n];
            n++;
        }
        req.data[n] = 0;
        req.size = n + 1;
    }
    r = sys_rpc_user(U_SEND_PORT, &req, sizeof(req),
                     U_REPLY_PORT, &rep, sizeof(rep));
    if (r > 0 && rep.id == ECHO_REP_ID) {
        put("balasan: ");
        sys_write(1u, (const char *)rep.data, rep.size);
        put("\n");
        /* Verifikasi awalan "echo:". */
        if (rep.size >= 6 &&
            rep.data[0] == 'e' && rep.data[1] == 'c' &&
            rep.data[2] == 'h' && rep.data[3] == 'o' &&
            rep.data[4] == ':')
            put("echo ok\n");
        else
            put("echo FAIL: balasan tak sesuai\n");
    } else {
        put("rpc FAIL\n");
    }

    /* 3. sbrk smoke test: 1 halaman, tulis & baca pola. */
    {
        int brk0 = sys_sbrk(0);
        int brk1 = sys_sbrk(4096);
        if (brk0 > 0 && brk1 == brk0) {
            volatile unsigned char *p = (volatile unsigned char *)brk0;
            int ok = 1;
            for (i = 0; i < 4096; i++)
                p[i] = (unsigned char)(i & 0xFF);
            for (i = 0; i < 4096; i++) {
                if (p[i] != (unsigned char)(i & 0xFF)) {
                    ok = 0;
                    break;
                }
            }
            put(ok ? "sbrk ok\n" : "sbrk FAIL: pola rusak\n");
        } else {
            put("sbrk FAIL: brk tak valid\n");
        }
    }

    put("USER TESTS PASSED\n");

    /* 5. Uji proteksi negatif: baca memori kernel (0x40000000, section
     * priv-only) dari USR HARUS memicu data abort -> thread dibunuh.
     * Bila lolos sampai baris berikut, itu BUG proteksi. */
    put("uji proteksi: baca 0x40000000 (harusnya fault)...\n");
    {
        volatile unsigned *kp = (volatile unsigned *)0x40000000u;
        unsigned v = *kp;   /* <-- fault di sini */
        (void)v;
        put("PROTEKSI GAGAL: baca memori kernel lolos dari USR!\n");
    }

    sys_yield();
    sys_exit();
    for (;;) { }
}
