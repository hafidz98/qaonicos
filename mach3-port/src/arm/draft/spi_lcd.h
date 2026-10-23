/*
 * spi_lcd.h -- DRAFT (GUI hardware track): SPI0 + ST7789 LCD.
 *
 * STATUS: written from the RV1106 device tree (SPI0 at
 * 0xFF500000, rk3066-spi compatible; GPIO0 at 0xFF380000) and
 * the standard ST7789 init sequence for 240x240 panels.
 * NOT integrated, NOT run: QEMU virt has no SPI0/ST7789.
 * Promotion path per the UI architecture decision (2026-09-28):
 * kernel exposes a generic SYS_SPI_XFER syscall (mechanism),
 * the display init + flush live in user space (policy) inside
 * the face program, which already owns the display on QEMU.
 * Bring-up: hardware phase H1.3.
 */
#ifndef QAONIC_DRAFT_SPI_LCD_H
#define QAONIC_DRAFT_SPI_LCD_H

/* Kernel-side mechanism (draft spi0.c). */
int	spi0_init(unsigned int baud_div);
int	spi0_tx(const unsigned char *buf, unsigned int len);

/* Board wiring (Luckfox Pico Mini, TODO(HW) confirm against the
 * schematic + the pins actually broken out on Mini B):
 *   LCD SCLK/MOSI = SPI0 CLK/TX, CS = SPI0 CS0,
 *   DC = a GPIO0 pin, RST = a GPIO0 pin, BL = a GPIO0 pin. */
#define LCD_DC_PIN	0	/* TODO(HW) */
#define LCD_RST_PIN	0	/* TODO(HW) */
#define LCD_BL_PIN	0	/* TODO(HW) */

#endif
