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

/* init_img: embedded init user program (Fase B, generated at build). */
extern unsigned char	init_img[];
extern unsigned int	init_img_len;

/* trap.c (Fase B) */
extern unsigned int	arm_timer_ticks(void);

/* clock.c (Fase B) */
extern void	arm_timer_enable(void);

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
 * Syscalls (Fase B ABI — kontinu dengan kernel lama;
 * lihat docs/SYSCALL-ABI.md).
 */
#define	SYS_WRITE	20u	/* write(fd, buf, len) -> len or -1 */
#define	SYS_YIELD	21u	/* yield remainder of slice -> 0 */
#define	SYS_EXIT	22u	/* exit(code) -> never returns */
#define	SYS_SBRK	24u	/* sbrk(incr) -> old brk or -1 */

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
 * Fase B user task bookkeeping.  One init task for now; the struct is
 * ready for more user tasks later (Fase C).
 */
struct user_task {
	task_t		task;
	vm_offset_t	heap_base;	/* VA: start of sbrk region */
	vm_offset_t	brk;		/* current break */
	vm_offset_t	heap_end;	/* VA: hard limit */
};

static struct user_task	init_utask;

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
		if (a1 < USER_VA_BASE ||
		    a2 > USER_VA_TOP - USER_VA_BASE ||
		    a1 + a2 > USER_VA_TOP ||
		    a1 + a2 < a1)
			return (unsigned int)-1;
		p = (const unsigned char *)a1;
		for (i = 0; i < a2; i++)
			uart_putc((char)p[i]);
		return a2;
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

		if (a0 == 0)
			return init_utask.brk;
		if (a0 > USER_VA_TOP - init_utask.brk)
			return (unsigned int)-1;
		new_brk = init_utask.brk + a0;
		if (new_brk > init_utask.heap_end)
			return (unsigned int)-1;
		a0 = init_utask.brk;	/* old brk = return value */
		init_utask.brk = new_brk;
		return a0;
	}
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
 * machine_halt -- Fase B: clean halt.  The CPU idles on wfi; the boot
 * log ends with the HALT marker for automated verification.
 */
void
machine_halt(void)
{
	printf("\nHALT: QaonicOS Fase B shutdown complete.\n");
	for (;;)
		__asm__ volatile ("wfi");
}

/*
 * user_launch_init -- Fase B: launch the first user task (init) at boot.
 *
 * Called from startrtclock() after the self-tests.  Builds a user task
 * from the embedded init image (init_img, generated at build time),
 * maps code + stack + heap pages, and enters USR mode (genuinely
 * unprivileged) via user_enter_test().  When init performs SYS_EXIT,
 * control returns here and the machine halts cleanly.
 */
void
user_launch_init(void)
{
	task_t		utask;
	thread_t	uthread;
	pmap_t		upmap;
	kern_return_t	kr;
	vm_offset_t	mem, pa, va;
	unsigned int	i, npages, rc;
	unsigned char	*dst;
	spl_t		s;

	printf("user_launch_init: creating init task...\n");

	if (init_img_len == 0 || init_img_len > 8 * USER_PGBYTES) {
		printf("user_launch_init: FAIL (bad init image size %u)\n",
		       init_img_len);
		return;
	}

	s = spl0();
	kr = task_create(kernel_task, FALSE, &utask);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_launch_init: FAIL (task_create kr=%d)\n", kr);
		return;
	}
	kr = thread_create(utask, &uthread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_launch_init: FAIL (thread_create kr=%d)\n", kr);
		return;
	}
	upmap = utask->map->pmap;
	if (upmap == PMAP_NULL) {
		(void) splx(s);
		printf("user_launch_init: FAIL (task has no pmap)\n");
		return;
	}

	/* code pages + 1 stack page + heap pages (contiguous, identity). */
	npages = (init_img_len + USER_PGBYTES - 1) / USER_PGBYTES;
	kr = kmem_alloc(kernel_map, &mem,
			(npages + 1 + INIT_HEAP_PAGES) * USER_PGBYTES);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_launch_init: FAIL (kmem_alloc kr=%d)\n", kr);
		return;
	}
	(void) splx(s);

	/* code */
	pa = trunc_page(mem);
	dst = (unsigned char *)pa;
	for (i = 0; i < init_img_len; i++)
		dst[i] = init_img[i];
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

	init_utask.task = utask;
	init_utask.heap_base = INIT_HEAP_VA;
	init_utask.brk = INIT_HEAP_VA;
	init_utask.heap_end = INIT_HEAP_VA + INIT_HEAP_PAGES * USER_PGBYTES;

	printf("user_launch_init: entering user mode (init)...\n");
	/* The cooperative sched test disables the timer; init needs
	 * ticks for SYS_YIELD.  Enable it here, after all task setup
	 * (an immediate IRQ during task_create trips MI thread_select). */
	arm_timer_enable();
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(INIT_STACK_VA + USER_PGBYTES, INIT_CODE_VA);
	arm_pmap_activate_kernel();

	printf("user_launch_init: init exited with code 0x%x\n", rc);
	if (rc == 0)
		printf("user_launch_init: PASS (init ran as user task)\n");
	else
		printf("user_launch_init: FAIL (init exit code 0x%x)\n", rc);
	machine_halt();
	/* NOTREACHED */
}
