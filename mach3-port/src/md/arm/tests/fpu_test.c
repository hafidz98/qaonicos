/*
 * mach3-port/src/md/arm/tests/fpu_test.c -- K1 (THR-1) FPU/NEON
 * save/restore selftest.
 *
 * Two kernel threads own distinct VFP/NEON register patterns and
 * switch between each other cooperatively while the 100 Hz timer
 * is live.  Each round is: burn CPU (timer ticks fire), switch
 * to the other thread, check the pattern bit-exact, re-set it.
 * Because each thread re-sets only AFTER its check, every check
 * observes the other thread's writes: without per-thread
 * save/restore in __Switch_context every check fails; with it
 * every check passes.  (39 checks total: A's 20 + B's 19;
 * B's final check is cut off when A prints the verdict and
 * continues boot -- B is left suspended in its last switch.)
 *
 * The switch path proven here (__Switch_context, keyed on the
 * pcb = thread identity) is the same path every other switch
 * uses: cooperative switch_context, stack_handoff, and the
 * scheduler's thread_block/thread_dispatch path (which the IPC
 * park/wake cycle funnels through).  There is no forcible timer
 * preemption in this kernel (thread_exception_return is a
 * no-op stub), so "under timer preemption" here means: the
 * 100 Hz timer fires throughout the test (ticks counted
 * before/after) and any AST switch would take the same hook.
 *
 * Called from startrtclock() (clock.c) after user_selftest()
 * and before sched_selftest().  Boot-flow note: current_thread()
 * is NULL in startrtclock, so there is NO boot thread to switch
 * back to (same constraint as sched_test).  On the success path
 * this function never returns: worker A prints the verdict and
 * calls sched_selftest() itself (which never returns: its
 * worker launches init).  On setup failure it prints FAIL and
 * returns so boot continues normally.  Worker B is left
 * suspended inside its last switch (same as sched_test's
 * worker_b); nobody switches to it again.
 *
 * Markers (grepped by scripts/verify-k1.py):
 *   fpu_selftest: PASS rounds=20 checks=39 ticks=..
 *   fpu_selftest: FAIL <detail>
 */
#include <mach/machine/vm_types.h>
#include <kern/thread.h>	/* thread_t, current_thread(), THREAD_NULL */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <machine/machspl.h>	/* spl_t, spl0, splx */
#include <machine/thread.h>	/* struct pcb (for the layout note) */

/* From uart.c */
extern void	uart_putc(char c);
extern task_t	kernel_task;
/* From trap.c */
extern unsigned int	arm_timer_ticks(void);
/* From clock.c's sched flow: the next boot phase. */
extern void	sched_selftest(void);

#define	FPU_ROUNDS	20

static void
fpu_putc(char c)
{
	uart_putc(c);
}

static void
fpu_puts(const char *s)
{
	while (*s)
		uart_putc(*s++);
}

