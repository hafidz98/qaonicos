/*
 * mach3/kernel/arm/user.c -- M4 item 4: user mode + syscall interface.
 *
 * user_selftest(): called from startrtclock() (after task_selftest).
 *   Creates a user task (task_create, private pmap via task->map->pmap),
 *   maps a code page + stack page with user permissions, switches TTBR0
 *   to the user L1, and enters USR mode via user_enter_test().
 *
 *   Phase A (clean): embedded program does SYS_WRITE then SYS_EXIT(0).
 *   Phase B (fault): embedded program does SYS_WRITE then touches 0x0;
 *     the data-abort handler kills just the user context (redirect to
 *     user_exit_trampoline) instead of panicking the kernel.
 *
 * Syscall ABI (ARM EABI-like): number in r7, args in r0-r2, return in
 * r0; svc #0.
 */
#include <mach/machine/vm_types.h>
#include <mach/kern_return.h>
#include <mach/vm_prot.h>
#include <machine/trap_frame.h>
#include <machine/pmap.h>
#include <machine/machspl.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include "ramfs.h"

extern void	panic(const char *, ...);
extern int	printf(const char *, ...);
extern void	uart_putc(char c);

/* userasm.s */
extern unsigned int	user_enter_test(unsigned int user_sp,
					  unsigned int user_pc);
extern void		user_exit_trampoline(void);

/* uprog.s: embedded position-independent user programs */
extern unsigned char	_uprog_start[], _uprog_end[];
extern unsigned char	_uprog_fault[];

/* Embedded user programs (Fase B/C, generated at build). */
extern unsigned char	init_img[];
extern unsigned int	init_img_len;
extern unsigned char	ucat_img[];
extern unsigned int	ucat_img_len;
extern unsigned char	uls_img[];
extern unsigned int	uls_img_len;
extern unsigned char	uecho_img[];
extern unsigned int	uecho_img_len;
extern unsigned char	umon_img[];
extern unsigned int	umon_img_len;
extern unsigned char	ugpio_img[];
extern unsigned int	ugpio_img_len;
extern unsigned char	usd_img[];
extern unsigned int	usd_img_len;
extern unsigned char	ufs_img[];
extern unsigned int	ufs_img_len;
extern unsigned char	face_img[];	/* App A1/A2: Qabot server */
extern unsigned int	face_img_len;
extern unsigned char	uiapp_img[];	/* App A2: menu/settings/monitor */
extern unsigned int	uiapp_img_len;
extern unsigned char	ntp_img[];	/* App A4: sinkron jam via NTP */
extern unsigned int	ntp_img_len;

/* trap.c (Fase B) */
extern unsigned int	arm_timer_ticks(void);

/* netstack.c (App A4: UDP untuk DNS/NTP) */
extern int	netstack_udp_send(unsigned int dst, unsigned short dport,
				      const unsigned char *data, unsigned dlen);
extern int	netstack_udp_recv(unsigned char *buf, unsigned maxlen,
				      unsigned int *src_ip,
				      unsigned short *src_port);

/* clock.c (Fase B) */
extern void	arm_timer_enable(void);

/* uart.c: non-blocking console read (Fase C, SYS_READ_CONSOLE). */
extern int	cnmaygetc(void);

/* blk.c: kapasitas storage real (Fase C, SYS_STAT). */
extern unsigned int	blk_total_sectors(void);

/* MI vm_page.c: jumlah halaman bebas saat ini (Fase C, SYS_STAT). */
extern int	vm_page_free_count;

/* arm_init.c: total RAM (Fase C, SYS_STAT). */
extern vm_offset_t	mem_size;

/* gpio.c (Fase D, SYS_GPIO_SET/GET). Bank di-fix 0 (seperti kernel lama). */
extern int	gpio_set(unsigned int bank, unsigned int pin,
			 unsigned int val);
extern int	gpio_get(unsigned int bank, unsigned int pin);

/* blk.c (Fase D, SYS_SD_READ/WRITE). */
extern int	sd_present(void);
extern int	sd_read(unsigned int sector, unsigned char *data);
extern int	sd_write(unsigned int sector, const unsigned char *data);

/* fat32.c (Fase D, SYS_MKDIR/FAT_WRITE/FAT_READ/FAT_DELETE/READDIR). */
extern int	fat32_mounted(void);
extern int	fat32_mount(void);
extern int	fat32_mkdir(const char *path);
extern int	fat32_write_file(const char *path, const unsigned char *data,
				 unsigned int len);
extern int	fat32_read_file(const char *path, unsigned char *dst,
				unsigned int max);
extern int	fat32_delete(const char *path);
extern int	fat32_listdir(const char *path, char *dst, unsigned int max);

/* gpu.c (App A1, SYS_DISPLAY_INFO/FLUSH). */
extern int	gpu_available_p(void);
extern int	gpu_flush_strip(unsigned int x, unsigned int y,
				unsigned int w, unsigned int h,
				const unsigned short *rgb565);

/* Info display untuk SYS_DISPLAY_INFO (12 byte). */
struct qaon_display {
	unsigned int	width;
	unsigned int	height;
	unsigned int	format;	/* 0 = input RGB565 (strip parsial) */
};
#define	QAON_DISP_W	240u
#define	QAON_DISP_H	240u

/* pmap.c */
extern void	arm_pmap_activate_user(pmap_t pmap);
extern void	arm_pmap_activate_kernel(void);

#define	USER_CODE_VA	0x100000u
#define	USER_STACK_VA	0x101000u
#define	USER_PGBYTES	4096u

/* Fase B init task layout (separate user task from the selftest above). */
#define	INIT_CODE_VA	0x100000u	/* init image (npages, R/W) */
#define	INIT_STACK_VA	0x110000u	/* 1 page; SP starts at 0x111000 */
#define	INIT_HEAP_VA	0x120000u	/* sbrk region */
#define	INIT_HEAP_PAGES	16u		/* 64 KB pre-allocated heap */
#define	USER_VA_BASE	0x100000u	/* lowest valid user VA */
#define	USER_VA_TOP	0x130000u	/* highest valid user VA */

/*
 * Syscalls (ABI Fase B/C — kontinu dengan kernel lama;
 * lihat docs/SYSCALL-ABI.md).
 */
