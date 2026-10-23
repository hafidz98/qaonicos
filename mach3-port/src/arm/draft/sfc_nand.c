/*
 * sfc_nand.c -- DRAFT (B1-HW): see sfc_nand.h for status.
 *
 * Layers:
 *   1. SFC controller primitives (PIO, FIFO) at 0xFFAC0000.
 *      Register map taken from the Rockchip U-Boot driver
 *      (sysdrv/source/uboot/.../drivers/spi/rockchip_sfc.c).
 *   2. W25N01GV command layer (page read / program / erase,
 *      on-chip ECC enabled, bad-block marker checks).
 *   3. littlefs-shaped backend: when this is promoted out of
 *      draft/, lfs_port.c swaps its RAM arrays for these calls
 *      (lfs geometry: read/prog 2048, block 128K, count =
 *      NAND_LFS_BLOCKS below).
 *
 * Partition choice: parameter.txt gives rootfs at NAND byte
 * offset 0x14800 * 512 = 41,943,040 = block 320, length 680
 * blocks. The /data filesystem lives there (rootfs itself is
 * still the B0 placeholder).
 */

#include "sfc_nand.h"

#define SFC_BASE	0xFFAC0000u
#define REG(x)		(*(volatile unsigned int *)(SFC_BASE + (x)))

#define SFC_CTRL	0x00
#define SFC_ICLR	0x08
#define SFC_RCVR	0x10
#define SFC_FSR		0x20
#define SFC_SR		0x24
#define SFC_CMD		0x100
#define SFC_ADDR	0x104
#define SFC_DATA	0x108

#define SFC_CMD_DIR_RD	0u
#define SFC_CMD_DIR_WR	1u

/* W25N01GV opcodes. */
#define CMD_RESET	0xFF
#define CMD_READ_ID	0x9F
#define CMD_GET_FEATURE	0x0F
#define CMD_SET_FEATURE	0x1F
#define CMD_WRITE_ENABLE 0x06
#define CMD_PAGE_READ	0x13	/* row -> cache */
#define CMD_READ_CACHE	0x03	/* cache -> host (1-bit) */
#define CMD_PROG_LOAD	0x02	/* host -> cache */
#define CMD_PROG_EXEC	0x10	/* cache -> row */
#define CMD_BLOCK_ERASE	0xD8

#define FEAT_BLOCK_LOCK	0xA0
#define FEAT_CONFIG	0xB0	/* bit4 ECC-E, bit3 BUF */
#define FEAT_STATUS	0xC0	/* bit0 OIP, bit2 EFAIL, bit3 PFAIL,
				 * bits 5:4 ECC status */
#define LFS_FIRST_BLOCK	320u
#define NAND_LFS_BLOCKS	680u

static unsigned int nand_id;

static void
sfc_wait_idle(void)
{
	/* TODO(HW): bound this with the kernel tick on hardware;
	 * a hung SFC must surface as an error, not a hang (B2). */
	while ((REG(SFC_SR) & 1u) != 0u)
		;
}

/*
 * One SPI transaction: opcode (+ optional address phase of
 * addr_bits, + dummy bytes handled by the caller as a separate
 * op when needed) and an optional FIFO data phase.
 * dir: SFC_CMD_DIR_RD (data into buf) / WR (data out of buf).
 * addr_len: 0, 1, 3 bytes typical for SPI NAND ops.
 *
 * TODO(HW): the CTRL packing below (1-bit cmd/addr/data) follows
 * the U-Boot driver's SFC v4+ path; confirm against SFC_VER on
 * the real SoC and against a logic-analyser trace of the stock
 * Luckfox firmware doing the same op.
 */
