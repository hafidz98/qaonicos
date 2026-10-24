/*
 * mach3-port/src/ext/lfs_port.c -- littlefs port for QaonicOS (B1).
 *
 * Two block-device backends behind the lfsbd_* callbacks:
 *
 *   RAM (B1): 1 MB array (256 blocks x 4096 B) with a power-cut
 *   simulator -- lfsbd_cut_after(n) drops all programming after
 *   n more bytes, mimicking power loss mid-write; the test then
 *   discards the lfs state (no unmount/sync) and remounts:
 *   littlefs must mount cleanly with old-or-new data, never
 *   garbage. The B1 self-test (lfs_run_test) always runs on
 *   this backend, so it can never erase the real /data.
 *
 *   BLK (FND-4): a reserved region of blk dev 0, so /data
 *   survives QEMU restarts and the provisioning / credential /
 *   onboarding flows can be regression-tested across boots.
 *   Region layout (sectors of 512 B, tail-anchored exactly
 *   like bootstate's own reservations):
 *     [total-2066, total-18)  this region: 2048 sectors
 *                             = 256 blocks x 4096 B (same
 *                             geometry as the RAM backend)
 *     [total-18,  total-2)    bootlog ring (16 sectors)
 *     total-2                 bootstate state sector
 *     total-1                 blk selftest scratch
 *   On the standard 16 MiB image (32768 sectors) the region
 *   is sectors 30702..32749. NAND semantics are preserved
 *   (erase -> 0xFF, prog only clears bits) so littlefs sees
 *   the same rules as on hardware; the B1-HW NAND backend
 *   (draft/sfc_nand.c, NAND blocks 320..999) later replaces
 *   the blk half without touching the littlefs glue.
 *
 * /data mounts on BLK by default; if the blk device is
 * absent or too small, the port falls back to RAM with a
 * boot-time warning (a no-disk boot keeps a working /data).
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

/* --- tiny free-list allocator behind malloc/free for littlefs ---
 *
 * Was a 4 KB bump pool with a no-op free(): every lfs_file_open
 * leaked its 256 B cache forever, so after ~16 file operations
 * (across the whole boot) all config reads/writes failed --
 * found by ONB-1, where the device PIN "disappeared" after a
 * few web requests. This is a first-fit allocator with real
 * free + coalescing over an 8 KB static pool; peak concurrent
 * use is a handful of cache blocks. */
#define LFS_POOL_SIZE	32768u
#define BLK_HDR	8u	/* size word + tag word */

/* aligned(8) is load-bearing: a plain char array can land on an
 * odd BSS address (it did -- 0x...aa1) and the header accesses
 * below fault on unaligned strd. */
static unsigned char lfs_pool[LFS_POOL_SIZE]
    __attribute__((aligned(8)));
static int lfs_pool_ready;

static unsigned int *
blk_word(unsigned int off, unsigned int w)
{
	return (unsigned int *)(void *)(lfs_pool + off + w * 4u);
}

void *
malloc(unsigned int size)
{
	unsigned int off = 0, want;

	if (!lfs_pool_ready) {
		*blk_word(0, 0) = LFS_POOL_SIZE;	/* free */
		*blk_word(0, 1) = 0;
		lfs_pool_ready = 1;
	}
	want = ((size + BLK_HDR) + 7u) & ~7u;
	if (want < 16u)
		want = 16u;
	while (off + BLK_HDR <= LFS_POOL_SIZE) {
		unsigned int raw = *blk_word(off, 0);
		unsigned int bsz = raw & ~1u;
		if (bsz < 16u)
			break;
		if (!(raw & 1u) && bsz >= want) {
			if (bsz - want >= 16u) {
				*blk_word(off + want, 0) = bsz - want;
				*blk_word(off + want, 1) = 0;
				*blk_word(off, 0) = want | 1u;
			} else {
				*blk_word(off, 0) = bsz | 1u;
			}
			*blk_word(off, 1) = 0xA110CA7Eu;
			return (void *)(lfs_pool + off + BLK_HDR);
		}
		off += bsz;
	}
	return 0;
}