#define	SYS_WRITE	20u	/* write(fd, buf, len) -> len or -1 */
#define	SYS_YIELD	21u	/* yield remainder of slice -> 0 */
#define	SYS_EXIT	22u	/* exit(code) -> never returns */
#define	SYS_SBRK	24u	/* sbrk(incr) -> old brk or -1 */
#define	SYS_OPEN	30u	/* open(path, flags) -> fd or -1 */
#define	SYS_READ	31u	/* read(fd, buf, len) -> bytes or -1 */
#define	SYS_CLOSE	32u	/* close(fd) -> 0 or -1 */
#define	SYS_LS		33u	/* ls(buf, max) -> file count or -1 */
#define	SYS_DELETE	34u	/* delete(path) -> 0 or -1 */
#define	SYS_STAT	57u	/* stat(buf, len) -> 0 or -1 */
#define	SYS_TLIST	58u	/* tlist(buf, max) -> entries or -1 */
#define	SYS_READ_CONSOLE 59u	/* read_console() -> byte or -1 */
#define	SYS_GPIO_SET	40u	/* gpio_set(pin, val) -> 0 or -1 (Fase D) */
#define	SYS_GPIO_GET	41u	/* gpio_get(pin) -> 0/1 or -1 (Fase D) */
#define	SYS_SD_READ	50u	/* sd_read(sector, buf512) -> 0 or -1 (Fase D) */
#define	SYS_SD_WRITE	51u	/* sd_write(sector, buf512) -> 0 or -1 (Fase D) */
#define	SYS_MKDIR	52u	/* mkdir(path) -> 0 or -1 (Fase D) */
#define	SYS_FAT_WRITE	53u	/* fat_write(path, buf, len) -> bytes/-1 (Fase D) */
#define	SYS_FAT_READ	54u	/* fat_read(path, buf, max) -> bytes/-1 (Fase D) */
#define	SYS_FAT_DELETE	55u	/* fat_delete(path) -> 0 or -1 (Fase D) */
#define	SYS_READDIR	56u	/* readdir(path, buf, max) -> count/-1 (Fase D) */
#define	SYS_DISPLAY_INFO 60u	/* display_info(buf, len) -> 0/-1 (App A1) */
#define	SYS_DISPLAY_FLUSH 61u	/* display_flush(x,y,w,h,buf,len) -> 0/-1 */
/* App A2: protokol token display + input (arbiter di kernel). */
#define	SYS_DISPLAY_GRANT 62u	/* face->kernel: beri token ke uiapp -> 0/-1 */
#define	SYS_DISPLAY_ACQUIRE 63u	/* -> 1 bila pemegang token, else 0 */
#define	SYS_DISPLAY_RELEASE 64u	/* pemegang kembalikan token -> 0/-1 */
#define	SYS_DISPLAY_GET_EVENT 65u	/* -> EV_* atau -1 (kosong/bukan pemegang) */
#define	SYS_DISPLAY_STATUS 66u	/* -> id pemegang token (0=face,1=uiapp) */
#define	SYS_UPTIME	67u	/* -> milidetik sejak boot */
#define	SYS_DISPLAY_SLEEP 68u	/* r0=1: uiapp minta sleep; r0=0: face ambil+clear */
#define	SYS_TIME_SET	69u	/* App A4: time_set(unix_sec) -> 0 */
#define	SYS_TIME_GET	70u	/* App A4: -> detik Unix / 0 (belum di-set) */
#define	SYS_UDP_SEND	71u	/* App A4: udp_send(ip,port,buf,len) -> 0/-1 */
#define	SYS_UDP_RECV	72u	/* App A4: udp_recv(buf,max,&ip,&port) -> n/0/-1 */

/* Kode event input (App A2; disalin ke user/ulib/ulib.h). */
#define	EV_UP		0
#define	EV_DOWN		1
#define	EV_LEFT		2
#define	EV_RIGHT	3
#define	EV_OK		4
#define	EV_BACK		5
#define	EV_MENU		6
#define	EV_TICK		7

/*
 * struct qaon_stat (Fase C): layout DISALIN MANUAL ke user/ulib/ulib.h.
 * Field yang belum tersedia di port Mach 3 ini diisi sentinel
 * QAON_UNKNOWN (0xFFFFFFFF) — "tandai yang belum ada".
 */
#define	QAON_UNKNOWN	0xFFFFFFFFu

struct qaon_stat {
	unsigned int	uptime_ms;	/* timer_ticks * 10 (100 Hz) */
	unsigned int	cpu_pct;	/* QAON_UNKNOWN: belum ada idle accounting */
	unsigned int	mem_used_kb;	/* (total_pages - vm_page_free_count) * 4 */
	unsigned int	mem_total_kb;	/* mem_size / 1024 */
	unsigned int	blk_total_sec;	/* blk_total_sectors() (real) */
	unsigned int	blk_used_sec;	/* QAON_UNKNOWN: belum ada FS di disk */
	unsigned int	net_rx_kb;	/* 0: belum ada driver net */
	unsigned int	net_tx_kb;	/* 0: belum ada driver net */
	unsigned int	nthreads;	/* jumlah user task yang diluncurkan */
};

struct qaon_tentry {
	unsigned int	id;
	unsigned int	state;	/* 0=RUNNABLE, 2=EXITED (ala kernel lama) */
	unsigned int	user;	/* 1 = user task */
};

/* user_exit_trampoline codes */
#define	USER_EXIT_DABT	0xDAB7u	/* data abort in user mode */
#define	USER_EXIT_PABT	0x9AB7u	/* prefetch abort in user mode */
#define	USER_EXIT_UNDEF	0x5EEDu	/* undefined insn in user mode */

/* --- cache maintenance (Cortex-A7, 64B lines; same as blk.c) --- */
#define	CACHE_LINE	64u