static int
sfc_op(unsigned char opcode, unsigned int addr, unsigned int addr_len,
    unsigned char *buf, unsigned int len, unsigned int dir)
{
	unsigned int cmd, ctrl, i;

	sfc_wait_idle();
	REG(SFC_ICLR) = 0xFFFFFFFFu;

	/* 1-bit phases; address bits: 0/8/24 mapped to the CMD
	 * field encoding (0=none, 1=24-bit, 2=32-bit, 3=X bits
	 * via SFC_ABIT). 8-bit addresses (feature ops) go through
	 * the X-bits path -- TODO(HW) confirm ABIT programming. */
	ctrl = 0;
	REG(SFC_CTRL) = ctrl;
	cmd = (unsigned int)opcode;
	cmd |= dir << 12;
	if (addr_len == 3u)
		cmd |= 1u << 14;
	else if (addr_len != 0u)
		cmd |= 3u << 14;
	cmd |= (len & 0x3FFFu) << 16;
	REG(SFC_ADDR) = addr;
	REG(SFC_CMD) = cmd;

	if (dir == SFC_CMD_DIR_WR) {
		for (i = 0; i < len; i++) {
			while (((REG(SFC_FSR) >> 8) & 0x1Fu) >= 0x1Fu)
				;
			REG(SFC_DATA) = buf[i];
		}
	} else {
		for (i = 0; i < len; i++) {
			while (((REG(SFC_FSR) >> 16) & 0x1Fu) == 0u)
				;
			buf[i] = (unsigned char)REG(SFC_DATA);
		}
	}
	sfc_wait_idle();
	return 0;
}

static int
nand_get_feature(unsigned char reg, unsigned char *val)
{
	return sfc_op(CMD_GET_FEATURE, reg, 1, val, 1, SFC_CMD_DIR_RD);
}

static int
nand_set_feature(unsigned char reg, unsigned char val)
{
	return sfc_op(CMD_SET_FEATURE, reg, 1, &val, 1, SFC_CMD_DIR_WR);
}

static int
nand_wait_ready(unsigned int *status_out)
{
	unsigned char st = 0xFF;

	/* OIP (bit0) clears when the operation completes.
	 * TODO(HW): add a timeout (~ms) from the kernel tick. */
	do {
		if (nand_get_feature(FEAT_STATUS, &st) != 0)
			return -1;
	} while (st & 1u);
	if (status_out)
		*status_out = st;
	return 0;
}

static int
nand_write_enable(void)
{
	return sfc_op(CMD_WRITE_ENABLE, 0, 0, (void *)0, 0,
	    SFC_CMD_DIR_WR);
}

int
sfc_nand_init(void)
{
	unsigned char id[3];
	unsigned char cfg;

	REG(SFC_RCVR) = 1u;		/* controller reset */
	while (REG(SFC_RCVR) & 1u)
		;
	sfc_op(CMD_RESET, 0, 0, (void *)0, 0, SFC_CMD_DIR_WR);
	nand_wait_ready((void *)0);

	id[0] = id[1] = id[2] = 0;
	/* TODO(HW): READ ID on the W25N needs one dummy byte
	 * after the opcode before data; fold it into the op's
	 * dummy phase when the SFC dummy handling is confirmed. */
	sfc_op(CMD_READ_ID, 0, 0, id, 3, SFC_CMD_DIR_RD);
	nand_id = (unsigned int)id[0] << 16 | (unsigned int)id[1] << 8 | id[2];
	if (id[0] != 0xEF)		/* Winbond */
		return -1;

	/* Unlock all blocks, enable on-chip ECC + buffered read
	 * mode (BUF=1): cache reads return the page buffer. */
	nand_set_feature(FEAT_BLOCK_LOCK, 0x00);
	if (nand_get_feature(FEAT_CONFIG, &cfg) == 0) {
		cfg |= (1u << 4) | (1u << 3);
		nand_set_feature(FEAT_CONFIG, cfg);
	}
	return 0;
}

unsigned int
sfc_nand_id(void)
{
	return nand_id;
}

/* Row address for PAGE_READ / PROG_EXEC / ERASE: block*64+page. */
static unsigned int
row_of(unsigned int block, unsigned int page)
{
	return block * NAND_PAGES_PER_BLOCK + page;
}