void
free(void *p)
{
	unsigned int off, cur;

	if (!p || !lfs_pool_ready)
		return;
	off = (unsigned int)((unsigned char *)p - lfs_pool) - BLK_HDR;
	if (off >= LFS_POOL_SIZE || *blk_word(off, 1) != 0xA110CA7Eu)
		return;	/* not one of ours */
	*blk_word(off, 0) &= ~1u;
	*blk_word(off, 1) = 0;
	/* Coalesce: merge adjacent free blocks in one pass. */
	cur = 0;
	while (cur + BLK_HDR <= LFS_POOL_SIZE) {
		unsigned int raw = *blk_word(cur, 0);
		unsigned int bsz = raw & ~1u;
		if (bsz < 16u)
			break;
		if (!(raw & 1u)) {
			unsigned int nxt = cur + bsz;
			while (nxt + BLK_HDR <= LFS_POOL_SIZE) {
				unsigned int nraw = *blk_word(nxt, 0);
				unsigned int nbsz = nraw & ~1u;
				if (nbsz < 16u || (nraw & 1u))
					break;
				bsz += nbsz;
				*blk_word(cur, 0) = bsz;
				nxt = cur + bsz;
			}
		}
		cur += bsz;
	}
}

/* --- block-device backends (RAM + persistent blk) --- */
#define LFSBD_BLOCK_SIZE	4096u
#define LFSBD_BLOCK_COUNT	256u
#define LFSBD_SIZE		(LFSBD_BLOCK_SIZE * LFSBD_BLOCK_COUNT)

extern int printf(const char *, ...);
extern int blk_read_sector(unsigned int sector, unsigned char *data);
extern int blk_write_sector(unsigned int sector,
    const unsigned char *data);
extern unsigned int blk_total_sectors(void);

/* Backend ids for lfsbd_kind. */
#define LFSBD_RAM	0
#define LFSBD_BLK	1

/* The /data default is the persistent backend (FND-4); the
 * self-test pins RAM for itself (see lfs_run_test). */
static int lfsbd_kind = LFSBD_BLK;