static void
dcache_clean_range(unsigned int va, unsigned int len)
{
	unsigned int a, end;

	end = (va + len + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
	for (a = va & ~(CACHE_LINE - 1u); a < end; a += CACHE_LINE)
		__asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r" (a));
	__asm__ volatile ("dsb ish" ::: "memory");
}

static void
icache_invalidate_all(void)
{
	__asm__ volatile ("mcr p15, 0, %0, c7, c5, 0" :: "r" (0));
	__asm__ volatile ("dsb ish; isb" ::: "memory");
}

/*
 * Fase B/C user task bookkeeping.  One struct per launched program
 * (Fase C: init, ucat, uls, uecho, umon — sequential).
 */
struct user_task {
	task_t		task;
	vm_offset_t	heap_base;	/* VA: start of sbrk region */
	vm_offset_t	brk;		/* current break */
	vm_offset_t	heap_end;	/* VA: hard limit */
};

/* Task user yang sedang berjalan (di-set sebelum user_enter_test).
 * user_syscall memakai ini untuk sbrk + file syscalls. */
static task_t	cur_utask = TASK_NULL;
static struct user_task *cur_udesc = 0;

/* Helper asm (userasm.s): baca/tulis banked sp/lr USR dari SVC. */
extern void	ctx_save_usr(unsigned int *sp_out, unsigned int *lr_out);
extern void	ctx_restore_usr(unsigned int sp, unsigned int lr);

/* Identitas program pemanggil (App A2): -1=oneshot, 0=face, 1=uiapp. */
static int	cur_progid = -1;

/*
 * Scheduler kooperatif App A2: daemon user (face, uiapp) jalan
 * bergantian di atas boot thread.  Setiap SYS_YIELD menyimpan konteks
 * user (register + banked sp/lr + pc/spsr) dan memuat milik daemon
 * berikutnya; pmap + bookkeeping ikut diganti.  Net dipump tiap yield.
 */
struct user_ctx {
	unsigned int	r[13];	/* r0-r12 */
	unsigned int	sp_usr;
	unsigned int	lr_usr;
	unsigned int	pc;	/* frame->lr */
	unsigned int	spsr;
};

#define	NDAEMON		3u	/* face, uiapp, ntp (App A4) */
#define	PROG_FACE	0
#define	PROG_UIAPP	1
#define	PROG_NTP	2	/* App A4: sinkron jam (tak pegang display) */

struct daemon {
	const char	*name;
	task_t		task;
	pmap_t		pmap;
	struct user_task *udesc;
	struct user_ctx	ctx;
	int		state;	/* 0=mati, 1=runnable */
	int		progid;
	unsigned	uprog_idx;	/* indeks di uprog_names/states */
};

static struct daemon	daemons[NDAEMON];
static unsigned		ndaemon_reg = 0;
static int		sched_cur = -1;
static int		sched_active = 0;

/* Arbiter display (App A2): token dipegang face (0) atau uiapp (1). */
static int		dpy_holder = PROG_FACE;
static int		dpy_sleep_req = 0;

/* Jam dinding (App A4): di-set program ntp via SYS_TIME_SET (detik
 * Unix UTC).  SYS_TIME_GET = set + (ticks_berlalu / 100). */
static unsigned		time_unix_set;
static unsigned		time_tick_set;
static int		time_valid = 0;

/* Antrean event input untuk pemegang token. */
#define	EVQ_LEN		16u
static int		evq[EVQ_LEN];
static unsigned		evq_head = 0, evq_tail = 0;

static void
evq_clear(void)
{
	evq_head = evq_tail = 0;
}

static void
evq_push(int ev)
{
	unsigned nxt = (evq_tail + 1u) % EVQ_LEN;
	if (nxt == evq_head)
		return;	/* penuh: buang yang terbaru */
	evq[evq_tail] = ev;
	evq_tail = nxt;
}

static int
evq_pop(void)
{
	int ev;
	if (evq_head == evq_tail)
		return -1;
	ev = evq[evq_head];
	evq_head = (evq_head + 1u) % EVQ_LEN;
	return ev;
}

/*
 * Penerjemah console -> EV_*: keyboard QEMU (WASD + Enter + Esc + M),
 * plus escape sequence panah (ESC [ A/B/C/D).  Non-blocking; -2 =
 * abaikan, -3 = butuh byte lanjutan (tak dipakai di sini).
 */
static int	con_pushback[4];
static int	con_npush = 0;

static int
con_getc(void)
{
	if (con_npush > 0)
		return con_pushback[--con_npush];
	return cnmaygetc();
}

static void
con_ungetc(int b)
{
	if (con_npush < 4 && b >= 0)
		con_pushback[con_npush++] = b;
}

static int
translate_key(int b)
{
	int b1, b2, ev;
	switch (b) {
	case 'w': case 'W': return EV_UP;
	case 's': case 'S': return EV_DOWN;
	case 'a': case 'A': return EV_LEFT;
	case 'd': case 'D': return EV_RIGHT;
	case '\r': case '\n': return EV_OK;
	case 'm': case 'M': return EV_MENU;
	case 0x1b:
		b1 = con_getc();
		if (b1 == '[') {
			b2 = con_getc();
			ev = -2;
			if (b2 == 'A') ev = EV_UP;
			else if (b2 == 'B') ev = EV_DOWN;
			else if (b2 == 'C') ev = EV_RIGHT;
			else if (b2 == 'D') ev = EV_LEFT;
			if (ev != -2)
				return ev;
			con_ungetc(b2);
			con_ungetc(b1);
		} else
			con_ungetc(b1);
		return EV_BACK;
	default:
		return -2;
	}
}

/* Daftar program user untuk SYS_TLIST (Fase C). App A1: 8 -> 16. */
#define	UPROG_MAX	16u
static const char	*uprog_names[UPROG_MAX];
static unsigned		uprog_states[UPROG_MAX];	/* 0=RUNNABLE, 2=EXITED */
static unsigned		nuprog = 0;

/*
 * Validasi range buffer user [va, va+len).  1 = valid.
 */
static int
user_range_ok(unsigned int va, unsigned int len)
{
	if (va < USER_VA_BASE)
		return 0;
	if (len > USER_VA_TOP - USER_VA_BASE)
		return 0;
	if (va + len > USER_VA_TOP)
		return 0;
	if (va + len < va)	/* wraparound */
		return 0;
	return 1;
}

/*
 * Salin path NUL-terminated dari user ke buffer kernel.
 * 0 = ok, -1 = path tak valid / tak muat.
 */
static int
copy_path_user(unsigned int va, char *kpath, unsigned max)
{
	unsigned i;

	if (!user_range_ok(va, max))
		return -1;
	for (i = 0; i < max - 1u; i++) {
		char c = ((const char *)va)[i];
		kpath[i] = c;
		if (c == 0)
			return 0;
	}
	kpath[max - 1u] = 0;
	return -1;	/* tidak NUL-terminated dalam batas */
}

/*
 * user_syscall: dispatch a syscall from user mode.
 * Called from trap.c's TRAP_SVC path with the trap frame.  The return
 * value is stored to frame->r[0] by the caller.  SYS_EXIT (and only it)
 * redirects the frame to user_exit_trampoline instead of returning to
 * user mode.
 *
 * ABI: number in r7, args in r0-r2, return in r0; svc #0.
 */
/* Scheduler + arbiter App A2 (definisi setelah user_syscall). */
static void	sched_yield_switch(struct arm_trap_frame *frame);
static void	sched_exit_switch(struct arm_trap_frame *frame, unsigned int code);
extern void	net_pump(void);

unsigned int
user_syscall(struct arm_trap_frame *frame)
{
	unsigned int num = frame->r[7];
	unsigned int a0 = frame->r[0];
	unsigned int a1 = frame->r[1];
	unsigned int a2 = frame->r[2];
	unsigned int a3 = frame->r[3];
	unsigned int a4 = frame->r[4];
	unsigned int a5 = frame->r[5];

	switch (num) {
	case SYS_WRITE: {
		const unsigned char *p;
		unsigned int i;

		/* Fase B validation: buffer must lie in the user VA
		 * window.  (Per-page table walk = future work; Fase B
		 * trusts the range.  All Fase B user mappings live in
		 * [USER_VA_BASE, USER_VA_TOP).) */
		if (!user_range_ok(a1, a2))
			return (unsigned int)-1;
		if (a0 == 1u || a0 == 2u) {
			p = (const unsigned char *)a1;
			for (i = 0; i < a2; i++)
				uart_putc((char)p[i]);
			return a2;
		}
		if (a0 >= 3u && cur_utask != TASK_NULL)
			return (unsigned int)ramfs_write(cur_utask, a0,
							 (const unsigned char *)a1,
							 a2);
		return (unsigned int)-1;
	}
	case SYS_YIELD:
		/* App A2: bila scheduler daemon aktif, yield = pindah ke
		 * daemon runnable berikutnya (round-robin) + pump net.
		 * Konteks pemanggil disimpan; saat kembali nanti r0=0.
		 * Fase one-shot (belum aktif): nop. */
		if (sched_active)
			sched_yield_switch(frame);
		return 0;
	case SYS_SBRK: {
		/* Heap pages are pre-allocated at task creation (no
		 * allocator calls in trap context); sbrk just bumps the
		 * break.  Returns old brk, or -1 on failure. */
		vm_offset_t new_brk;

		if (cur_udesc == 0)
			return (unsigned int)-1;
		if (a0 == 0)
			return cur_udesc->brk;
		if (a0 > USER_VA_TOP - cur_udesc->brk)
			return (unsigned int)-1;
		new_brk = cur_udesc->brk + a0;
		if (new_brk > cur_udesc->heap_end)
			return (unsigned int)-1;
		a0 = cur_udesc->brk;	/* old brk = return value */
		cur_udesc->brk = new_brk;
		return a0;
	}
	case SYS_OPEN: {
		char kpath[RAMFS_PATH_MAX];

		if (cur_utask == TASK_NULL)
			return (unsigned int)-1;
		if (copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)ramfs_open(cur_utask, kpath, a1);
	}
	case SYS_READ: {
		/* fd 0 (stdin): tidak ada input di bring-up -> 0 (EOF).
		 * fd 1/2: tak valid untuk read.  fd >= 3: ramfs. */
		if (a0 == 0u)
			return 0;
		if (a0 == 1u || a0 == 2u)
			return (unsigned int)-1;
		if (!user_range_ok(a1, a2))
			return (unsigned int)-1;
		if (cur_utask == TASK_NULL)
			return (unsigned int)-1;
		return (unsigned int)ramfs_read(cur_utask, a0,
						(unsigned char *)a1, a2);
	}
	case SYS_CLOSE:
		if (cur_utask == TASK_NULL)
			return (unsigned int)-1;
		return (unsigned int)ramfs_close(cur_utask, a0);
	case SYS_LS:
		if (!user_range_ok(a0, a1))
			return (unsigned int)-1;
		return (unsigned int)ramfs_list((char *)a0, a1);
	case SYS_DELETE: {
		char kpath[RAMFS_PATH_MAX];

		if (copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)ramfs_delete(kpath);
	}
	case SYS_STAT: {
		struct qaon_stat *s;
		unsigned int total_pages;

		if (a1 < sizeof(struct qaon_stat))
			return (unsigned int)-1;
		if (!user_range_ok(a0, sizeof(struct qaon_stat)))
			return (unsigned int)-1;
		s = (struct qaon_stat *)a0;
		s->uptime_ms = arm_timer_ticks() * 10u;
		s->cpu_pct = QAON_UNKNOWN;	/* belum ada idle accounting */
		total_pages = (unsigned int)(mem_size / 4096u);
		s->mem_total_kb = (unsigned int)(mem_size / 1024u);
		s->mem_used_kb = (total_pages - (unsigned int)vm_page_free_count)
				 * 4u;
		s->blk_total_sec = blk_total_sectors();
		s->blk_used_sec = QAON_UNKNOWN;	/* belum ada FS di disk */
		s->net_rx_kb = 0;	/* belum ada driver net (Fase D) */
		s->net_tx_kb = 0;
		s->nthreads = nuprog;
		return 0;
	}
	case SYS_TLIST: {
		struct qaon_tentry *e;
		unsigned i, n;

		if (a1 == 0u || a1 > 64u)
			return (unsigned int)-1;
		if (!user_range_ok(a0,
				   a1 * (unsigned)sizeof(struct qaon_tentry)))
			return (unsigned int)-1;
		e = (struct qaon_tentry *)a0;
		n = (nuprog < a1) ? nuprog : a1;
		for (i = 0; i < n; i++) {
			e[i].id = i;
			e[i].state = uprog_states[i];
			e[i].user = 1u;
		}
		return n;
	}
	case SYS_READ_CONSOLE:
		return (unsigned int)cnmaygetc();
	case SYS_GPIO_SET:
		/* Bank di-fix 0 (seperti kernel lama Fase 14). */
		return (unsigned int)gpio_set(0u, a0, a1);
	case SYS_GPIO_GET:
		return (unsigned int)gpio_get(0u, a0);
	case SYS_SD_READ:
		if (!sd_present() || !user_range_ok(a1, 512u))
			return (unsigned int)-1;
		return (unsigned int)sd_read(a0, (unsigned char *)a1);
	case SYS_SD_WRITE:
		if (!sd_present() || !user_range_ok(a1, 512u))
			return (unsigned int)-1;
		return (unsigned int)sd_write(a0, (unsigned char *)a1);
	case SYS_MKDIR: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)fat32_mkdir(kpath);
	}
	case SYS_FAT_WRITE: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)fat32_write_file(kpath,
						      (unsigned char *)a1, a2);
	}
	case SYS_FAT_READ: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)fat32_read_file(kpath,
						     (unsigned char *)a1, a2);
	}
	case SYS_FAT_DELETE: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)fat32_delete(kpath);
	}
	case SYS_READDIR: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)fat32_listdir(kpath, (char *)a1, a2);
	}
	case SYS_DISPLAY_INFO: {
		struct qaon_display *d;
		if (!gpu_available_p() || !user_range_ok(a0, a1) ||
		    a1 < sizeof(struct qaon_display))
			return (unsigned int)-1;
		d = (struct qaon_display *)a0;
		d->width = QAON_DISP_W;
		d->height = QAON_DISP_H;
		d->format = 0u;
		return 0u;
	}
	case SYS_DISPLAY_FLUSH: {
		/* a0=x a1=y a2=w a3=h a4=buf(RGB565) a5=len */
		/* App A2: hanya pemegang token display boleh flush. */
		if (cur_progid != dpy_holder)
			return (unsigned int)-1;
		if (!gpu_available_p() ||
		    a2 == 0u || a3 == 0u ||
		    a0 + a2 > QAON_DISP_W || a1 + a3 > QAON_DISP_H ||
		    a5 < a2 * a3 * 2u ||
		    !user_range_ok(a4, a5))
			return (unsigned int)-1;
		return (unsigned int)gpu_flush_strip(a0, a1, a2, a3,
						    (const unsigned short *)a4);
	}
	case SYS_EXIT:
		if (sched_active) {
			sched_exit_switch(frame, a0);
			return 0;
		}
		/* Leave user mode for good: resume at the trampoline
		 * in SVC; r0 carries the exit code. */
		frame->r[0] = a0;
		frame->lr = (unsigned int)user_exit_trampoline;
		frame->spsr = ARM_MODE_SVC;
		return a0;
	case SYS_DISPLAY_GRANT: {
		/* Hanya face boleh memberi token ke uiapp. */
		if (cur_progid != PROG_FACE)
			return (unsigned int)-1;
		dpy_holder = PROG_UIAPP;
		evq_clear();
		printf("[dpy] GRANT -> uiapp\n");
		return 0;
	}
	case SYS_DISPLAY_ACQUIRE:
		return (dpy_holder == cur_progid) ? 1u : 0u;
	case SYS_DISPLAY_RELEASE: {
		if (cur_progid != dpy_holder)
			return (unsigned int)-1;
		dpy_holder = PROG_FACE;
		/* JANGAN clear dpy_sleep_req di sini: alur Sleep Now =
		 * sleep(1) DULU baru release; flag harus sampai ke face. */
		evq_clear();
		printf("[dpy] RELEASE -> face\n");
		return 0;
	}
	case SYS_DISPLAY_GET_EVENT: {
		int b, ev;
		/* Hanya pemegang token yang dapat antrean event. */
		if (cur_progid != dpy_holder)
			return (unsigned int)-1;
		/* Kuras console -> terjemahkan -> antrekan. */
		for (;;) {
			b = con_getc();
			if (b < 0)
				break;
			ev = translate_key(b);
			if (ev >= 0)
				evq_push(ev);
		}
		return (unsigned int)evq_pop();
	}
	case SYS_DISPLAY_STATUS:
		return (unsigned int)dpy_holder;
	case SYS_UPTIME:
		return arm_timer_ticks() * 10u;
	case SYS_DISPLAY_SLEEP: {
		int ev;
		/* r0=1: pemegang token minta sleep; r0=0: face ambil+clear. */
		if (a0 == 0) {
			if (cur_progid != PROG_FACE)
				return (unsigned int)-1;
			ev = dpy_sleep_req;
			dpy_sleep_req = 0;
			return (unsigned int)ev;
		} else {
			if (cur_progid != dpy_holder)
				return (unsigned int)-1;
			dpy_sleep_req = 1;
			return 0;
		}
	}
	case SYS_TIME_SET:
		/* a0 = detik Unix (UTC). */
		time_unix_set = a0;
		time_tick_set = arm_timer_ticks();
		time_valid = 1;
		return 0;
	case SYS_TIME_GET: {
		unsigned dt;
		if (!time_valid)
			return 0;
		dt = arm_timer_ticks() - time_tick_set;
		return time_unix_set + dt / 100u;
	}
	case SYS_UDP_SEND: {
		/* a0=ip dst, a1=port dst, a2=buf, a3=len -> 0/-1 */
		int r;
		if (a3 > 512u || !user_range_ok(a2, a3))
			return (unsigned int)-1;
		r = netstack_udp_send(a0, (unsigned short)a1,
				      (const unsigned char *)a2, a3);
		return (unsigned int)(r < 0 ? -1 : 0);
	}
	case SYS_UDP_RECV: {
		/* a0=buf, a1=maxlen, a2=&src_ip, a3=&src_port */
		int n;
		unsigned int sip;
		unsigned short sport;
		if (a1 == 0u || a1 > 512u || !user_range_ok(a0, a1) ||
		    !user_range_ok(a2, sizeof(unsigned int)) ||
		    !user_range_ok(a3, sizeof(unsigned short)))
			return (unsigned int)-1;
		n = netstack_udp_recv((unsigned char *)a0, a1, &sip, &sport);
		if (n > 0) {
			*(unsigned int *)a2 = sip;
			*(unsigned short *)a3 = sport;
		}
		return (unsigned int)n;
	}
	default:
		return (unsigned int)-2;	/* ENOSYS */
	}
}

