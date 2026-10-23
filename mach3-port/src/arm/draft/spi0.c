/*
 * spi0.c -- DRAFT (GUI hardware track): Rockchip SPI0 master,
 * TX-only polled, for the ST7789 LCD path. See spi_lcd.h.
 *
 * Register map: rockchip,rk3066-spi (Linux spi-rockchip.c).
 * TODO(HW): clock enable + rate setup live in the CRU; the
 * offsets must be taken from the RV1103 TRM/CRU driver on the
 * board (same lesson as the audio clock TODO in D2). Until
 * then spi0_init takes a raw BAUDR divider and assumes the
 * bootloader left SCLK_SPI0 enabled.
 */

#include "spi_lcd.h"

#define SPI0_BASE	0xFF500000u
#define REG(x)		(*(volatile unsigned int *)(SPI0_BASE + (x)))

#define SPI_CTRLR0	0x00
#define SPI_ENR		0x08
#define SPI_SER		0x0C
#define SPI_BAUDR	0x14
#define SPI_TXFTLR	0x18
#define SPI_SR		0x28
#define SPI_TXDR	0x400

#define SR_TX_EMPTY	(1u << 2)
#define SR_BUSY		(1u << 0)

int
spi0_init(unsigned int baud_div)
{
	REG(SPI_ENR) = 0;
	/* Master, SPI mode 0 (CPOL=0, CPHA=0), 8-bit frames,
	 * TX-only (DFS=7, TMOD=01b in CTRLR0[11:10]... encoding
	 * per rk3066: CTRLR0 bits: DFS[3:0]=7, TMOD[9:8]=1). */
	REG(SPI_CTRLR0) = 7u | (1u << 8);
	REG(SPI_BAUDR) = baud_div;	/* SCLK = clk_spi / div */
	REG(SPI_TXFTLR) = 0;
	REG(SPI_SER) = 1;		/* CS0 */
	REG(SPI_ENR) = 1;
	return 0;
}

int
spi0_tx(const unsigned char *buf, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		while (!(REG(SPI_SR) & SR_TX_EMPTY))
			;
		REG(SPI_TXDR) = buf[i];
	}
	while (REG(SPI_SR) & SR_BUSY)
		;
	return 0;
}
