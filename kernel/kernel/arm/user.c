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

/* trap.c (Fase B) */
extern unsigned int	arm_timer_ticks(void);

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

/* Daftar program user untuk SYS_TLIST (Fase C). */
#define	UPROG_MAX	8u
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
unsigned int
user_syscall(struct arm_trap_frame *frame)
{
	unsigned int num = frame->r[7];
	unsigned int a0 = frame->r[0];
	unsigned int a1 = frame->r[1];
	unsigned int a2 = frame->r[2];

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
	case SYS_YIELD: {
		/* Cooperative yield: wait for the next 100Hz timer tick.
		 * IRQs are on in trap context (SVC entry preserves the I
		 * flag; user mode runs with IRQs enabled), so wfi wakes
		 * on the tick.  The spin cap is a safety net only. */
		unsigned int t0 = arm_timer_ticks();
		unsigned int spins = 0;

		__asm__ volatile ("cpsie i" ::: "memory");
		while (arm_timer_ticks() == t0 && spins < 1000000u) {
			__asm__ volatile ("wfi" ::: "memory");
			spins++;
		}
		return 0;
	}
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
	case SYS_EXIT:
		/* Leave user mode for good: resume at the trampoline
		 * in SVC; r0 carries the exit code. */
		frame->r[0] = a0;
		frame->lr = (unsigned int)user_exit_trampoline;
		frame->spsr = ARM_MODE_SVC;
		return a0;
	default:
		return (unsigned int)-2;	/* ENOSYS */
	}
}

/*
 * user_fault: a user-mode abort/undef becomes thread death, not a
 * kernel panic.  Redirect the trap frame to user_exit_trampoline with
 * a fault code in r0.
 */
void
user_fault(struct arm_trap_frame *frame, unsigned int code)
{
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
 * launch_uprog -- Fase C: launch one user program as its own Mach task.
 *
 * Generalized from the Fase B init launcher: builds a user task from
 * an embedded image, maps code + stack + heap pages, enters USR mode
 * via user_enter_test().  When the program performs SYS_EXIT, control
 * returns here with the exit code.  Programs run SEQUENTIALLY
 * (cooperative, no preemption in this port); coordination between
 * programs uses ramfs sentinel files.
 *
 * Returns the program's exit code, or ~0u on launch failure.
 */
static unsigned int
launch_uprog(const char *name, unsigned char *img, unsigned int img_len,
	     struct user_task *udesc)
{
	task_t		utask;
	thread_t	uthread;
	pmap_t		upmap;
	kern_return_t	kr;
	vm_offset_t	mem, pa, va;
	unsigned int	i, npages, rc;
	unsigned char	*dst;
	spl_t		s;

	printf("launch_uprog: creating task for '%s'...\n", name);

	if (img_len == 0 || img_len > 8 * USER_PGBYTES) {
		printf("launch_uprog: FAIL (bad image size %u)\n", img_len);
		return ~0u;
	}

	s = spl0();
	kr = task_create(kernel_task, FALSE, &utask);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("launch_uprog: FAIL (task_create kr=%d)\n", kr);
		return ~0u;
	}
	kr = thread_create(utask, &uthread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("launch_uprog: FAIL (thread_create kr=%d)\n", kr);
		return ~0u;
	}
	upmap = utask->map->pmap;
	if (upmap == PMAP_NULL) {
		(void) splx(s);
		printf("launch_uprog: FAIL (task has no pmap)\n");
		return ~0u;
	}

	/* code pages + 1 stack page + heap pages (contiguous, identity). */
	npages = (img_len + USER_PGBYTES - 1) / USER_PGBYTES;
	kr = kmem_alloc(kernel_map, &mem,
			(npages + 1 + INIT_HEAP_PAGES) * USER_PGBYTES);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("launch_uprog: FAIL (kmem_alloc kr=%d)\n", kr);
		return ~0u;
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

	/* Register for SYS_TLIST + set as current for syscalls. */
	if (nuprog < UPROG_MAX) {
		uprog_names[nuprog] = name;
		uprog_states[nuprog] = 0u;	/* RUNNABLE */
		nuprog++;
	}
	cur_utask = utask;
	cur_udesc = udesc;

	printf("launch_uprog: entering user mode ('%s')...\n", name);
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(INIT_STACK_VA + USER_PGBYTES, INIT_CODE_VA);
	arm_pmap_activate_kernel();

	if (nuprog > 0)
		uprog_states[nuprog - 1u] = 2u;	/* EXITED (DEAD) */
	cur_utask = TASK_NULL;
	cur_udesc = 0;

	printf("launch_uprog: '%s' exited with code 0x%x\n", name, rc);
	return rc;
}

/* Program user Fase C: diluncurkan berurutan. */
struct uprog_image {
	const char	*name;
	unsigned char	*img;
	unsigned int	*lenp;
};

static struct uprog_image uprogs[] = {
	{ "init",  init_img,  &init_img_len  },
	{ "ucat",  ucat_img,  &ucat_img_len  },
	{ "uls",   uls_img,   &uls_img_len   },
	{ "uecho", uecho_img, &uecho_img_len },
	{ "umon",  umon_img,  &umon_img_len  },
};
#define	NUPROGS	(sizeof(uprogs) / sizeof(uprogs[0]))

static struct user_task	uprog_udesc[UPROG_MAX];

/*
 * user_launch_init -- Fase C: launch all user programs sequentially,
 * then verify the ramfs coordination artifacts.
 *
 * Called from startrtclock() after the self-tests.  Each program runs
 * to SYS_EXIT before the next is launched.  After the last program,
 * the kernel verifies the sentinel files the programs were supposed
 * to create, prints the verdict, and halts cleanly.
 */
void
user_launch_init(void)
{
	unsigned i, fails = 0;
	static const char *want_files[] = {
		"/motd.txt", "/echo.txt", "/umon.out",
		"/.motd_ready", "/.ucat_done", "/.uls_done",
		"/.uecho_done", "/.umon_done",
	};

	ramfs_init();

	/* The cooperative sched test disables the timer; user programs
	 * need ticks for SYS_YIELD.  Enable it here, after all task setup
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
		printf("user_launch_init: PASS (5/5 programs, 8/8 files)\n");
	else
		printf("user_launch_init: %u FAILs\n", fails);
	machine_halt();
	/* NOTREACHED */
}