/*
 * Scheduler kooperatif App A2 (implementasi).
 *
 * Semua daemon berjalan di atas boot thread: trap SVC tidak pernah
 * kembali ke user_launch_init sampai semua daemon mati.  Setiap
 * SYS_YIELD menyimpan konteks user penuh (r0-r12, banked sp/lr USR,
 * pc, spsr) lalu memuat milik daemon berikutnya + ganti pmap.
 */
static void
ctx_save(struct user_ctx *c, struct arm_trap_frame *f)
{
	unsigned int i;
	for (i = 0; i < 13u; i++)
		c->r[i] = f->r[i];
	c->pc = f->lr;
	c->spsr = f->spsr;
	ctx_save_usr(&c->sp_usr, &c->lr_usr);
	c->r[0] = 0;	/* SYS_YIELD mengembalikan 0 saat thread kembali */
}

static void
ctx_load(struct arm_trap_frame *f, struct user_ctx *c)
{
	unsigned int i;
	for (i = 0; i < 13u; i++)
		f->r[i] = c->r[i];
	f->lr = c->pc;
	f->spsr = c->spsr;
	ctx_restore_usr(c->sp_usr, c->lr_usr);
}

static struct daemon *
pick_next(void)
{
	unsigned int i;
	for (i = 1; i <= NDAEMON; i++) {
		unsigned int j = (unsigned int)(sched_cur + (int)i) % NDAEMON;
		if (daemons[j].state)
			return &daemons[j];
	}
	return 0;
}

