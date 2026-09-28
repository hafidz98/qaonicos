
/*
 * ulib.c - Implementasi wrapper syscall QaonicOS (Fase B/C).
 *
 * ABI: nomor di r7, argumen di r0-r5, return di r0; svc #0.
 * "lr" masuk clobber karena svc menimpanya (pelajaran Clang P6).
 */
#include "ulib.h"

int
sys_write(int fd, const void *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #20\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (fd), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_yield(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #21\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

void
sys_exit(int code)
{
	__asm__ volatile(
		"mov r0, %0\n\t"
		"mov r7, #22\n\t"
		"svc #0"
		:
		: "r" (code)
		: "r0", "r7", "lr", "memory", "cc");
	for (;;) { }	/* tak tercapai */
}

void *
sys_sbrk(int incr)
{
	void *ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #24\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (incr)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_open(const char *path, unsigned flags)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #30\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path), "r" (flags)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_read(int fd, void *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #31\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (fd), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_close(int fd)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #32\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (fd)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_ls(char *buf, unsigned max)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #33\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (buf), "r" (max)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_delete(const char *path)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #34\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_stat(struct qaon_stat *st)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #57\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (st), "r" (sizeof(struct qaon_stat))
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_tlist(struct qaon_tentry *e, unsigned max)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #58\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (e), "r" (max)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_read_console(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #59\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

unsigned
ustrlen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

int
puts(const char *s)
{
	return sys_write(1, s, ustrlen(s));
}

/*
 * Lapisan kompatibel API lama (Fase 10).
 */
int
u_write(unsigned fd, const char *buf, unsigned len)
{
	return sys_write((int)fd, buf, len);
}

int
u_open(const char *path, unsigned flags)
{
	return sys_open(path, flags);
}

int
u_read(unsigned fd, char *buf, unsigned len)
{
	return sys_read((int)fd, buf, len);
}

int
u_close(unsigned fd)
{
	return sys_close((int)fd);
}

int
u_ls(char *buf, unsigned max)
{
	return sys_ls(buf, max);
}

void
u_yield(void)
{
	sys_yield();
}

void
u_exit(void)
{
	sys_exit(0);
}

void
u_put(const char *s)
{
	sys_write(1, s, ustrlen(s));
}

unsigned
u_strlen(const char *s)
{
	return ustrlen(s);
}

int
u_mcmp(const char *a, const char *b, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 1;
	}
	return 0;
}

int
u_stat(struct qaon_stat *s)
{
	return sys_stat(s);
}

int
u_tlist(struct qaon_tentry *e, unsigned max)
{
	return sys_tlist(e, max);
}

int
u_console_getc(void)
{
	return sys_read_console();
}

int
u_wait_file(const char *path)
{
	unsigned i;
	int fd;

	for (i = 0; i < ULIB_POLL_MAX; i++) {
		if ((i & 255u) == 0)
			u_yield();
		fd = u_open(path, O_RDONLY);
		if (fd >= 0) {
			u_close((unsigned)fd);
			return 1;
		}
	}
	return 0;
}

int
u_touch(const char *path)
{
	int fd = u_open(path, O_CREAT | O_RDWR);
	if (fd < 0)
		return 0;
	u_close((unsigned)fd);
	return 1;
}

/*
 * Wrapper GPIO (Fase D): SYS_GPIO_SET=40, SYS_GPIO_GET=41.
 */
int
sys_gpio_set(unsigned pin, unsigned val)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #40\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (pin), "r" (val)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_gpio_get(unsigned pin)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #41\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (pin)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

int
u_gpio_set(unsigned pin, unsigned val)
{
	return sys_gpio_set(pin, val);
}

int
u_gpio_get(unsigned pin)
{
	return sys_gpio_get(pin);
}

/*
 * Wrapper SD (Fase D): SYS_SD_READ=50, SYS_SD_WRITE=51.
 */
int
sys_sd_read(unsigned sector, void *buf)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #50\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (sector), "r" (buf)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_sd_write(unsigned sector, const void *buf)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #51\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (sector), "r" (buf)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
u_sd_read(unsigned sector, void *buf)
{
	return sys_sd_read(sector, buf);
}

int
u_sd_write(unsigned sector, const void *buf)
{
	return sys_sd_write(sector, buf);
}

/*
 * Wrapper FAT32 (Fase D): SYS_MKDIR=52, FAT_WRITE=53, FAT_READ=54,
 * FAT_DELETE=55, READDIR=56.
 */
int
sys_mkdir(const char *path)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #52\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_fat_write(const char *path, const void *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #53\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_fat_read(const char *path, void *buf, unsigned max)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #54\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path), "r" (buf), "r" (max)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_fat_delete(const char *path)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r7, #55\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path)
		: "r0", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_readdir(const char *path, char *buf, unsigned max)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #56\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (path), "r" (buf), "r" (max)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

int
u_mkdir(const char *path)
{
	return sys_mkdir(path);
}

int
u_fat_write(const char *path, const void *buf, unsigned len)
{
	return sys_fat_write(path, buf, len);
}

int
u_fat_read(const char *path, void *buf, unsigned max)
{
	return sys_fat_read(path, buf, max);
}

int
u_fat_delete(const char *path)
{
	return sys_fat_delete(path);
}

int
u_readdir(const char *path, char *buf, unsigned max)
{
	return sys_readdir(path, buf, max);
}
