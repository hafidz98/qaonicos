/*
 * main.c - RV1103 bare-metal bring-up banner.
 *
 * Prints a fixed banner and the SoC peripheral / memory map over
 * UART2 (0xff4c0000), then parks the core in WFI.
 *
 * Uses only the existing polled driver in uart.c / uart.h.
 */

#include "uart.h"

/* One-line helper: print "label = 0x........\n". */
static void print_addr(const char *label, uint32_t addr)
{
    uart_puts(label);
    uart_puts(" = ");
    uart_puthex(addr);
    uart_putc('\n');
}

void main(void)
{
    uart_init();

    uart_puts("\n");
    uart_puts("=== RV1103 bring-up ===\n");

    uart_puts("\nSoC address map:\n");
    print_addr("UART2 ", 0xff4c0000u);
    print_addr("GICD  ", 0xff1f1000u);
    print_addr("GICC  ", 0xff1f2000u);
    print_addr("CRU   ", 0xff3a0000u);
    print_addr("GRF   ", 0xff000000u);
    print_addr("PMU   ", 0xff300000u);

    uart_puts("\nMemory:\n");
    uart_puts("DRAM base = ");
    uart_puthex(0x00000000u);
    uart_puts("  size = 64MB\n");

    uart_puts("\nbring-up OK, halting (wfi)\n");

    for (;;) {
        __asm__ volatile("wfi");
    }
}