static void
sched_activate(struct daemon *d)
{
	cur_utask = d->task;
	cur_udesc = d->udesc;
	cur_progid = d->progid;
	arm_pmap_activate_user(d->pmap);
}

static void
sched_yield_switch(struct arm_trap_frame *f)
{
	struct daemon *cur, *next;
	/* Pump network sekali tiap yield: HTTP tetap hidup di sela
	 * daemon.  Driver net polling murni (tanpa IRQ) jadi aman
	 * dari konteks trap SVC. */
	net_pump();
	cur = &daemons[sched_cur];
	ctx_save(&cur->ctx, f);
	next = pick_next();
	if (next == cur || next == 0)
		return;	/* sendirian: frame tak diubah, r0=0 via caller */
	sched_cur = (int)(next - daemons);
	sched_activate(next);
	ctx_load(f, &next->ctx);
}

static void
sched_exit_switch(struct arm_trap_frame *f, unsigned int code)
{
	struct daemon *next;
	net_pump();
	printf("sched: '%s' exit 0x%x\n", daemons[sched_cur].name, code);
	daemons[sched_cur].state = 0;
	uprog_states[daemons[sched_cur].uprog_idx] = 2u;	/* EXITED */
	next = pick_next();
	if (next == 0) {
		/* Semua mati: trampoline terakhir. */
		f->r[0] = code;
		f->lr = (unsigned int)user_exit_trampoline;
		f->spsr = ARM_MODE_SVC;
		return;
	}
	sched_cur = (int)(next - daemons);
	sched_activate(next);
	ctx_load(f, &next->ctx);
}

