/*
 * uart.c - Polled UART driver for Rockchip RV1103 (UART2)
 *
 * UART2 base address 0xff4c0000, register window 0x100, taken from
 * hw-addrs.md / rv1106.dtsi (node serial@ff4c0000, compatible
 * "rockchip,rv1106-uart", "snps,dw-apb-uart").
 *
 * 8250/16550 register offsets used:
 *   RBR/THR  0x00  (read = RX buffer, write = TX holding)
 *   IER      0x04
 *   IIR/FCR  0x08  (read = IIR, write = FCR)
 *   LCR      0x0C
 *   LSR      0x14
 *
 * Polled transmit: spin on LSR.THRE before writing THR.
 *
 * The U-Boot stage has already programmed the UART clock and baud
 * divisor, so this driver deliberately does NOT program the CRU.
 *
 * C99, freestanding, no libc.
 */

#include "uart.h"

/* --- Hardware description ------------------------------------------- */

#define UART2_BASE 0xff4c0000u

#define UART_RBR 0x00u /* Receive Buffer Register        (read)  */
#define UART_THR 0x00u /* Transmit Holding Register      (write) */
#define UART_IER 0x04u /* Interrupt Enable Register              */
#define UART_IIR 0x08u /* Interrupt Identification Reg.  (read)  */
#define UART_FCR 0x08u /* FIFO Control Register          (write) */
#define UART_LCR 0x0Cu /* Line Control Register                  */
#define UART_LSR 0x14u /* Line Status Register                   */

/* Line Status Register bits. */
#define UART_LSR_THRE (1u << 5) /* Transmit Holding Register Empty */
#define UART_LSR_TEMT (1u << 6) /* Transmitter Empty               */

/* Line Control Register: 8 data bits, no parity, 1 stop bit, DLAB=0. */
#define UART_LCR_8N1 0x03u

/* FIFO Control Register: enable FIFO, clear RX and TX FIFOs. */
#define UART_FCR_ENABLE 0x01u
#define UART_FCR_CLR_RX 0x02u
#define UART_FCR_CLR_TX 0x04u
#define UART_FCR_INIT \
    (UART_FCR_ENABLE | UART_FCR_CLR_RX | UART_FCR_CLR_TX)

/* 32-bit MMIO accessor. */
#define UART_REG(off) (*(volatile uint32_t *)(UART2_BASE + (off)))

/* --- Driver --------------------------------------------------------- */

void uart_init(void)
{
    /* Polled operation: mask every interrupt source. */
    UART_REG(UART_IER) = 0x00u;

    /* Enable the FIFO and flush any stale data. Writing FCR also
     * resets the IIR state. */
    UART_REG(UART_FCR) = UART_FCR_INIT;

    /* 8N1. Writing LCR also clears DLAB (bit 7). */
    UART_REG(UART_LCR) = UART_LCR_8N1;
}

void uart_putc(char c)
{
    /* Wait until the transmit holding register accepts a new byte. */
    while ((UART_REG(UART_LSR) & UART_LSR_THRE) == 0u) {
        /* busy-wait (polled) */
    }
    UART_REG(UART_THR) = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    while (*s != '\0') {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
        ++s;
    }
}

void uart_puthex(uint32_t v)
{
    static const char hexdigits[] = "0123456789abcdef";
    int shift;

    uart_putc('0');
    uart_putc('x');
    for (shift = 28; shift >= 0; shift -= 4) {
        uart_putc(hexdigits[(v >> (uint32_t)shift) & 0x0Fu]);
    }
}