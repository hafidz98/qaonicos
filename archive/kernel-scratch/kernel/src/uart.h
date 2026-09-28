/*
 * uart.h - Polled UART driver for Rockchip RV1103 (UART2)
 *
 * Target : RV1103 (Luckfox Pico Mini), UART2 @ 0xff4c0000
 * IP     : Synopsys DesignWare APB UART (snps,dw-apb-uart),
 *          8250/16550-compatible register layout, 32-bit MMIO.
 *
 * C99, freestanding, no libc.
 */
#ifndef RV1103_UART_H
#define RV1103_UART_H

#include <stdint.h>

/*
 * uart_init - configure UART2 for polled TX/RX.
 *
 * Sets the line control to 8 data bits, no parity, 1 stop bit (8N1)
 * and enables/resets the hardware FIFOs via FCR. Interrupts are left
 * disabled (polled operation only).
 *
 * The UART clock and baud divisor must already be configured by the
 * bootloader (U-Boot); this driver never touches the CRU.
 */
void uart_init(void);

/* uart_putc - transmit a single raw byte, blocking until THR is free. */
void uart_putc(char c);

/*
 * uart_puts - transmit a NUL-terminated string.
 * A '\n' is expanded to "\r\n" for terminals.
 */
void uart_puts(const char *s);

/* uart_puthex - print a 32-bit value as "0x" + 8 lowercase hex digits. */
void uart_puthex(uint32_t v);

#endif /* RV1103_UART_H */