/* Daemon didaftarkan dari user_launch_init (setelah one-shot). */
static void
sched_register_daemon(const char *name, task_t task, pmap_t pmap,
		      struct user_task *udesc, unsigned int stack_top,
		      unsigned int progid)
{
	struct daemon *d;
	unsigned int i;
	if (ndaemon_reg >= NDAEMON) {
		printf("sched: FAIL (daemon table penuh)\n");
		return;
	}
	d = &daemons[ndaemon_reg];
	d->name = name;
	d->task = task;
	d->pmap = pmap;
	d->udesc = udesc;
	for (i = 0; i < 13u; i++)
		d->ctx.r[i] = 0;
	d->ctx.sp_usr = stack_top;
	d->ctx.lr_usr = 0;
	d->ctx.pc = INIT_CODE_VA;
	d->ctx.spsr = 0x10u;	/* USR, IRQ on */
	d->state = 1;
	d->progid = (int)progid;
	d->uprog_idx = nuprog;
	if (nuprog < UPROG_MAX) {
		uprog_names[nuprog] = name;
		uprog_states[nuprog] = 0u;	/* RUNNABLE */
		nuprog++;
	}
	ndaemon_reg++;
	printf("sched: daemon '%s' terdaftar (progid %u)\n", name, progid);
}

/* Loop utama daemon: masuk user mode; kembali hanya bila semua mati. */
void	machine_halt(void);

static void
sched_run(void)
{
	struct daemon *d;
	unsigned int rc;
	sched_active = 1;
	d = pick_next();
	if (d == 0) {
		printf("sched: tidak ada daemon\n");
		return;
	}
	sched_cur = (int)(d - daemons);
	sched_activate(d);
	printf("sched: masuk user mode ('%s')...\n", d->name);
	rc = user_enter_test(d->ctx.sp_usr, d->ctx.pc);
	arm_pmap_activate_kernel();
	printf("sched: semua daemon mati (rc=0x%x)\n", rc);
	machine_halt();
}

/*
 * user_fault: a user-mode abort/undef becomes thread death, not a
 * kernel panic.  Redirect the trap frame to user_exit_trampoline with
 * a fault code in r0.
 */
void
user_fault(struct arm_trap_frame *frame, unsigned int code)
{
	/* App A2: fault di daemon = kematian thread itu saja, yang
	 * lain lanjut (crash isolation). */
	if (sched_active && sched_cur >= 0 && daemons[sched_cur].state) {
		printf("sched: '%s' FAULT 0x%x (diisolasi)\n",
		       daemons[sched_cur].name, code);
		sched_exit_switch(frame, code);
		return;
	}
	frame->r[0] = code;
	frame->lr = (unsigned int)user_exit_trampoline;
	frame->spsr = ARM_MODE_SVC;
}

/*
 * user_selftest
 */
