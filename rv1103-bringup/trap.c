/*
 * trap.c - ARMv7 exception dispatch (bring-up).
 *
 * Records every trap; SVC additionally latches the r7 "syscall number".
 * No libc, -ffreestanding.
 */
#include "trap.h"
#include "syscall.h"
#include "board.h"
#include "armv7/exception.h"
#include "vm.h"

volatile unsigned trap_count[8];
volatile unsigned last_trap_exc;
volatile unsigned svc_last_num;

/*
 * Data abort forensics for the VM phase: print the fault status and
 * fault address registers before the vector stub parks the CPU, so a
 * bad vm_map shows up as "DFSR=... DFAR=..." instead of a silent hang.
 * (Prefetch aborts get IFSR/IFAR the same way.)
 *
 * Uses direct polled MMIO, not uart.c: uart.c hardcodes the RV1103
 * UART2 base, while this file also builds for QEMU virt (PL011).
 */
static void abt_putc(char c)
{
#if defined(BOARD_RV1103)
    volatile uint32_t *lsr = (volatile uint32_t *)(UART_BASE + 0x14u);
    volatile uint32_t *thr = (volatile uint32_t *)(UART_BASE + 0x00u);
    while ((*lsr & (1u << 5)) == 0u) { }   /* THRE */
    *thr = (uint32_t)(unsigned char)c;
#else
    volatile unsigned *fr = (volatile unsigned *)(UART_BASE + 0x18u);
    volatile unsigned *dr = (volatile unsigned *)(UART_BASE + 0x00u);
    while ((*fr & (1u << 5)) != 0u) { }    /* TXFF */
    *dr = (unsigned)(unsigned char)c;
#endif
    if (c == '\n')
        abt_putc('\r');
}

static void abt_puts(const char *s)
{
    while (*s)
        abt_putc(*s++);
}

static void abt_puthex(uint32_t v)
{
    int i;
    abt_puts("0x");
    for (i = 7; i >= 0; i--)
        abt_putc("0123456789abcdef"[(v >> (i * 4)) & 0xfu]);
}

static void report_abort(const char *kind, uint32_t fsr, uint32_t far,
                         struct trap_regs *regs)
{
    unsigned i;
    uint32_t ttbr0;
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
    abt_puts("\n[abort] ");
    abt_puts(kind);
    abt_puts(" FSR=");
    abt_puthex(fsr);
    abt_puts(" FAR=");
    abt_puthex(far);
    abt_puts(" TTBR0=");
    abt_puthex(ttbr0);
    if (regs) {
        uint32_t pc = regs->lr - 8u;
        abt_puts(" PC~=");
        abt_puthex(pc);
        for (i = 0; i < 13; i++) {
            abt_puts(i == 0 ? " r0=" : " ");
            abt_puthex(regs->r[i]);
        }
    }
    abt_putc('\n');
}

void arm_trap(unsigned exc, struct trap_regs *regs)
{
    if (exc < EXC_VECTOR_COUNT) {
        trap_count[exc]++;
        last_trap_exc = exc;
    }
    if (exc == EXC_DATA_ABORT) {
        uint32_t dfsr, dfar;
        __asm__ __volatile__("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
        __asm__ __volatile__("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
        /* Pager first: a translation fault in the demand range gets a
         * fresh page and the instruction is retried. The stub returns
         * via rfefd using the stacked LR_abt (= fault + 8 for data
         * aborts), so rewind it to the faulting instruction. */
        if (vm_page_fault(dfar, dfsr)) {
            if (regs)
                regs->lr -= 8u;
            return;
        }
        report_abort("data", (unsigned)dfsr, (unsigned)dfar, regs);
        for (;;) { __asm__ __volatile__("wfi"); }  /* genuine bug: park */
    } else if (exc == EXC_PREFETCH_ABORT) {
        uint32_t ifsr, ifar;
        __asm__ __volatile__("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));
        __asm__ __volatile__("mrc p15, 0, %0, c6, c0, 2" : "=r"(ifar));
        report_abort("prefetch", (unsigned)ifsr, (unsigned)ifar, regs);
    }
    if (exc == EXC_SVC && regs) {
        svc_last_num = regs->r[7];
        /* r7 doubles as the syscall number; dispatch may rewrite r0. */
        svc_dispatch(regs);
    }
}
