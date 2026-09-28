/*
 * mach3/kernel/arm/uart.c -- PL011 UART driver (QEMU -M virt).
 *
 * Base 0x09000000 (UART0).  PL011 registers used:
 *   DR  0x00  (data)
 *   FR  0x18  (flag: bit5 TXFF = TX FIFO full, bit4 RXFE = RX FIFO empty)
 *
 * QEMU initializes the PL011; we only need polled TX/RX.
 * Provides Mach console hooks: cnputc/cngetc/cnmaygetc.
 */
#include <mach/machine/vm_types.h>

#define	UART0_BASE	0x09000000u
#define	UART_DR		0x00u
#define	UART_FR		0x18u
#define	UART_FR_TXFF	(1u << 5)
#define	UART_FR_RXFE	(1u << 4)

#define	UART_REG(off)	(*(volatile unsigned int *)(UART0_BASE + (off)))

void
uart_init(void)
{
	/* QEMU's PL011 is ready; nothing to program. */
}

void
uart_putc(char c)
{
	while (UART_REG(UART_FR) & UART_FR_TXFF)
		;
	UART_REG(UART_DR) = (unsigned int)(unsigned char)c;
}

/*
 * Mach console entry points.
 */
void
cnputc(char c, vm_offset_t arg)
{
	if (c == '\n')
		uart_putc('\r');
	uart_putc(c);
}

int
cngetc(void)
{
	while (UART_REG(UART_FR) & UART_FR_RXFE)
		;
	return (int)(UART_REG(UART_DR) & 0xffu);
}

int
cnmaygetc(void)
{
	if (UART_REG(UART_FR) & UART_FR_RXFE)
		return -1;
	return (int)(UART_REG(UART_DR) & 0xffu);
}
