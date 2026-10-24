/*
 * lfsimg.c -- host-side littlefs image tool (FND-5 test support).
 *
 * Mounts the /data littlefs volume inside a QaonicOS blk disk
 * image (the reserved tail region from lfs_port.c) using the
 * VENDORED littlefs sources -- the exact code the kernel runs,
 * with the exact lfs_config the kernel port uses (read/prog/
 * cache/lookahead 256, block 4096 x 256, block_cycles 500) --
 * so a volume written here is a volume the guest mounts, and
 * vice versa.
 *
 * The block device is the image file itself: read = pread,
 * prog = read-modify-write AND-ing the new bytes in (the
 * NAND rule the kernel BLK backend enforces: a prog never
 * turns a 0 bit into a 1), erase = fill 0xFF, sync = fflush.
 *
 * Region: 2048 sectors (256 blocks x 4096 B) ending 18
 * sectors below the end of the image (bootlog ring 16 +
 * bootstate state 1 + blk selftest scratch 1) -- the same
 * tail anchor as lfs_port.c. The image's sector count is
 * taken from its file size, so any image size works.
 *
 * Usage:
 *   lfsimg <img> ls
 *   lfsimg <img> put <lfs-name> <host-file>
 *   lfsimg <img> get <lfs-name> <host-file>
 *   lfsimg <img> del <lfs-name>
 *
 * Build (done by scripts/verify-fnd5.py):
 *   cc -O2 -I mach3-port/src/ext/littlefs scripts/lfsimg.c \
 *      mach3-port/src/ext/littlefs/lfs.c \
 *      mach3-port/src/ext/littlefs/lfs_util.c -o lfsimg
 *
 * If the volume does not mount (blank/erased region), it is
 * formatted first -- the same thing the guest does on first
 * mount -- and a note is printed to stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lfs.h"

#define BLK_SIZE	4096u
#define BLK_COUNT	256u
#define REGION_SECTORS	(BLK_SIZE * BLK_COUNT / 512u)	/* 2048 */
#define TAIL_SECTORS	18u

static FILE *g_img;
static long g_base;		/* byte offset of the region */

static int
bd_read(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, void *buffer, lfs_size_t size)
{
	(void)c;
	if (fseek(g_img, g_base + (long)block * BLK_SIZE + off,
	    SEEK_SET) != 0)
		return LFS_ERR_IO;
	if (fread(buffer, 1, size, g_img) != size)
		return LFS_ERR_IO;
	return 0;
}

static int
bd_prog(const struct lfs_config *c, lfs_block_t block,
    lfs_off_t off, const void *buffer, lfs_size_t size)
{
	static unsigned char cur[256];
	const unsigned char *src = buffer;
	lfs_size_t i;

	(void)c;
	/* AND semantics, exactly like the kernel BLK backend:
	 * programming can only clear bits. */
	if (fseek(g_img, g_base + (long)block * BLK_SIZE + off,
	    SEEK_SET) != 0)
		return LFS_ERR_IO;
	if (fread(cur, 1, size, g_img) != size)
		return LFS_ERR_IO;
	for (i = 0; i < size; i++)
		cur[i] = (unsigned char)(cur[i] & src[i]);
	if (fseek(g_img, g_base + (long)block * BLK_SIZE + off,
	    SEEK_SET) != 0)
		return LFS_ERR_IO;
	if (fwrite(cur, 1, size, g_img) != size)
		return LFS_ERR_IO;
	return 0;
}

static int
bd_erase(const struct lfs_config *c, lfs_block_t block)
{
	static unsigned char ff[BLK_SIZE];

	(void)c;
	memset(ff, 0xff, sizeof(ff));
	if (fseek(g_img, g_base + (long)block * BLK_SIZE,
	    SEEK_SET) != 0)
		return LFS_ERR_IO;
	if (fwrite(ff, 1, BLK_SIZE, g_img) != BLK_SIZE)
		return LFS_ERR_IO;
	return 0;
}

static int
bd_sync(const struct lfs_config *c)
{
	(void)c;
	return fflush(g_img) == 0 ? 0 : LFS_ERR_IO;
}

static unsigned char read_buf[256], prog_buf[256], look_buf[256];
static lfs_t lfs;
static struct lfs_config cfg;

