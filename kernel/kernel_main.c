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
#include "../rv1103-bringup/ipc.h"
#include "../rv1103-bringup/syscall.h"
#include "../rv1103-bringup/zone.h"
#include "../rv1103-bringup/lib.h"

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

/* IPC: one task, one shared space (Mach threads of a task share it). */
static struct zone port_zone, msg_zone;
static uint8_t port_pool[IPC_NPORTS * sizeof(struct ipc_port)] __attribute__((aligned(8)));
static uint8_t msg_pool[16 * sizeof(struct ipc_msg)] __attribute__((aligned(8)));
static struct ipc_space kern_space;
static unsigned demo_port;

/* mach_msg-style traps. Explicit clobbers (P6 lesson: svc kills lr). */
static int svc_send(unsigned name, const struct ipc_wire *w, unsigned len)
{
    unsigned ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #10\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(name), "r"(w), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return (int)ret;
}

static int svc_recv(unsigned name, struct ipc_wire *w, unsigned len)
{
    unsigned ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #11\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(name), "r"(w), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return (int)ret;
}

static void wire_puts(struct ipc_wire *w, uint32_t id, const char *txt)
{
    unsigned n = 0;
    w->bits = 0;
    w->id = id;
    while (txt[n] && n < IPC_MSG_DATA - 1) {
        w->data[n] = (uint8_t)txt[n];
        n++;
    }
    w->data[n] = 0;
    w->size = n + 1;
}

/* Thread A: VFP compute thread, also the IPC initiator. */
static struct pcb pcb_main, pcb_a, pcb_b;
static unsigned char stack_a[4096] __attribute__((aligned(8)));
static unsigned char stack_b[4096] __attribute__((aligned(8)));
static struct vfp_state vfp_a, vfp_b;

static void thread_a(void)
{
    int i;
    struct ipc_wire w;

    /* IPC: send first, then wait for B's reply. */
    wire_puts(&w, 0xA1, "halo dari thread A");
    puts("  A: sending msg id=0xa1 -> ");
    putdec((unsigned)svc_send(demo_port, &w, sizeof(w)));
    putc('\n');

    fpu_save(&vfp_a);
    ctx_switch(&pcb_a, &pcb_b);
    fpu_restore(&vfp_a);

    if (svc_recv(demo_port, &w, sizeof(w)) >= 0) {
        puts("  A: got reply id="); puthex64(w.id);
        puts(" : "); puts((const char *)w.data); putc('\n');
    }

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

/* Thread B: syscall thread (takes SVC traps), answers A's message. */
static void thread_b(void)
{
    int i;
    struct ipc_wire w;

    if (svc_recv(demo_port, &w, sizeof(w)) >= 0) {
        puts("  B: got msg id="); puthex64(w.id);
        puts(" : "); puts((const char *)w.data); putc('\n');
    }
    wire_puts(&w, 0xB1, "balasan dari thread B");
    puts("  B: replying -> ");
    putdec((unsigned)svc_send(demo_port, &w, sizeof(w)));
    putc('\n');

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

    /* 3. IPC: zones, one shared space, one port, syscall dispatch. */
    zone_init(&port_zone, port_pool, sizeof(port_pool),
              sizeof(struct ipc_port));
    zone_init(&msg_zone, msg_pool, sizeof(msg_pool),
              sizeof(struct ipc_msg));
    ipc_space_init(&kern_space, &port_zone, &msg_zone);
    syscall_init(&kern_space);
    demo_port = ipc_port_alloc(&kern_space, IPC_SEND | IPC_RECV);
    puts("[ipc ] zones up, port allocated, name = ");
    putdec(demo_port);
    puts(" (send+recv)\n");

    /* 4. Threads: A <-IPC-> B, then A (fpu) and B (svc) interleave. */
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