static void
fpu_putu(unsigned int v)
{
	char b[12];
	int i = 0;
	if (v == 0u) {
		uart_putc('0');
		return;
	}
	while (v != 0u) {
		b[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	}
	while (i > 0)
		uart_putc(b[--i]);
}

/* Distinct patterns: A words 0xA......., B words 0xB....... */
static unsigned int	pat_a[64];
static unsigned int	pat_b[64];
static unsigned int	scr[64];	/* readback scratch (single CPU) */
static unsigned int	fpu_fails;
static unsigned int	fpu_checks;
static unsigned int	fpu_t0;
static thread_t		fpu_worker_a = THREAD_NULL;
static thread_t		fpu_worker_b = THREAD_NULL;

static void
pat_init(void)
{
	int i;
	for (i = 0; i < 64; i++) {
		pat_a[i] = 0xa0000000u + (unsigned int)i * 0x00010001u;
		pat_b[i] = 0xb0000000u + (unsigned int)i * 0x00010001u;
	}
}

static void
fpu_set(const unsigned int *pat, unsigned int fpscr)
{
	__asm__ volatile (
		"vldmia %0!, {d0-d15}\n"
		"vldmia %0!, {d16-d31}\n"
		"vmsr fpscr, %1\n"
		: "+r" (pat) : "r" (fpscr) : "memory");
}

/* Returns 0 if regs match pat/fpscr, else 1 + first bad word index. */
static int
fpu_check(const unsigned int *pat, unsigned int fpscr, int *bad)
{
	unsigned int got_fpscr;
	unsigned int *s = scr;
	int i;

	__asm__ volatile (
		"vstmia %0!, {d0-d15}\n"
		"vstmia %0!, {d16-d31}\n"
		"vmrs %1, fpscr\n"
		: "+r" (s), "=r" (got_fpscr) : : "memory");
	for (i = 0; i < 64; i++) {
		if (scr[i] != pat[i]) {
			*bad = i;
			return 1;
		}
	}
	if (got_fpscr != fpscr) {
		*bad = 64;
		return 1;
	}
	return 0;
}

static void
fpu_burn(void)
{
	volatile unsigned int i;
	for (i = 0; i < 1000000u; i++)
		__asm__ volatile ("" : : : "memory");
}

static void
fpu_verdict_and_continue(void)
{
	unsigned int t1 = arm_timer_ticks();

	if (fpu_fails != 0) {
		fpu_puts("fpu_selftest: FAIL fails=");
		fpu_putu(fpu_fails);
		fpu_puts(" checks=");
		fpu_putu(fpu_checks);
		fpu_puts("\n");
	} else {
		fpu_puts("fpu_selftest: PASS rounds=");
		fpu_putu((unsigned int)FPU_ROUNDS);
		fpu_puts(" checks=");
		fpu_putu(fpu_checks);
		fpu_puts(" ticks=");
		fpu_putu(t1 - fpu_t0);
		fpu_puts("\n");
	}
	/* Next boot phase; never returns (its worker launches init). */
	sched_selftest();
	for (;;)
		__asm__ volatile ("wfi");
}

static void
fpu_thread_a(void)
{
	int r, bad;

	fpu_set(pat_a, 0x00000000u);
	for (r = 0; r < FPU_ROUNDS; r++) {
		fpu_burn();
		(void) switch_context(current_thread(), (void *)0,
			fpu_worker_b);
		fpu_checks++;
		if (fpu_check(pat_a, 0x00000000u, &bad) != 0) {
			fpu_fails++;
			fpu_puts("fpu_selftest: MISMATCH A round=");
			fpu_putu((unsigned int)r);
			fpu_puts(" word=");
			fpu_putu((unsigned int)bad);
			fpu_puts("\n");
		}
		fpu_set(pat_a, 0x00000000u);
	}
	fpu_verdict_and_continue();
	/* NOTREACHED */
	for (;;)
		__asm__ volatile ("wfi");
}

static void
fpu_thread_b(void)
{
	int r, bad;

	fpu_set(pat_b, 0x20000000u);
	for (r = 0; r < FPU_ROUNDS; r++) {
		fpu_burn();
		(void) switch_context(current_thread(), (void *)0,
			fpu_worker_a);
		fpu_checks++;
		if (fpu_check(pat_b, 0x20000000u, &bad) != 0) {
			fpu_fails++;
			fpu_puts("fpu_selftest: MISMATCH B round=");
			fpu_putu((unsigned int)r);
			fpu_puts(" word=");
			fpu_putu((unsigned int)bad);
			fpu_puts("\n");
		}
		fpu_set(pat_b, 0x20000000u);
	}
	/* Never resumed (A prints the verdict and continues boot). */
	for (;;)
		__asm__ volatile ("wfi");
}

void
fpu_selftest(void)
{
	spl_t s;

	fpu_puts("fpu_selftest: two threads, distinct VFP patterns...\n");
	pat_init();
	fpu_fails = 0;
	fpu_checks = 0;

	s = spl0();
	if (thread_create(kernel_task, &fpu_worker_a) == 0) {
		thread_start(fpu_worker_a, fpu_thread_a);
		thread_doswapin(fpu_worker_a);
		fpu_worker_a->pcb->kss.lr = (unsigned int)fpu_thread_a;
	}
	if (thread_create(kernel_task, &fpu_worker_b) == 0) {
		thread_start(fpu_worker_b, fpu_thread_b);
		thread_doswapin(fpu_worker_b);
		fpu_worker_b->pcb->kss.lr = (unsigned int)fpu_thread_b;
	}
	(void) splx(s);

	if (fpu_worker_a == THREAD_NULL || fpu_worker_b == THREAD_NULL) {
		fpu_puts("fpu_selftest: FAIL (thread_create)\n");
		return;
	}

	fpu_t0 = arm_timer_ticks();
	/* No boot thread to switch back to (current_thread() is NULL
	 * in startrtclock): worker A continues boot via
	 * sched_selftest() when done.  This call never returns. */
	(void) switch_context(THREAD_NULL, (void *)0, fpu_worker_a);
	/* NOTREACHED on the success path. */
	fpu_puts("fpu_selftest: FAIL (switch returned)\n");
}