void
user_selftest(void)
{
	task_t		utask;
	thread_t	uthread;
	pmap_t		upmap;
	kern_return_t	kr;
	vm_offset_t	mem, code_pa, stack_pa;
	unsigned int	code_len, rc, i;
	unsigned char	*dst;
	spl_t		s;

	printf("user_selftest: creating user task...\n");

	s = spl0();
	kr = task_create(kernel_task, FALSE, &utask);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_selftest: FAIL (task_create kr=%d)\n", kr);
		return;
	}
	/* A thread object in the task (scheduler dispatch is future work). */
	kr = thread_create(utask, &uthread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_selftest: FAIL (thread_create kr=%d)\n", kr);
		return;
	}
	upmap = utask->map->pmap;
	if (upmap == PMAP_NULL) {
		(void) splx(s);
		printf("user_selftest: FAIL (task has no pmap)\n");
		return;
	}

	/* Two physical pages (identity): code + user stack. */
	kr = kmem_alloc(kernel_map, &mem, 3 * USER_PGBYTES);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_selftest: FAIL (kmem_alloc kr=%d)\n", kr);
		return;
	}
	code_pa = trunc_page(mem);
	stack_pa = code_pa + USER_PGBYTES;
	(void) splx(s);

	/* ---- Phase A: clean write + exit ---- */
	code_len = (unsigned int)(_uprog_end - _uprog_start);
	dst = (unsigned char *)code_pa;
	for (i = 0; i < code_len; i++)
		dst[i] = _uprog_start[i];
	/* Fresh code: clean D-cache, invalidate I-cache before execute. */
	dcache_clean_range(code_pa, USER_PGBYTES);
	icache_invalidate_all();
	/* Zero the user stack. */
	dst = (unsigned char *)stack_pa;
	for (i = 0; i < USER_PGBYTES; i++)
		dst[i] = 0;

	pmap_enter(upmap, USER_CODE_VA, code_pa,
		   VM_PROT_READ | VM_PROT_WRITE, FALSE);
	pmap_enter(upmap, USER_STACK_VA, stack_pa,
		   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	printf("user_selftest: entering user mode (clean)...\n");
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(USER_STACK_VA + USER_PGBYTES, USER_CODE_VA);
	arm_pmap_activate_kernel();

	if (rc != 0) {
		printf("user_selftest: FAIL (clean exit code 0x%x)\n", rc);
		return;
	}
	printf("user_selftest: clean exit ok\n");

	/* ---- Phase B: fault isolation ---- */
	code_len = (unsigned int)(_uprog_end - _uprog_fault);
	dst = (unsigned char *)code_pa;
	for (i = 0; i < code_len; i++)
		dst[i] = _uprog_fault[i];
	dcache_clean_range(code_pa, USER_PGBYTES);
	icache_invalidate_all();

	printf("user_selftest: entering user mode (fault)...\n");
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(USER_STACK_VA + USER_PGBYTES, USER_CODE_VA);
	arm_pmap_activate_kernel();

	if (rc != USER_EXIT_DABT) {
		printf("user_selftest: FAIL (fault code 0x%x, want 0x%x)\n",
		       rc, USER_EXIT_DABT);
		return;
	}
	printf("user_selftest: PASS (user mode + syscall + fault isolation)\n");
}

/*
 * machine_halt -- Fase C: clean halt.  The CPU idles on wfi; the boot
 * log ends with the HALT marker for automated verification.
 */
void
machine_halt(void)
{
	printf("\nHALT: QaonicOS Fase C shutdown complete.\n");
	for (;;)
		__asm__ volatile ("wfi");
}

/*
 * setup_uprog_task -- App A2: bangun Mach task untuk satu program user
 * (task + thread + pmap + code/stack/heap) TANPA masuk user mode.
 * Dipakai launch_uprog (one-shot) dan launch_daemon (persisten).
 * Mengembalikan 0 bila sukses.
 */
static int
setup_uprog_task(const char *name, unsigned char *img, unsigned int img_len,
		 struct user_task *udesc, task_t *out_task, pmap_t *out_pmap)
{
	task_t		utask;
	thread_t	uthread;
	pmap_t		upmap;
	kern_return_t	kr;
	vm_offset_t	mem, pa, va;
	unsigned int	i, npages;
	unsigned char	*dst;
	spl_t		s;

	printf("setup_uprog_task: '%s'...\n", name);

	if (img_len == 0 || img_len > 16 * USER_PGBYTES) {
		printf("setup_uprog_task: FAIL (bad image size %u)\n", img_len);
		return -1;
	}

