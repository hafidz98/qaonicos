/*
 * sfc_nand.h -- DRAFT (B1-HW): SPI NAND driver for the Luckfox
 * Pico Mini B (Rockchip SFC + Winbond W25N01GV).
 *
 * STATUS: written from the RV1106 device tree + the shipping
 * Rockchip U-Boot SFC driver + the W25N01GV command set.
 * NOT integrated, NOT run: QEMU has no SFC, so this file is
 * compile-checked only. Bring-up happens at hardware phase H1
 * (docs/rencana-uji-hardware/RENCANA.md). TODO(HW) marks every
 * spot that needs confirmation on the real board.
 */
#ifndef QAONIC_DRAFT_SFC_NAND_H
#define QAONIC_DRAFT_SFC_NAND_H

/* Geometry of the W25N01GV (128 MB). */
#define NAND_PAGE_SIZE		2048u
#define NAND_PAGES_PER_BLOCK	64u
#define NAND_BLOCK_SIZE		(NAND_PAGE_SIZE * NAND_PAGES_PER_BLOCK)
#define NAND_BLOCK_COUNT	1024u

int	sfc_nand_init(void);		/* reset + read ID; 0 ok */
unsigned int sfc_nand_id(void);		/* cached JEDEC ID */

/* littlefs-compatible block backend over a block range.
 * first_block skips the boot partitions (see sfc_nand.c). */
int	nand_read(unsigned int block, unsigned int page_off,
	    void *dst, unsigned int len);
int	nand_prog(unsigned int block, unsigned int page_off,
	    const void *src, unsigned int len);
int	nand_erase(unsigned int block);
int	nand_is_bad(unsigned int block);

#endif
