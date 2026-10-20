/*
 * st7789.h -- DRAFT (GUI hardware track): ST7789 240x240 panel
 * bring-up data for the face's hardware display path.
 *
 * The face renderer already produces an RGB565 framebuffer on
 * QEMU (virtio-gpu). On hardware the same framebuffer is pushed
 * to the panel: set the CASET/RASET window, send RAMWR, then
 * stream the pixels with DC=1 (see spi_lcd.h for the driver
 * draft). Dirty-region flushing maps 1:1 onto sub-windows.
 *
 * Init sequence below is the canonical one for the common
 * 1.3" 240x240 ST7789 modules. TODO(HW): gamma/porch values
 * vary per module vendor; verify against the actual panel
 * (H1.3) -- wrong values still show an image, just washed out.
 */
#ifndef QAONIC_DRAFT_ST7789_H
#define QAONIC_DRAFT_ST7789_H

#define ST7789_W	240
#define ST7789_H	240

/* Command bytes: {cmd, len, args...}; len 0xFF = delay marker
 * (next byte = ms), 0xFE = end of table. */
static const unsigned char st7789_init_seq[] = {
	0x01, 0xFF, 150,		/* SWRESET, wait 150 ms */
	0x11, 0xFF, 120,		/* SLPOUT, wait 120 ms */
	0x3A, 1, 0x55,			/* COLMOD: RGB565 */
	0x36, 1, 0x00,			/* MADCTL: no rotation */
	0x2A, 4, 0x00, 0x00, 0x00, 0xEF,	/* CASET 0..239 */
	0x2B, 4, 0x00, 0x00, 0x00, 0xEF,	/* RASET 0..239 */
	0x21, 0,			/* INVON (most 240x240 IPS) */
	0x13, 0,			/* NORON */
	0x29, 0xFF, 20,			/* DISPON, wait 20 ms */
	0xFE
};

#define ST7789_CMD_CASET	0x2A
#define ST7789_CMD_RASET	0x2B
#define ST7789_CMD_RAMWR	0x2C

#endif