	s = spl0();
	kr = task_create(kernel_task, FALSE, &utask);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (task_create kr=%d)\n", kr);
		return -1;
	}
	kr = thread_create(utask, &uthread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (thread_create kr=%d)\n", kr);
		return -1;
	}
	upmap = utask->map->pmap;
	if (upmap == PMAP_NULL) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (task has no pmap)\n");
		return -1;
	}

	/* code pages + 1 stack page + heap pages (contiguous, identity). */
	npages = (img_len + USER_PGBYTES - 1) / USER_PGBYTES;
	kr = kmem_alloc(kernel_map, &mem,
			(npages + 1 + INIT_HEAP_PAGES) * USER_PGBYTES);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (kmem_alloc kr=%d)\n", kr);
		return -1;
	}
	(void) splx(s);

	/* code */
	pa = trunc_page(mem);
	dst = (unsigned char *)pa;
	for (i = 0; i < img_len; i++)
		dst[i] = img[i];
	dcache_clean_range(pa, npages * USER_PGBYTES);
	icache_invalidate_all();
	va = INIT_CODE_VA;
	for (i = 0; i < npages; i++, va += USER_PGBYTES, pa += USER_PGBYTES)
		pmap_enter(upmap, va, pa,
			   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	/* stack (zeroed) */
	dst = (unsigned char *)pa;
	for (i = 0; i < USER_PGBYTES; i++)
		dst[i] = 0;
	pmap_enter(upmap, INIT_STACK_VA, pa,
		   VM_PROT_READ | VM_PROT_WRITE, FALSE);
	pa += USER_PGBYTES;

	/* heap (zeroed sbrk region) */
	dst = (unsigned char *)pa;
	for (i = 0; i < INIT_HEAP_PAGES * USER_PGBYTES; i++)
		dst[i] = 0;
	va = INIT_HEAP_VA;
	for (i = 0; i < INIT_HEAP_PAGES; i++,
	     va += USER_PGBYTES, pa += USER_PGBYTES)
		pmap_enter(upmap, va, pa,
			   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	udesc->task = utask;
	udesc->heap_base = INIT_HEAP_VA;
	udesc->brk = INIT_HEAP_VA;
	udesc->heap_end = INIT_HEAP_VA + INIT_HEAP_PAGES * USER_PGBYTES;

	*out_task = utask;
	*out_pmap = upmap;
	return 0;
}

/*
 * launch_uprog -- Fase C: launch one user program as its own Mach task.
 *
 * Program berjalan sampai SYS_EXIT (one-shot, sequential); lalu
 * berikutnya.  Koordinasi antar program via ramfs sentinel files.
 *
 * Returns the program's exit code, or ~0u on launch failure.
 */
static unsigned int
launch_uprog(const char *name, unsigned char *img, unsigned int img_len,
	     struct user_task *udesc)
{
	task_t		utask;
	pmap_t		upmap;
	unsigned int	rc;

	printf("launch_uprog: creating task for '%s'...\n", name);
	if (setup_uprog_task(name, img, img_len, udesc, &utask, &upmap) != 0)
		return ~0u;

	/* Register for SYS_TLIST + set as current for syscalls. */
	if (nuprog < UPROG_MAX) {
		uprog_names[nuprog] = name;
		uprog_states[nuprog] = 0u;	/* RUNNABLE */
		nuprog++;
	}
	cur_utask = utask;
	cur_udesc = udesc;
	cur_progid = -1;	/* one-shot: bukan pemegang token */

	printf("launch_uprog: entering user mode ('%s')...\n", name);
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(INIT_STACK_VA + USER_PGBYTES, INIT_CODE_VA);
	arm_pmap_activate_kernel();

	if (nuprog > 0)
		uprog_states[nuprog - 1u] = 2u;	/* EXITED (DEAD) */
	cur_utask = TASK_NULL;
	cur_udesc = 0;
	cur_progid = -1;

	printf("launch_uprog: '%s' exited with code 0x%x\n", name, rc);
	return rc;
}

/*
 * launch_daemon -- App A2: daftarkan program user persisten (face,
 * uiapp) ke scheduler kooperatif.  Tidak masuk user mode di sini;
 * sched_run() yang menjalankannya bergantian nanti.
 */
static void
launch_daemon(const char *name, unsigned char *img, unsigned int img_len,
	      struct user_task *udesc, unsigned int progid)
{
	task_t	utask;
	pmap_t	upmap;

	if (setup_uprog_task(name, img, img_len, udesc, &utask, &upmap) != 0) {
		printf("launch_daemon: FAIL ('%s')\n", name);
		return;
	}
	sched_register_daemon(name, utask, upmap, udesc,
			    INIT_STACK_VA + USER_PGBYTES, progid);
}

/* Program user Fase C: diluncurkan berurutan. */
struct uprog_image {
	const char	*name;
	unsigned char	*img;
	unsigned int	*lenp;
};

/* Program one-shot Fase C/D: jalan berurutan sampai SYS_EXIT. */
static struct uprog_image uprogs[] = {
	{ "init",  init_img,  &init_img_len  },
	{ "ucat",  ucat_img,  &ucat_img_len  },
	{ "uls",   uls_img,   &uls_img_len   },
	{ "uecho", uecho_img, &uecho_img_len },
	{ "umon",  umon_img,  &umon_img_len  },
	{ "ugpio", ugpio_img, &ugpio_img_len },
	{ "usd",   usd_img,   &usd_img_len   },
	{ "ufs",   ufs_img,   &ufs_img_len   },
};
#define	NUPROGS	(sizeof(uprogs) / sizeof(uprogs[0]))

/* Daemon persisten App A2/A4: jalan di scheduler kooperatif. */
static struct uprog_image daemon_images[] = {
	{ "face",  face_img,  &face_img_len  },	/* progid 0: Qabot server */
	{ "uiapp", uiapp_img, &uiapp_img_len },	/* progid 1: menu */
	{ "ntp",   ntp_img,   &ntp_img_len   },	/* progid 2: sinkron jam */
};
#define	NDAEMON_IMAGES	(sizeof(daemon_images) / sizeof(daemon_images[0]))

static struct user_task	uprog_udesc[UPROG_MAX];

/*
 * user_launch_init -- Fase C/D + App A2.
 *
 * Called from startrtclock() after the self-tests.
 *  1. Program one-shot (init..ufs) jalan berurutan sampai SYS_EXIT;
 *     artefak ramfs diverifikasi.
 *  2. Network di-init (net_init_all); setelah ini net dipump dari
 *     scheduler tiap SYS_YIELD.
 *  3. Daemon persisten (face, uiapp) didaftarkan lalu sched_run()
 *     menjalankannya bergantian (tak kembali).
 */
extern void	net_init_all(void);

void
user_launch_init(void)
{
	unsigned i, fails = 0;
	static const char *want_files[] = {
		"/motd.txt", "/echo.txt", "/umon.out",
		"/gpio.out", "/sd.out", "/fat.out",
		"/.motd_ready", "/.ucat_done", "/.uls_done",
		"/.uecho_done", "/.umon_done",
		"/.gpio_cmd_ready", "/.ugpio_done",
		"/.sd_cmd_ready", "/.usd_done",
		"/.fat_cmd_ready", "/.ufs_done",
	};

	ramfs_init();

	/* The cooperative sched test disables the timer; user programs
	 * need ticks for SYS_UPTIME.  Enable it here, after all task setup
	 * (an immediate IRQ during task_create trips MI thread_select). */
	arm_timer_enable();

	for (i = 0; i < NUPROGS; i++) {
		unsigned int rc = launch_uprog(uprogs[i].name, uprogs[i].img,
					       *uprogs[i].lenp,
					       &uprog_udesc[i]);
		if (rc != 0) {
			printf("user_launch_init: FAIL ('%s' exit 0x%x)\n",
			       uprogs[i].name, rc);
			fails++;
		}
	}

	/* Verifikasi artefak koordinasi ramfs. */
	for (i = 0; i < sizeof(want_files) / sizeof(want_files[0]); i++) {
		if (!ramfs_exists(want_files[i])) {
			printf("user_launch_init: FAIL (missing %s)\n",
			       want_files[i]);
			fails++;
		}
	}

	if (fails == 0)
		printf("user_launch_init: PASS (%u/%u programs, %u/%u files)\n",
		       NUPROGS, NUPROGS,
		       (unsigned)(sizeof(want_files) / sizeof(want_files[0])),
		       (unsigned)(sizeof(want_files) / sizeof(want_files[0])));
	else
		printf("user_launch_init: %u FAILs\n", fails);

	/* Fase D: init network (blocking); pump-nya dari scheduler. */
	net_init_all();

	/* App A2: daftarkan daemon persisten, lalu jalan. */
	for (i = 0; i < NDAEMON_IMAGES; i++)
		launch_daemon(daemon_images[i].name, daemon_images[i].img,
			      *daemon_images[i].lenp,
			      &uprog_udesc[NUPROGS + i], i);
	sched_run();
	/* NOTREACHED */
}
