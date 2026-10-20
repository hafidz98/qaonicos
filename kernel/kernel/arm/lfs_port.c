/*
 * kernel/kernel/arm/lfs_port.c -- littlefs port for QaonicOS (B1).
 *
 * Backend: RAM block device (1 MB = 256 blocks x 4096 B) so the
 * filesystem logic + power-cut behaviour are testable in QEMU
 * without NAND hardware. The block callbacks are isolated: the
 * SPI NAND backend (B1-HW) replaces lfsbd_* without touching the
 * littlefs glue above it.
 *
 * Power-cut simulation: lfsbd_cut_after(n) drops all programming
 * after n more bytes, mimicking power loss mid-write. The test
 * then discards the lfs state (no unmount/sync) and remounts:
 * littlefs must mount cleanly with old-or-new data, never garbage.
 *
 * Self-test stages (bitmask returned by lfs_run_test):
 *   bit0 format+mount ok
 *   bit1 write -> unmount -> remount -> read verify ok
 *   bit2 remount after simulated power cut ok
 *   bit3 pre-cut file still intact after the cut
 *   bit4 normal write/read works after recovery
 * PASS mask = 0x1f.
 */

#include "littlefs/lfs.h"

/* Freestanding string functions (kernel has strlen only).
 * Byte loops: simple and correct; speed is irrelevant here. */
void *
memcpy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	unsigned int i;

	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

void *
memmove(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	unsigned int i;

	if (d < s) {
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		for (i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return dst;
}

void *
memset(void *dst, int c, unsigned int n)
{
	unsigned char *d = dst;
	unsigned int i;

	for (i = 0; i < n; i++)
		d[i] = (unsigned char)c;
	return dst;
}

int
memcmp(const void *a, const void *b, unsigned int n)
{
	const unsigned char *x = a, *y = b;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (x[i] != y[i])
			return (int)x[i] - (int)y[i];
	}
	return 0;
}

char *
strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (*s == '\0')
			return 0;
	}
}

static int
str_in_set(char ch, const char *set)
{
	for (; *set; set++)
		if (*set == ch)
			return 1;
	return 0;
}

unsigned int
strspn(const char *s, const char *set)
{
	unsigned int n = 0;

	while (s[n] && str_in_set(s[n], set))
		n++;
	return n;
}

unsigned int
strcspn(const char *s, const char *set)
{
	unsigned int n = 0;

	while (s[n] && !str_in_set(s[n], set))
		n++;
	return n;
}

/* --- tiny static pool behind malloc/free for littlefs --- */
static unsigned char lfs_pool[4096];
static unsigned int lfs_pool_off;

void *
malloc(unsigned int size)
{
	void *p;

	size = (size + 7u) & ~7u;
	if (lfs_pool_off + size > sizeof(lfs_pool))
		return 0;
	p = &lfs_pool[lfs_pool_off];
	lfs_pool_off += size;
	return p;
}

void
free(void *p)
{
	(void)p;	/* pool is bump-only; buffers are static anyway */
}

/* --- RAM block device --- */
#define LFSBD_BLOCK_SIZE	4096u
#define LFSBD_BLOCK_COUNT	256u
#define LFSBD_SIZE		(LFSBD_BLOCK_SIZE * LFSBD_BLOCK_COUNT)

static unsigned char lfsbd_disk[LFSBD_SIZE];
static int lfsbd_live;		/* 0 = power "off" (progs dropped) */
static long lfsbd_cut_budget;	/* bytes left before the cut; -1 = never */

static void
lfsbd_erase_all(void)
{
	unsigned int i;

	for (i = 0; i < LFSBD_SIZE; i++)
		lfsbd_disk[i] = 0xff;
}

static int
lfsbd_read(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, void *buffer, lfs_size_t size)
{
	unsigned char *dst = buffer;
	unsigned int i, addr;

	(void)c;
	addr = block * LFSBD_BLOCK_SIZE + off;
	for (i = 0; i < size; i++)
		dst[i] = lfsbd_disk[addr + i];
	return 0;
}

static int
lfsbd_prog(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, const void *buffer, lfs_size_t size)
{
	const unsigned char *src = buffer;
	unsigned int i, addr;

	(void)c;
	if (!lfsbd_live)
		return 0;	/* power is gone: silently dropped */
	addr = block * LFSBD_BLOCK_SIZE + off;
	for (i = 0; i < size; i++) {
		if (lfsbd_cut_budget == 0) {
			lfsbd_live = 0;	/* the cut happens here */
			return 0;
		}
		if (lfsbd_cut_budget > 0)
			lfsbd_cut_budget--;
		/* NAND semantics: prog can only clear bits. */
		lfsbd_disk[addr + i] &= src[i];
	}
	return 0;
}

static int
lfsbd_erase(const struct lfs_config *c, lfs_block_t block)
{
	unsigned int i, addr;

	(void)c;
	if (!lfsbd_live)
		return 0;
	addr = block * LFSBD_BLOCK_SIZE;
	for (i = 0; i < LFSBD_BLOCK_SIZE; i++)
		lfsbd_disk[addr + i] = 0xff;
	return 0;
}

