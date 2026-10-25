
/*
 * ulib.c - Implementasi wrapper syscall QaonicOS (Fase B/C).
 *
 * ABI: nomor di r7, argumen di r0-r5, return di r0; svc #0.
 * "lr" masuk clobber karena svc menimpanya (pelajaran Clang P6).
 */
#include "ulib.h"
/* Full struct uipc_msg: the RPC wrappers dereference
 * hdr.reply / hdr.id (the other wrappers only pass
 * pointers, which is why the forward declaration in
 * ulib.h sufficed until IPC-4). */
#include "uipc.h"

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

/* Boot state (syscall 98, B2): sub 0 fills info[0..3] with
 * boot count, fail_count, safe_mode, last_reason. Same
 * r0/r1 convention as the shell's sh_bootstate wrapper;
 * promoted to ulib so uiapp screens (Monitor, UI-4) can
 * read it without duplicating raw asm. Named
 * sys_bootinfo (not sys_bootstate) because ufirst.c
 * already owns a static sys_bootstate(sub) with a
 * different signature. */
int
sys_bootinfo(unsigned *info)
{
	int ret;
	__asm__ volatile(
		"mov r0, #0\n\t"
		"mov r1, %1\n\t"
		"mov r7, #98\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (info)
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

/*
 * Wrapper display (App A1): SYS_DISPLAY_INFO=60, SYS_DISPLAY_FLUSH=61.
 */
int
sys_display_info(struct qaon_display *di, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r7, #60\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (di), "r" (len)
		: "r0", "r1", "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_display_flush(unsigned x, unsigned y, unsigned w, unsigned h,
		  const void *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = x;
	register unsigned _r1 __asm__("r1") = y;
	register unsigned _r2 __asm__("r2") = w;
	register unsigned _r3 __asm__("r3") = h;
	register unsigned _r4 __asm__("r4") = (unsigned)buf;
	register unsigned _r5 __asm__("r5") = len;
	int ret;
	__asm__ volatile(
		"mov r7, #61\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2),
		  "r" (_r3), "r" (_r4), "r" (_r5)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/*
 * Wrapper protokol token display (App A2): 62-68.
 */
static int
sys_display_simple(unsigned num, unsigned a0)
{
	register unsigned _r0 __asm__("r0") = a0;
	int ret;
	__asm__ volatile(
		"mov r7, %1\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (num), "r" (_r0)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_display_grant(void)
{
	return sys_display_simple(62u, 0u);
}

int
sys_display_acquire(void)
{
	return sys_display_simple(63u, 0u);
}

int
sys_display_release(void)
{
	return sys_display_simple(64u, 0u);
}

int
sys_display_get_event(void)
{
	return sys_display_simple(65u, 0u);
}

int
sys_display_status(void)
{
	return sys_display_simple(66u, 0u);
}

unsigned
sys_uptime(void)
{
	return (unsigned)sys_display_simple(67u, 0u);
}

int
sys_display_sleep(int req)
{
	return sys_display_simple(68u, (unsigned)req);
}

/*
 * Wrapper jam + UDP (App A4): 69-72.
 */
int
sys_time_set(unsigned unix_sec)
{
	return sys_display_simple(69u, unix_sec);
}

unsigned
sys_time_get(void)
{
	return (unsigned)sys_display_simple(70u, 0u);
}

int
sys_time_quality(void)
{
	return sys_display_simple(100u, 0u);
}

int
sys_time_sync(void)
{
	return sys_display_simple(101u, 1u);
}

int
sys_time_unsync(void)
{
	return sys_display_simple(101u, 0u);
}

int
sys_udp_send(unsigned dst_ip, unsigned dst_port,
	     const void *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = dst_ip;
	register unsigned _r1 __asm__("r1") = dst_port;
	register unsigned _r2 __asm__("r2") = (unsigned)buf;
	register unsigned _r3 __asm__("r3") = len;
	int ret;
	__asm__ volatile(
		"mov r7, #71\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2), "r" (_r3)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_udp_recv(void *buf, unsigned maxlen,
	     unsigned *src_ip, unsigned short *src_port)
{
	register unsigned _r0 __asm__("r0") = (unsigned)buf;
	register unsigned _r1 __asm__("r1") = maxlen;
	register unsigned _r2 __asm__("r2") = (unsigned)src_ip;
	register unsigned _r3 __asm__("r3") = (unsigned)src_port;
	int ret;
	__asm__ volatile(
		"mov r7, #72\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2), "r" (_r3)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* TCP client (Q2a): 73-77. Pola sama dengan wrapper syscall lain. */

int
sys_tcp_connect(unsigned dst_ip, unsigned dst_port)
{
	register unsigned _r0 __asm__("r0") = dst_ip;
	register unsigned _r1 __asm__("r1") = dst_port;
	int ret;
	__asm__ volatile(
		"mov r7, #73\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_tcp_status(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #74\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_tcp_send(const void *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = (unsigned)buf;
	register unsigned _r1 __asm__("r1") = len;
	int ret;
	__asm__ volatile(
		"mov r7, #75\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_tcp_recv(void *buf, unsigned maxlen)
{
	register unsigned _r0 __asm__("r0") = (unsigned)buf;
	register unsigned _r1 __asm__("r1") = maxlen;
	int ret;
	__asm__ volatile(
		"mov r7, #76\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_tcp_close(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #77\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_face_expr(unsigned expr, const char *text)
{
	register unsigned _r0 __asm__("r0") = expr;
	register unsigned _r1 __asm__("r1") = (unsigned)text;
	int ret;
	__asm__ volatile(
		"mov r7, #78\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_face_poll(unsigned *expr_out, char *text_out)
{
	register unsigned _r0 __asm__("r0") = 0xFFFFFFFFu;
	register unsigned _r1 __asm__("r1") = (unsigned)expr_out;
	register unsigned _r2 __asm__("r2") = (unsigned)text_out;
	int ret;
	__asm__ volatile(
		"mov r7, #78\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* Q9: spawn(path) -> 0 bila dimulai, -1 gagal.
 * Lalu loop sys_spawn_wait() sampai dapat exit code. */
int
sys_spawn(const char *path)
{
	register unsigned _r0 __asm__("r0") = (unsigned)path;
	int ret;
	__asm__ volatile(
		"mov r7, #79\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* Q9: -1 bila child masih jalan, else exit code. */
int
sys_spawn_wait(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #80\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* ===================== IPC (IPC-1, ulib/uipc.h) =====================
 * Raw stubs + friendly wrappers. Numbers and sub-ops are
 * defined in ulib/uipc.h; the numeric literals here mirror
 * them (uipc.h includes this header, not vice versa). */

/* SYS_SEND (10): (dest, buf, len, flags) -> 0 or UIPC_E*.
 * NOTE on the constraints: r0 is READ-WRITE -- it
 * carries the argument in and the kernel's return
 * value out (trap.c overwrites the frame's r0; r1-r3
 * are restored). A pure input constraint for r0 lets
 * the compiler reuse r0 across the call: the EPARK
 * loop in uipc_recv() then re-issues the receive
 * with port=-11 (the previous return value), which
 * the glue rightly answers EDEST. That latent stub
 * bug (invisible while IPC-2's build happened to
 * reload the argument) is what the IPC-4 server
 * probe caught; every IPC stub below models r0 as
 * read-write for the same reason. */
int
sys_ipc_send(unsigned dest, const void *buf, unsigned len,
	     unsigned flags)
{
	register unsigned _r0 __asm__("r0") = dest;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	register unsigned _r3 __asm__("r3") = flags;
	__asm__ volatile(
		"mov r7, #10\n\t"
		"svc #0"
		: "+r" (_r0)
		: "r" (_r1), "r" (_r2), "r" (_r3)
		: "r7", "lr", "memory", "cc");
	return (int)_r0;
}

/* SYS_RECV (11): (port, buf, max, timeout_ms) -> total
 * len or UIPC_E*. timeout 0 = poll; >0 = park up to
 * that many ms; -1 = park without a deadline. A woken
 * park returns UIPC_EPARK (-11) to this stub; the
 * uipc_recv() wrapper below loops on it. The numeric
 * literal mirrors uipc.h. */
int
sys_ipc_recv(unsigned port, void *buf, unsigned max,
	     int timeout_ms)
{
	register unsigned _r0 __asm__("r0") = port;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = max;
	register unsigned _r3 __asm__("r3") = (unsigned)timeout_ms;
	__asm__ volatile(
		"mov r7, #11\n\t"
		"svc #0"
		: "+r" (_r0)
		: "r" (_r1), "r" (_r2), "r" (_r3)
		: "r7", "lr", "memory", "cc");
	return (int)_r0;
}

/* SYS_RPC (12): (dest, buf, len, timeout_ms) -> reply
 * len or UIPC_E*. The request is in buf with hdr.reply
 * set to a reply port the caller holds; the reply is
 * written over the request. A woken park returns
 * UIPC_EPARK (-11) like SYS_RECV; the uipc_rpc()
 * wrapper below continues the wait with a plain
 * uipc_recv() on the reply port (never by re-issuing
 * SYS_RPC, which would duplicate the request). */
int
sys_ipc_rpc(unsigned dest, void *buf, unsigned len,
	    int timeout_ms)
{
	register unsigned _r0 __asm__("r0") = dest;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	register unsigned _r3 __asm__("r3") = (unsigned)timeout_ms;
	__asm__ volatile(
		"mov r7, #12\n\t"
		"svc #0"
		: "+r" (_r0)
		: "r" (_r1), "r" (_r2), "r" (_r3)
		: "r7", "lr", "memory", "cc");
	return (int)_r0;
}

/* SYS_RPC_USER (23): (name string, buf, len, timeout_ms)
 * -- registry lookup, then the SYS_RPC engine. Same
 * return contract as SYS_RPC. */
int
sys_ipc_rpc_user(const char *name, void *buf, unsigned len,
		 int timeout_ms)
{
	register unsigned _r0 __asm__("r0") = (unsigned)name;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	register unsigned _r3 __asm__("r3") = (unsigned)timeout_ms;
	__asm__ volatile(
		"mov r7, #23\n\t"
		"svc #0"
		: "+r" (_r0)
		: "r" (_r1), "r" (_r2), "r" (_r3)
		: "r7", "lr", "memory", "cc");
	return (int)_r0;
}

/* SYS_IPC_PORT (25): (sub, a1, a2) -> name/0 or UIPC_E* */
int
sys_ipc_port(unsigned sub, unsigned a1, unsigned a2)
{
	register unsigned _r0 __asm__("r0") = sub;
	register unsigned _r1 __asm__("r1") = a1;
	register unsigned _r2 __asm__("r2") = a2;
	__asm__ volatile(
		"mov r7, #25\n\t"
		"svc #0"
		: "+r" (_r0)
		: "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return (int)_r0;
}

/* Sub-ops: 0 ALLOC_RECV, 1 DESTROY, 2 SETQLIMIT,
 * 3 REGISTER, 4 LOOKUP, 5 REG_GEN (uipc.h). Subs >= 3
 * take a name string in a1. */

int
uipc_port_alloc(void)
{
	return sys_ipc_port(0u, 0u, 0u);
}

int
uipc_port_destroy(unsigned name)
{
	return sys_ipc_port(1u, name, 0u);
}

int
uipc_port_setqlimit(unsigned name, unsigned limit)
{
	return sys_ipc_port(2u, name, limit);
}

int
uipc_register(const char *name, unsigned port)
{
	return sys_ipc_port(3u, (unsigned)name, port);
}

int
uipc_lookup(const char *name)
{
	return sys_ipc_port(4u, (unsigned)name, 0u);
}

int
uipc_reg_gen(const char *name)
{
	return sys_ipc_port(5u, (unsigned)name, 0u);
}

int
uipc_send(unsigned dest, const struct uipc_msg *m, unsigned len)
{
	return sys_ipc_send(dest, m, len, 0u);
}

int
uipc_reply(unsigned dest_once, const struct uipc_msg *m,
	   unsigned len)
{
	return sys_ipc_send(dest_once, m, len, 1u); /* SEND_ONCE */
}

int
uipc_recv_poll(unsigned port, struct uipc_msg *m, unsigned max)
{
	return sys_ipc_recv(port, m, max, 0);
}

/* IPC-2: blocking receive. The kernel parks the caller
 * inside SYS_RECV when the queue is empty; on wake the
 * stub returns UIPC_EPARK (-11) and we simply call in
 * again -- the kernel kept the wait (with its original
 * deadline) and the re-entry collects the message or
 * the UIPC_ETIME verdict. */
int
uipc_recv(unsigned port, struct uipc_msg *m, unsigned max,
	  int timeout_ms)
{
	int rc;
	do {
		rc = sys_ipc_recv(port, m, max, timeout_ms);
	} while (rc == -11);	/* UIPC_EPARK */
	return rc;
}

/* IPC-4 RPC wrapper. Allocates the reply port, sets
 * hdr.reply, and destroys the reply port on every
 * return path. A parked wait returns UIPC_EPARK (-11)
 * from the raw stub; we continue it with a plain
 * uipc_recv() on the reply port (the kernel wait
 * record survives; re-issuing SYS_RPC would duplicate
 * the request). A delivered Mach send-once death
 * notice (id 71) is mapped to UIPC_EPEER. */
int
uipc_rpc(unsigned dest, struct uipc_msg *m, unsigned len,
	 int timeout_ms)
{
	int reply = uipc_port_alloc();
	int rc;
	if (reply < 0)
		return reply;
	m->reply = (unsigned)reply;
	rc = sys_ipc_rpc(dest, m, len, timeout_ms);
	if (rc == -11)	/* UIPC_EPARK */
		rc = uipc_recv((unsigned)reply, m, len, timeout_ms);
	if (rc > 0 && m->id == 71)	/* notify, not a reply */
		rc = -12;	/* UIPC_EPEER */
	uipc_port_destroy((unsigned)reply);
	return rc;
}

int
uipc_rpc_user(const char *name, struct uipc_msg *m,
	      unsigned len, int timeout_ms)
{
	int reply = uipc_port_alloc();
	int rc;
	if (reply < 0)
		return reply;
	m->reply = (unsigned)reply;
	rc = sys_ipc_rpc_user(name, m, len, timeout_ms);
	if (rc == -11)	/* UIPC_EPARK */
		rc = uipc_recv((unsigned)reply, m, len, timeout_ms);
	if (rc > 0 && m->id == 71)	/* notify, not a reply */
		rc = -12;	/* UIPC_EPEER */
	uipc_port_destroy((unsigned)reply);
	return rc;
}

/* Q9: jadi eksklusif pembaca console. */
int
sys_console_takeover(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #81\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* Dev/test: inject a button event (EV_*) into the kernel
 * event queue -- the same queue hardware GPIO buttons feed. */
int
sys_event_inject(int ev)
{
	register unsigned _r0 __asm__("r0") = (unsigned)ev;
	int ret;
	__asm__ volatile(
		"mov r7, #105\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* FND-1: software reset (PSCI SYSTEM_RESET). On success this
 * never returns; a return means the platform refused (-1). */
int
sys_reset(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #106\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* FND-1: power off (PSCI SYSTEM_OFF). Same contract as
 * sys_reset(): success never returns. */
int
sys_poweroff(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #107\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* D1: ACODEC init + register readback. */
int
sys_audio_init(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #82\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

unsigned
sys_audio_reg(unsigned off)
{
	register unsigned _r0 __asm__("r0") = off;
	unsigned ret;
	__asm__ volatile(
		"mov r7, #83\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* D2: I2S RX + capture. */
int
sys_audio_start(unsigned rate, unsigned bits, unsigned ch)
{
	register unsigned _r0 __asm__("r0") = rate;
	register unsigned _r1 __asm__("r1") = bits;
	register unsigned _r2 __asm__("r2") = ch;
	int ret;
	__asm__ volatile(
		"mov r7, #84\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_audio_stop(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #86\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* D3: mic gain + overrun counter. */
int
sys_audio_gain(unsigned level)
{
	register unsigned _r0 __asm__("r0") = level;
	int ret;
	__asm__ volatile(
		"mov r7, #87\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0)
		: "r7", "lr", "memory", "cc");
	return ret;
}

unsigned
sys_audio_overrun(void)
{
	unsigned ret;
	__asm__ volatile(
		"mov r7, #88\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

/* D2b: PL330 DMA. */
int
sys_pl330_init(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #89\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		:
		: "r7", "lr", "memory", "cc");
	return ret;
}

int
sys_pl330_rx(void *dst, unsigned nwords)
{
	register unsigned _r0 __asm__("r0") = (unsigned)dst;
	register unsigned _r1 __asm__("r1") = nwords;
	int ret;
	__asm__ volatile(
		"mov r7, #90\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (_r0), "r" (_r1)
		: "r7", "lr", "memory", "cc");
	return ret;
}

/*
 * sys_audio_read: blocking read with timeout (ms).
 * Accumulates until len bytes or timeout.
 * Returns bytes read, 0 on timeout (no data), -1 on error.
 */
int
sys_audio_read(void *buf, unsigned len, unsigned timeout_ms)
{
	unsigned t0 = sys_uptime();
	unsigned total = 0u;
	int n;

	if (len > 8192u)
		return -1;
	while (total < len) {
		register unsigned _r0 __asm__("r0") =
		    (unsigned)buf + total;
		register unsigned _r1 __asm__("r1") = len - total;
		__asm__ volatile(
			"mov r7, #85\n\t"
			"svc #0\n\t"
			"mov %0, r0"
			: "=r" (n)
			: "r" (_r0), "r" (_r1)
			: "r7", "lr", "memory", "cc");
		if (n < 0)
			return -1;
		total += (unsigned)n;
		if (total >= len)
			break;
		if (sys_uptime() - t0 >= timeout_ms)
			break;
		sys_yield();
	}
	return (int)total;
}
