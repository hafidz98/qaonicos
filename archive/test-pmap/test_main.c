/* P2 pmap functional test on QEMU virt (Cortex-A7). */
#include "../rv1103-bringup/pmap.h"

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

static void puthex(unsigned v)
{
    int i;
    puts("0x");
    for (i = 7; i >= 0; i--) {
        unsigned n = (v >> (i * 4)) & 0xFu;
        putc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}

int main(void)
{
    volatile unsigned *pa_cell = (volatile unsigned *)0x40001000u;
    volatile unsigned *va_cell = (volatile unsigned *)0x80001000u;
    unsigned r1, r2;
    int fails = 0;

    puts("P2 pmap test (qemu-virt, cortex-a7)\n");

    pmap_init();
    puts("pmap_init ok\n");

    *pa_cell = 0x12345678u;
    pmap_enable();
    puts("MMU enabled\n");

    /* Test 1: 1:1 mapping still works with MMU on. */
    r1 = *pa_cell;
    if (r1 == 0x12345678u) { puts("PASS 1:1 rw after MMU on\n"); }
    else { puts("FAIL 1:1 readback: "); puthex(r1); putc('\n'); fails++; }

    /* Test 2: fresh VA->PA section mapping translates. */
    pmap_map_section(0x80000000u, 0x40000000u, PMAP_CACHEABLE);
    *va_cell = 0xDEADBEEFu;
    r2 = *pa_cell;
    if (r2 == 0xDEADBEEFu) { puts("PASS remap 0x80000000->0x40000000\n"); }
    else { puts("FAIL remap readback: "); puthex(r2); putc('\n'); fails++; }

    /* Test 3: UART still reachable -> device mapping works (every puts). */
    puts("PASS device mapping (uart alive)\n");

    pmap_unmap_section(0x80000000u);
    pmap_disable();
    puts("MMU disabled\n");

    if (fails == 0) puts("ALL TESTS PASSED\n");
    else { puts("FAILURES: "); puthex((unsigned)fails); putc('\n'); }

    for (;;) { }
}
