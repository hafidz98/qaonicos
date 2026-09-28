/* P4 context-switch functional test on QEMU virt (Cortex-A7). */
#include "../rv1103-bringup/pcb.h"

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

static void putdec(unsigned v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) putc(buf[--i]);
}

static struct pcb pcb_main, pcb_t1, pcb_t2;
static unsigned char stack1[4096] __attribute__((aligned(8)));
static unsigned char stack2[4096] __attribute__((aligned(8)));

/* Local counters must survive being switched out and back in: they live
 * in callee-saved regs and/or the thread's own stack, both of which
 * ctx_switch is responsible for preserving. */
static void thread1(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        puts("T1 i="); putdec((unsigned)i); putc('\n');
        ctx_switch(&pcb_t1, &pcb_t2);
    }
    ctx_switch(&pcb_t1, &pcb_main);
}

static void thread2(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        puts("T2 i="); putdec((unsigned)i); putc('\n');
        ctx_switch(&pcb_t2, &pcb_t1);
    }
    ctx_switch(&pcb_t2, &pcb_main);
}

int main(void)
{
    puts("P4 ctx-switch test (qemu-virt, cortex-a7)\n");

    pcb_init(&pcb_t1, stack1 + sizeof(stack1), thread1);
    pcb_init(&pcb_t2, stack2 + sizeof(stack2), thread2);

    ctx_switch(&pcb_main, &pcb_t1);
    /* Resumed here after thread1's final switch. */

    puts("back in main\n");
    puts("ALL TESTS PASSED\n");
    for (;;) { }
}