int
nand_is_bad(unsigned int block)
{
	/* Bad-block marker: first spare byte of the block's first
	 * page (column 2048). PAGE_READ then read 1 byte at 2048. */
	unsigned char m = 0;
	unsigned int st;

	if (sfc_op(CMD_PAGE_READ, row_of(block, 0), 3, (void *)0, 0,
	    SFC_CMD_DIR_WR) != 0)
		return 1;		/* unreadable: treat as bad */
	if (nand_wait_ready(&st) != 0)
		return 1;
	if (sfc_op(CMD_READ_CACHE, 2048, 2, &m, 1, SFC_CMD_DIR_RD) != 0)
		return 1;
	return m != 0xFF;
}

int
nand_read(unsigned int block, unsigned int page_off, void *dst,
    unsigned int len)
{
	/* page_off/len are in bytes within the block (lfs calls
	 * with page-aligned offsets; len may span pages). */
	unsigned char *d = (unsigned char *)dst;
	unsigned int done = 0;

	while (done < len) {
		unsigned int byte_in_block = page_off + done;
		unsigned int page = byte_in_block / NAND_PAGE_SIZE;
		unsigned int col = byte_in_block % NAND_PAGE_SIZE;
		unsigned int chunk = NAND_PAGE_SIZE - col;
		unsigned int st;

		if (chunk > len - done)
			chunk = len - done;
		if (sfc_op(CMD_PAGE_READ, row_of(block, page), 3,
		    (void *)0, 0, SFC_CMD_DIR_WR) != 0)
			return -1;
		if (nand_wait_ready(&st) != 0)
			return -1;
		/* ECC status bits 5:4: 0b10 = uncorrectable. */
		if (((st >> 4) & 3u) == 2u)
			return -2;
		if (sfc_op(CMD_READ_CACHE, col, 2, d + done, chunk,
		    SFC_CMD_DIR_RD) != 0)
			return -1;
		done += chunk;
	}
	return 0;
}

int
nand_prog(unsigned int block, unsigned int page_off, const void *src,
    unsigned int len)
{
	/* NAND rule (same as the RAM simulator in lfs_port.c):
	 * programming may only clear bits; the caller (littlefs)
	 * guarantees the page was erased. One page per PROG_EXEC. */
	const unsigned char *s = (const unsigned char *)src;
	unsigned int done = 0;

	while (done < len) {
		unsigned int byte_in_block = page_off + done;
		unsigned int page = byte_in_block / NAND_PAGE_SIZE;
		unsigned int col = byte_in_block % NAND_PAGE_SIZE;
		unsigned int chunk = NAND_PAGE_SIZE - col;
		unsigned int st;

		if (chunk > len - done)
			chunk = len - done;
		if (nand_write_enable() != 0)
			return -1;
		if (sfc_op(CMD_PROG_LOAD, col, 2,
		    (unsigned char *)(s + done), chunk,
		    SFC_CMD_DIR_WR) != 0)
			return -1;
		if (sfc_op(CMD_PROG_EXEC, row_of(block, page), 3,
		    (void *)0, 0, SFC_CMD_DIR_WR) != 0)
			return -1;
		if (nand_wait_ready(&st) != 0)
			return -1;
		if (st & (1u << 3))	/* P-FAIL */
			return -2;
		done += chunk;
	}
	return 0;
}

int
nand_erase(unsigned int block)
{
	unsigned int st;

	if (nand_is_bad(block))
		return -2;
	if (nand_write_enable() != 0)
		return -1;
	if (sfc_op(CMD_BLOCK_ERASE, row_of(block, 0), 3, (void *)0, 0,
	    SFC_CMD_DIR_WR) != 0)
		return -1;
	if (nand_wait_ready(&st) != 0)
		return -1;
	if (st & (1u << 2))		/* E-FAIL */
		return -2;
	return 0;
}