/* --- RAM backend --- */
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
ram_read(const struct lfs_config *c, lfs_block_t block,
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
ram_prog(const struct lfs_config *c, lfs_block_t block,
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
ram_erase(const struct lfs_config *c, lfs_block_t block)
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

/* --- blk backend (FND-4): reserved region of blk dev 0 --- */
#define LFSB_SECTOR_SIZE	512u
#define LFSB_SECT_PER_BLOCK	(LFSBD_BLOCK_SIZE / LFSB_SECTOR_SIZE)
#define LFSB_REGION_SECTORS	(LFSBD_BLOCK_COUNT * LFSB_SECT_PER_BLOCK)
/* Tail reservations owned by others (see bootstate.c/blk.c):
 * 16 bootlog sectors + 1 state sector + 1 selftest scratch. */
#define LFSB_TAIL_SECTORS	18u

static unsigned char lfsb_sec[LFSB_SECTOR_SIZE]
    __attribute__((aligned(16)));

/* First sector of the littlefs region on blk dev 0. */
static unsigned int
lfsb_base(void)
{
	return blk_total_sectors() - LFSB_TAIL_SECTORS -
	    LFSB_REGION_SECTORS;
}

/* The blk device must exist and be big enough to hold the
 * region plus the tail reservations (with slack below the
 * region for whatever a future layout puts at the head). */
static int
lfsb_ok(void)
{
	return blk_total_sectors() >=
	    LFSB_REGION_SECTORS + LFSB_TAIL_SECTORS + 64u;
}

/* Bootstate's B2.8 lesson applies here too: blk requests can
 * time out transiently under host CPU contention, so all
 * region I/O goes through 3-attempt wrappers. */
static int
lfsb_read_sec(unsigned int sec, unsigned char *buf)
{
	int i;

	for (i = 0; i < 3; i++)
		if (blk_read_sector(sec, buf) == 0)
			return 0;
	return -1;
}

static int
lfsb_write_sec(unsigned int sec, const unsigned char *buf)
{
	int i;

	for (i = 0; i < 3; i++)
		if (blk_write_sector(sec, buf) == 0)
			return 0;
	return -1;
}

static int
blk_read_bd(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, void *buffer, lfs_size_t size)
{
	unsigned char *dst = buffer;
	unsigned int addr, done = 0;

	(void)c;
	addr = lfsb_base() * LFSB_SECTOR_SIZE +
	    block * LFSBD_BLOCK_SIZE + off;
	while (done < size) {
		unsigned int sec = (addr + done) / LFSB_SECTOR_SIZE;
		unsigned int soff = (addr + done) % LFSB_SECTOR_SIZE;
		unsigned int n = LFSB_SECTOR_SIZE - soff;
		unsigned int i;

		if (n > size - done)
			n = size - done;
		if (lfsb_read_sec(sec, lfsb_sec) != 0)
			return -5;	/* LFS_ERR_IO */
		for (i = 0; i < n; i++)
			dst[done + i] = lfsb_sec[soff + i];
		done += n;
	}
	return 0;
}

static int
blk_prog_bd(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, const void *buffer, lfs_size_t size)
{
	const unsigned char *src = buffer;
	unsigned int addr, done = 0;

	(void)c;
	addr = lfsb_base() * LFSB_SECTOR_SIZE +
	    block * LFSBD_BLOCK_SIZE + off;
	while (done < size) {
		unsigned int sec = (addr + done) / LFSB_SECTOR_SIZE;
		unsigned int soff = (addr + done) % LFSB_SECTOR_SIZE;
		unsigned int n = LFSB_SECTOR_SIZE - soff;
		unsigned int i;

		if (n > size - done)
			n = size - done;
		if (lfsb_read_sec(sec, lfsb_sec) != 0)
			return -5;	/* LFS_ERR_IO */
		/* NAND semantics: prog can only clear bits. The
		 * medium is a disk image, so this is a
		 * read-modify-write that ANDs the new data in --
		 * littlefs must never observe a 0 -> 1 transition
		 * without an erase, exactly as on NAND. */
		for (i = 0; i < n; i++)
			lfsb_sec[soff + i] &= src[done + i];
		if (lfsb_write_sec(sec, lfsb_sec) != 0)
			return -5;	/* LFS_ERR_IO */
		done += n;
	}
	return 0;
}

static int
blk_erase_bd(const struct lfs_config *c, lfs_block_t block)
{
	unsigned int i, sec;

	(void)c;
	for (i = 0; i < LFSB_SECTOR_SIZE; i++)
		lfsb_sec[i] = 0xff;
	sec = lfsb_base() + block * LFSB_SECT_PER_BLOCK;
	for (i = 0; i < LFSB_SECT_PER_BLOCK; i++)
		if (lfsb_write_sec(sec + i, lfsb_sec) != 0)
			return -5;	/* LFS_ERR_IO */
	return 0;
}

/* --- dispatchers: the callbacks littlefs is configured with --- */
static int
lfsbd_read(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, void *buffer, lfs_size_t size)
{
	if (lfsbd_kind == LFSBD_BLK)
		return blk_read_bd(c, block, off, buffer, size);
	return ram_read(c, block, off, buffer, size);
}

static int
lfsbd_prog(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, const void *buffer, lfs_size_t size)
{
	if (lfsbd_kind == LFSBD_BLK)
		return blk_prog_bd(c, block, off, buffer, size);
	return ram_prog(c, block, off, buffer, size);
}

static int
lfsbd_erase(const struct lfs_config *c, lfs_block_t block)
{
	if (lfsbd_kind == LFSBD_BLK)
		return blk_erase_bd(c, block);
	return ram_erase(c, block);
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

	/* FND-4: the self-test always runs on the RAM backend --
	 * it formats its medium and simulates power cuts on it,
	 * so it must never touch the persistent /data region.
	 * If /data is mounted, unmount it first: its state is
	 * on blk, and the next lfs_port_mount() re-picks the
	 * default backend and remounts it. (Pre-FND-4 the test
	 * formatted the same RAM disk /data later mounted; that
	 * coupling is gone.) */
	if (lfs_mounted) {
		lfs_unmount(&lfs_vfs);
		lfs_mounted = 0;
	}
	lfsbd_kind = LFSBD_RAM;
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

/* --- File API for the shell (/data) --- */

/* Mount the data filesystem; format on first use. 0 ok, -1 fail. */
int
lfs_port_mount(void)
{
	int err;

	if (lfs_mounted)
		return 0;
	/* FND-4: /data defaults to the persistent blk backend;
	 * fall back to RAM (with a warning) when the blk device
	 * is absent or too small for the reserved region, so a
	 * no-disk boot still gets a working /data. */
	lfsbd_kind = LFSBD_BLK;
	if (!lfsb_ok()) {
		printf("lfs: blk absent/small (%u sectors) -- "
		    "/data on RAM backend (volatile)\n",
		    blk_total_sectors());
		lfsbd_kind = LFSBD_RAM;
	}
	lfs_cfg_init();
	lfsbd_live = 1;
	lfsbd_cut_budget = -1;
	err = lfs_mount(&lfs_vfs, &lfs_cfg);
	if (err) {
		err = lfs_format(&lfs_vfs, &lfs_cfg);
		if (err)
			return -1;
		err = lfs_mount(&lfs_vfs, &lfs_cfg);
		if (err)
			return -1;
	}
	lfs_mounted = 1;
	if (lfsbd_kind == LFSBD_BLK)
		printf("lfs: /data on blk region sectors %u..%u\n",
		    lfsb_base(), lfsb_base() + LFSB_REGION_SECTORS - 1u);
	else
		printf("lfs: /data on RAM backend\n");
	return 0;
}

/* List root entries, one "name\n" per line. Returns bytes or -1. */
int
lfs_port_list(char *dst, unsigned int max)
{
	lfs_dir_t d;
	struct lfs_info info;
	unsigned int pos = 0;
	int err;

	if (!lfs_mounted && lfs_port_mount() != 0)
		return -1;
	err = lfs_dir_open(&lfs_vfs, &d, "/");
	if (err)
		return -1;
	for (;;) {
		unsigned int i;

		err = lfs_dir_read(&lfs_vfs, &d, &info);
		if (err <= 0)
			break;
		if (info.name[0] == '.')
			continue;
		for (i = 0; info.name[i]; i++) {
			if (pos + 2 >= max)
				break;
			dst[pos++] = info.name[i];
		}
		if (pos + 1 < max)
			dst[pos++] = '\n';
	}
	lfs_dir_close(&lfs_vfs, &d);
	if (pos < max)
		dst[pos] = '\0';
	return (int)pos;
}

/* Read a whole file. Returns length or -1. */
int
lfs_port_read(const char *path, unsigned char *dst, unsigned int max)
{
	lfs_file_t f;
	lfs_ssize_t r;
	int err;

	if (!lfs_mounted && lfs_port_mount() != 0)
		return -1;
	err = lfs_file_open(&lfs_vfs, &f, path, LFS_O_RDONLY);
	if (err)
		return -1;
	r = lfs_file_read(&lfs_vfs, &f, dst, max);
	lfs_file_close(&lfs_vfs, &f);
	if (r < 0)
		return -1;
	return (int)r;
}

/* Write (create/truncate) a whole file. 0 ok, -1 fail. */
int
lfs_port_write(const char *path, const unsigned char *src,
    unsigned int len)
{
	lfs_file_t f;
	int err;

	/* Mount on demand: the boot self-test leaves the FS
	 * unmounted by design, and kernel-side consumers (web UI
	 * config) must not depend on some other client having
	 * mounted it first. (ONB-1 regression hunt, 2026-10-22:
	 * PIN writes silently failed on boots where nothing else
	 * had mounted the filesystem.) */
	if (!lfs_mounted && lfs_port_mount() != 0)
		return -1;
	err = lfs_file_open(&lfs_vfs, &f, path,
	    LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
	if (err)
		return -1;
	if (len && lfs_file_write(&lfs_vfs, &f, src, len) < 0) {
		lfs_file_close(&lfs_vfs, &f);
		return -1;
	}
	err = lfs_file_close(&lfs_vfs, &f);
	return err ? -1 : 0;
}