static int
mount_or_format(void)
{
	int err = lfs_mount(&lfs, &cfg);

	if (err == 0)
		return 0;
	fprintf(stderr, "lfsimg: mount failed (%d); formatting\n", err);
	err = lfs_format(&lfs, &cfg);
	if (err != 0)
		return err;
	return lfs_mount(&lfs, &cfg);
}

static int
op_ls(void)
{
	lfs_dir_t dir;
	struct lfs_info info;
	int err = lfs_dir_open(&lfs, &dir, "/");

	if (err != 0)
		return err;
	while ((err = lfs_dir_read(&lfs, &dir, &info)) > 0) {
		if (info.type == LFS_TYPE_REG)
			printf("%s %u\n", info.name,
			    (unsigned)info.size);
	}
	lfs_dir_close(&lfs, &dir);
	return err < 0 ? err : 0;
}

static int
op_put(const char *name, const char *host)
{
	FILE *f = fopen(host, "rb");
	lfs_file_t file;
	unsigned char buf[1024];
	size_t n;
	int err;

	if (f == NULL) {
		perror(host);
		return -1;
	}
	err = lfs_file_open(&lfs, &file, name,
	    LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
	if (err != 0) {
		fclose(f);
		return err;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (lfs_file_write(&lfs, &file, buf, n) < 0) {
			lfs_file_close(&lfs, &file);
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	err = lfs_file_close(&lfs, &file);
	return err;
}

static int
op_get(const char *name, const char *host)
{
	FILE *f = fopen(host, "wb");
	lfs_file_t file;
	unsigned char buf[1024];
	lfs_ssize_t n;
	int err;

	if (f == NULL) {
		perror(host);
		return -1;
	}
	err = lfs_file_open(&lfs, &file, name, LFS_O_RDONLY);
	if (err != 0) {
		fclose(f);
		return err;
	}
	while ((n = lfs_file_read(&lfs, &file, buf,
	    sizeof(buf))) > 0)
		fwrite(buf, 1, (size_t)n, f);
	lfs_file_close(&lfs, &file);
	fclose(f);
	return n < 0 ? -1 : 0;
}

int
main(int argc, char **argv)
{
	long total_sectors;
	int err, rc = 1;

	if (argc < 3) {
		fprintf(stderr,
		    "usage: lfsimg <img> <ls|put|get|del> ...\n");
		return 2;
	}
	g_img = fopen(argv[1], "r+b");
	if (g_img == NULL) {
		perror(argv[1]);
		return 2;
	}
	if (fseek(g_img, 0, SEEK_END) != 0)
		return 2;
	total_sectors = ftell(g_img) / 512;
	if (total_sectors < (long)(REGION_SECTORS + TAIL_SECTORS + 64)) {
		fprintf(stderr, "lfsimg: image too small\n");
		return 2;
	}
	g_base = (total_sectors - TAIL_SECTORS - REGION_SECTORS) * 512L;

	memset(&cfg, 0, sizeof(cfg));
	cfg.read = bd_read;
	cfg.prog = bd_prog;
	cfg.erase = bd_erase;
	cfg.sync = bd_sync;
	cfg.read_size = 256;
	cfg.prog_size = 256;
	cfg.block_size = BLK_SIZE;
	cfg.block_count = BLK_COUNT;
	cfg.cache_size = 256;
	cfg.lookahead_size = 256;
	cfg.block_cycles = 500;
	cfg.read_buffer = read_buf;
	cfg.prog_buffer = prog_buf;
	cfg.lookahead_buffer = look_buf;

	err = mount_or_format();
	if (err != 0) {
		fprintf(stderr, "lfsimg: mount/format failed (%d)\n", err);
		return 1;
	}

	if (strcmp(argv[2], "ls") == 0 && argc == 3)
		rc = op_ls() == 0 ? 0 : 1;
	else if (strcmp(argv[2], "put") == 0 && argc == 5)
		rc = op_put(argv[3], argv[4]) == 0 ? 0 : 1;
	else if (strcmp(argv[2], "get") == 0 && argc == 5)
		rc = op_get(argv[3], argv[4]) == 0 ? 0 : 1;
	else if (strcmp(argv[2], "del") == 0 && argc == 4)
		rc = lfs_remove(&lfs, argv[3]) == 0 ? 0 : 1;
	else
		fprintf(stderr, "lfsimg: bad op/args\n");

	lfs_unmount(&lfs);
	fclose(g_img);
	return rc;
}