static int
lfsbd_sync(const struct lfs_config *c)
{
	(void)c;
	return 0;
}

/* --- littlefs glue --- */
static lfs_t lfs_vfs;
static struct lfs_config lfs_cfg;
static unsigned char lfs_read_buf[256];
static unsigned char lfs_prog_buf[256];
static unsigned char lfs_look_buf[256];
static int lfs_mounted;

static void
lfs_cfg_init(void)
{
	lfs_cfg.context = 0;
	lfs_cfg.read = lfsbd_read;
	lfs_cfg.prog = lfsbd_prog;
	lfs_cfg.erase = lfsbd_erase;
	lfs_cfg.sync = lfsbd_sync;
	lfs_cfg.read_size = 256;
	lfs_cfg.prog_size = 256;
	lfs_cfg.block_size = LFSBD_BLOCK_SIZE;
	lfs_cfg.block_count = LFSBD_BLOCK_COUNT;
	lfs_cfg.cache_size = 256;
	lfs_cfg.lookahead_size = 256;
	lfs_cfg.block_cycles = 500;
	lfs_cfg.read_buffer = lfs_read_buf;
	lfs_cfg.prog_buffer = lfs_prog_buf;
	lfs_cfg.lookahead_buffer = lfs_look_buf;
}

/* Write len bytes of a repeating pattern to path, sync, close. */
static int
lfs_write_pattern(const char *path, unsigned int len, unsigned char seed)
{
	lfs_file_t f;
	unsigned char buf[128];
	unsigned int i, done = 0;
	int err;

	err = lfs_file_open(&lfs_vfs, &f, path,
	    LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
	if (err)
		return err;
	while (done < len) {
		unsigned int n = len - done;

		if (n > sizeof(buf))
			n = sizeof(buf);
		for (i = 0; i < n; i++)
			buf[i] = (unsigned char)(seed + done + i);
		if (lfs_file_write(&lfs_vfs, &f, buf, n) < 0) {
			lfs_file_close(&lfs_vfs, &f);
			return -1;
		}
		done += n;
	}
	err = lfs_file_close(&lfs_vfs, &f);
	return err;
}

/* Verify path holds exactly the pattern written above. */
static int
lfs_verify_pattern(const char *path, unsigned int len, unsigned char seed)
{
	lfs_file_t f;
	unsigned char buf[128];
	unsigned int done = 0;
	int err;

	err = lfs_file_open(&lfs_vfs, &f, path, LFS_O_RDONLY);
	if (err)
		return err;
	while (done < len) {
		lfs_ssize_t r = lfs_file_read(&lfs_vfs, &f, buf, sizeof(buf));
		unsigned int i;

		if (r <= 0) {
			lfs_file_close(&lfs_vfs, &f);
			return -1;
		}
		for (i = 0; i < (unsigned int)r; i++) {
			if (buf[i] != (unsigned char)(seed + done + i)) {
				lfs_file_close(&lfs_vfs, &f);
				return -2;
			}
		}
		done += (unsigned int)r;
	}
	lfs_file_close(&lfs_vfs, &f);
	return done == len ? 0 : -3;
}

int
lfs_run_test(void)
{
	int mask = 0;
	int err;

	lfs_cfg_init();
	lfsbd_erase_all();
	lfsbd_live = 1;
	lfsbd_cut_budget = -1;
	lfs_mounted = 0;

	/* bit0: format + mount */
	err = lfs_format(&lfs_vfs, &lfs_cfg);
	if (err)
		return mask;
	err = lfs_mount(&lfs_vfs, &lfs_cfg);
	if (err)
		return mask;
	lfs_mounted = 1;
	mask |= 1;

	/* bit1: write, unmount, remount, verify */
	if (lfs_write_pattern("hello.txt", 1000, 0x10) == 0) {
		lfs_unmount(&lfs_vfs);
		lfs_mounted = 0;
		if (lfs_mount(&lfs_vfs, &lfs_cfg) == 0) {
			lfs_mounted = 1;
			if (lfs_verify_pattern("hello.txt", 1000, 0x10) == 0)
				mask |= 2;
		}
	}
	if (!lfs_mounted)
		return mask;

	/* bit2+bit3: power cut during a second write, then remount */
	lfsbd_cut_budget = 300;	/* die 300 prog-bytes into the write */
	(void)lfs_write_pattern("big.bin", 8192, 0x40);
	/* Simulated crash: state dropped without unmount/sync. */
	lfs_mounted = 0;
	lfsbd_live = 1;		/* power returns */
	lfsbd_cut_budget = -1;
	err = lfs_mount(&lfs_vfs, &lfs_cfg);
	if (err == 0) {
		lfs_mounted = 1;
		mask |= 4;
		if (lfs_verify_pattern("hello.txt", 1000, 0x10) == 0)
			mask |= 8;
	}
	if (!lfs_mounted)
		return mask;

	/* bit4: filesystem still fully usable after recovery */
	if (lfs_write_pattern("after.txt", 500, 0x70) == 0 &&
	    lfs_verify_pattern("after.txt", 500, 0x70) == 0)
		mask |= 16;

	lfs_unmount(&lfs_vfs);
	lfs_mounted = 0;
	return mask;
}
