/* P3 trap functional test on QEMU virt (Cortex-A7). */
#include "../rv1103-bringup/trap.h"
#include "../rv1103-bringup/armv7/exception.h"

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

int main(void)
{
    int fails = 0;

    puts("P3 trap test (qemu-virt, cortex-a7)\n");

    /* SVC #0 with r7 = 42. Must return and record r7. */
    __asm__ volatile("mov r7, %0\n\tsvc #0" :: "r"(42u) : "r7", "lr", "memory");
    if (svc_last_num == 42u && trap_count[EXC_SVC] == 1u &&
        last_trap_exc == EXC_SVC)
        puts("PASS svc #0 (r7=42 recorded, returned)\n");
    else { puts("FAIL svc #0\n"); fails++; }

    /* Undefined instruction. Must record and return past it. */
    __asm__ volatile(".word 0xe7f000f0" ::: "memory");   /* udf #0 */
    if (trap_count[EXC_UNDEF] == 1u && last_trap_exc == EXC_UNDEF)
        puts("PASS undef (recorded, returned)\n");
    else { puts("FAIL undef\n"); fails++; }

    /* Second SVC: traps must be repeatable. */
    __asm__ volatile("mov r7, %0\n\tsvc #1" :: "r"(7u) : "r7", "lr", "memory");
    if (svc_last_num == 7u && trap_count[EXC_SVC] == 2u)
        puts("PASS svc #1 (repeat)\n");
    else { puts("FAIL svc #1\n"); fails++; }

    if (fails == 0) puts("ALL TESTS PASSED\n");
    else puts("FAILURES PRESENT\n");

    for (;;) { }
}
