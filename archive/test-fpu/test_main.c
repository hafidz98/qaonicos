/* P5 VFPv4 functional test on QEMU virt (Cortex-A7). */
#include "../rv1103-bringup/fpu.h"

/* PL011 minimal TX (virt UART0 @ 0x09000000). */
#define UARTDR  (*(volatile unsigned *)0x09000000u)
#define UARTFR  (*(volatile unsigned *)0x09000018u)

static void putc(char c)
{
    while (UARTFR & (1u << 5)) { }
    UARTDR = (unsigned)c;
    if (c == '\n') putc('\r');
}

static void puts(const char *s)
{
    while (*s) putc(*s++);
}

/* 3.25 in IEEE-754 double. */
#define BITS_3_25 0x400A000000000000ull

static uint64_t dbits(double x)
{
    uint64_t b;
    __builtin_memcpy(&b, &x, 8);
    return b;
}

int main(void)
{
    static struct vfp_state st;
    int fails = 0;
    double x;

    puts("P5 fpu test (qemu-virt, cortex-a7)\n");

    /* Exercise the real API (idempotent), then verify it took effect. */
    fpu_enable();
    {
        uint32_t fpexc;
        __asm__ volatile("vmrs %0, fpexc" : "=r"(fpexc));
        if (fpexc & (1u << 30))
            puts("PASS fpexc.en set by fpu_enable()\n");
        else { puts("FAIL fpexc.en\n"); fails++; }
    }

    /* VFP arithmetic: without the enable above this would undef-fault. */
    x = 1.5;
    x = x * 2.0 + 0.25;
    if (dbits(x) == BITS_3_25)
        puts("PASS vfp arithmetic (1.5*2+0.25=3.25)\n");
    else { puts("FAIL vfp arithmetic\n"); fails++; }

    /* Save/restore must cover the raw VFP register file. Do it entirely
     * in d-regs (a C `double` local may live on the stack, which is not
     * part of the VFP state). Patterns: 1.0/2.0/3.0/4.0, clobber 9.0. */
    {
        uint32_t lo, hi;
        uint64_t b;
#define SET4()  __asm__ volatile( \
            "vmov.f64 d0,  #1.0\n\t" \
            "vmov.f64 d8,  #2.0\n\t" \
            "vmov.f64 d16, #3.0\n\t" \
            "vmov.f64 d31, #4.0\n\t" \
            ::: "d0", "d8", "d16", "d31", "memory")
#define CLOBBER4() __asm__ volatile( \
            "vmov.f64 d0,  #9.0\n\t" \
            "vmov.f64 d8,  #9.0\n\t" \
            "vmov.f64 d16, #9.0\n\t" \
            "vmov.f64 d31, #9.0\n\t" \
            ::: "d0", "d8", "d16", "d31", "memory")
#define READBACK(dn, expect) do { \
            __asm__ volatile("vmov %0, %1, " #dn : "=r"(lo), "=r"(hi)); \
            b = ((uint64_t)hi << 32) | lo; \
            if (b != (expect)) { \
                puts("FAIL save/restore " #dn "\n"); fails++; \
            } \
        } while (0)

        SET4();
        fpu_save(&st);
        CLOBBER4();
        fpu_restore(&st);
        READBACK(d0,  0x3FF0000000000000ull);   /* 1.0 */
        READBACK(d8,  0x4000000000000000ull);   /* 2.0 */
        READBACK(d16, 0x4008000000000000ull);   /* 3.0 */
        READBACK(d31, 0x4010000000000000ull);   /* 4.0 */
        if (fails == 0)
            puts("PASS save/restore (d0/d8/d16/d31)\n");
    }

    if (fails == 0) puts("ALL TESTS PASSED\n");
    else puts("FAILURES PRESENT\n");

    for (;;) { }
}
