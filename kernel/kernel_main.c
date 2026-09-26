/*
 * kernel_main.c - Integrated Mach-x-Luckfox bring-up demo.
 *
 * One ELF that boots like a real kernel: MMU on (pmap), traps live,
 * FPU enabled, then two threads ping-ponging through ctx_switch while
 * one does VFP math and the other takes SVC traps.
 *
 * Runs on: qemu-system-arm -M virt -cpu cortex-a7
 */
#include "../rv1103-bringup/pmap.h"
#include "../rv1103-bringup/trap.h"
#include "../rv1103-bringup/pcb.h"
#include "../rv1103-bringup/fpu.h"

/* PL011 (QEMU virt UART0). */
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

static void puthex64(uint64_t v)
{
    int i;
    puts("0x");
    for (i = 15; i >= 0; i--)
        putc("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}

static uint64_t dbits(double x)
{
    uint64_t b;
    __builtin_memcpy(&b, &x, 8);
    return b;
}

/* Thread A: VFP compute thread. */
static struct pcb pcb_main, pcb_a, pcb_b;
static unsigned char stack_a[4096] __attribute__((aligned(8)));
static unsigned char stack_b[4096] __attribute__((aligned(8)));
static struct vfp_state vfp_a, vfp_b;

static void thread_a(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        double x = 1.5 * (double)(i + 1);
        x = x * 2.0 + 0.25;
        puts("  A: i="); putdec((unsigned)i);
        puts("  fpu -> "); puthex64(dbits(x)); putc('\n');
        /* A real kernel saves/restores VFP state on every switch. */
        fpu_save(&vfp_a);
        ctx_switch(&pcb_a, &pcb_b);
        fpu_restore(&vfp_a);
    }
    fpu_save(&vfp_a);
    ctx_switch(&pcb_a, &pcb_main);
}

/* Thread B: syscall thread (takes SVC traps). */
static void thread_b(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        /* NOTE the "lr" clobber: svc overwrites lr_svc with the return
         * address (lesson learned in the P6 Clang cross-check). */
        __asm__ volatile("mov r7, %0\n\tsvc #0"
                         :: "r"(200u + (unsigned)i) : "r7", "lr", "memory");
        puts("  B: i="); putdec((unsigned)i);
        puts("  svc trap, r7 recorded="); putdec(svc_last_num); putc('\n');
        fpu_save(&vfp_b);
        ctx_switch(&pcb_b, &pcb_a);
        fpu_restore(&vfp_b);
    }
    fpu_save(&vfp_b);
    ctx_switch(&pcb_b, &pcb_main);
}

void kernel_main(void)
{
    puts("\nMach-x-Luckfox kernel booting (qemu-virt, cortex-a7)\n");

    /* 1. Memory management. */
    pmap_init();
    pmap_enable();
    puts("[pmap] MMU on: 1:1 DRAM + device mappings\n");

    /* 2. Floating point. */
    fpu_enable();
    {
        double x = 1.5;
        x = x * 2.0 + 0.25;
        puts("[fpu ] VFPv4 on, 1.5*2+0.25 = ");
        puthex64(dbits(x));
        puts(" (3.25)\n");
    }

    /* 3. Traps. */
    __asm__ volatile("mov r7, %0\n\tsvc #0"
                     :: "r"(99u) : "r7", "lr", "memory");
    puts("[trap] SVC #0 taken, r7 recorded = ");
    putdec(svc_last_num);
    putc('\n');

    /* 4. Threads. */
    puts("[sched] spawning threads A (fpu) and B (svc)\n");
    pcb_init(&pcb_a, stack_a + sizeof(stack_a), thread_a);
    pcb_init(&pcb_b, stack_b + sizeof(stack_b), thread_b);
    ctx_switch(&pcb_main, &pcb_a);
    /* Back here when both threads finish. */

    puts("[sched] all threads done\n");
    puts("kernel demo complete - halting\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}
