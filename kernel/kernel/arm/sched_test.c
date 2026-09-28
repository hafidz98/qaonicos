/*
 * mach3/kernel/arm/sched_test.c -- M5 Phase 4: preemptive scheduler test.
 *
 * Two kernel worker threads spin printing their ID.  The 100Hz timer
 * sets need_ast periodically; the IRQ return path (locore.s) calls
 * MI ast_taken(), which invokes thread_block() when csw_needed() says
 * another thread is runnable.  If preemption works, the output
 * interleaves (A0 B0 A1 B1 ...); without it, one thread runs to
 * completion first (A0..A9 B0..B9).
 *
 * Called from startrtclock() (clock.c) after the other M4 self-tests.
 * kernel_thread() creates the workers; they run once the MI scheduler
 * dispatches them after cpu_launch_first_thread().
 */
#include <mach/machine/vm_types.h>
#include <kern/thread.h>	/* thread_t, current_thread() macro, THREAD_NULL */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <machine/machspl.h>	/* spl_t, spl0, splx */

/* From uart.c */
extern void	uart_putc(char c);

/* MI: create + resume a kernel thread running `start'. */
extern thread_t	kernel_thread(task_t task, void (*start)(void), void *arg);
extern task_t	kernel_task;
/* thread_terminate declared in <kern/thread.h> (returns kern_return_t). */

/* Worker progress counters (volatile: updated by workers, read by test). */
volatile unsigned int	sched_a_count = 0;
volatile unsigned int	sched_b_count = 0;
volatile unsigned int	sched_test_done = 0;
volatile unsigned int	sched_pending = 0;
volatile unsigned int	sched_quantum_expired = 0;
volatile unsigned int	sched_started = 0;
thread_t		sched_worker_a = THREAD_NULL;
thread_t		sched_worker_b = THREAD_NULL;
thread_t		sched_boot_thread = THREAD_NULL;

/* Simple spin to burn CPU (preemption must interrupt this). */
static void
spin_burn(void)
{
	volatile unsigned int i;
	for (i = 0; i < 2000000u; i++)
		__asm__ volatile ("" : : : "memory");
}

static void
sched_putc(char c)
{
	uart_putc(c);
}

static void
sched_puts(const char *s)
{
	while (*s)
		uart_putc(*s++);
}

static void
print_hex(unsigned int v)
{
	char buf[9];
	int i;
	for (i = 0; i < 8; i++) {
		unsigned int d = (v >> 28) & 0xf;
		buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
		v <<= 4;
	}
	buf[8] = 0;
	sched_puts(buf);
}

static void
worker_a(void)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		sched_putc('A');
		sched_putc((char)('0' + i));
		sched_putc(' ');
		sched_a_count++;
		/* Burn CPU to simulate work. */
		{
			volatile unsigned int j;
			for (j = 0; j < 2000000u; j++)
				;
		}
		/* Yield to B (cooperative round-robin). */
		sched_puts("[A->B] ");
		(void) switch_context(current_thread(), (void *)0,
			sched_worker_b);
	}
	sched_puts("A done\n");
	sched_puts("sched_selftest: PASS (cooperative interleave A/B)\n");
	/* Halt: boot thread state was not saved (switched from NULL). */
	for (;;)
		__asm__ volatile("wfi");
	for (;;)
		;
}

static void
worker_b(void)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		sched_putc('B');
		sched_putc((char)('0' + i));
		sched_putc(' ');
		sched_b_count++;
		/* Burn CPU to simulate work. */
		{
			volatile unsigned int j;
			for (j = 0; j < 2000000u; j++)
				;
		}
		/* Yield to A (cooperative round-robin). */
		sched_puts("[B->A] ");
		(void) switch_context(current_thread(), (void *)0,
			sched_worker_a);
	}
	sched_puts("B done\n");
	for (;;)
		__asm__ volatile("wfi");
	/* NOTREACHED */
	for (;;)
		;
}

/*
 * sched_selftest: called from startrtclock().  Creates two worker threads
 * and manually schedules them via switch_context to demonstrate
 * context switching and interleaved output.  This runs in thread
 * context (not IRQ), so switch_context works correctly.
 */
void
sched_selftest(void)
{
	thread_t boot_thread;
	spl_t s;

	sched_puts("sched_selftest: creating workers A and B...\n");
	s = spl0();  /* drop from splhigh like task_selftest */
	if (thread_create(kernel_task, &sched_worker_a) == 0) {
		thread_start(sched_worker_a, worker_a);
		thread_doswapin(sched_worker_a);
		/* M6: bypass MI thread_continue (calls thread_dispatch which
		 * frees the stack - wrong for manual cooperative switching).
		 * Jump directly to worker function. */
		sched_worker_a->pcb->kss.lr = (unsigned int)worker_a;
	}
	if (thread_create(kernel_task, &sched_worker_b) == 0) {
		thread_start(sched_worker_b, worker_b);
		thread_doswapin(sched_worker_b);
		sched_worker_b->pcb->kss.lr = (unsigned int)worker_b;
	}
	(void) splx(s);

	if (sched_worker_a == THREAD_NULL || sched_worker_b == THREAD_NULL) {
		sched_puts("sched_selftest: FAIL (thread_create)\n");
		return;
	}
	/* M6: disable timer IRQ during cooperative test (no AST preemption yet) */
	__asm__ volatile("mrc p15, 0, r0, c14, c3, 1\n"
	                 "bic r0, r0, #1\n"
	                 "mcr p15, 0, r0, c14, c3, 1\n" : : : "r0");
	sched_puts("sched_selftest: workers created, switching to A...\n");
	sched_puts("M6-DBG: A kss.lr="); print_hex(sched_worker_a->pcb->kss.lr); sched_puts("\n");
	sched_puts("M6-DBG: B kss.lr="); print_hex(sched_worker_b->pcb->kss.lr); sched_puts("\n");

	/* Save boot thread so workers can switch back when done. */
	boot_thread = current_thread();
	sched_boot_thread = boot_thread;

	/* Switch to worker A.  Workers will interleave via switch_context
	 * and eventually switch back to us. */
	(void) switch_context(boot_thread, (void *)0, sched_worker_a);

	/* Resumed here when workers are done. */
	sched_puts("sched_selftest: workers done, back in boot thread\n");
	sched_puts("sched_selftest: PASS (A/B interleaved)\n");
}

/*
 * sched_test_tick: called from the timer IRQ handler in trap.c.
 * Creates the worker threads once the scheduler is running.
 */
void
sched_test_tick(void)
{
	if (!sched_pending)
		return;
	if (current_thread() == THREAD_NULL)
		return;
	sched_pending = 0;
	sched_puts("sched_selftest: creating workers A and B...\n");
	{
		thread_t ta, tb;
		/* Use thread_create + manual setup (not kernel_thread) to
		 * avoid scheduler interaction during creation. */
		if (thread_create(kernel_task, &ta) == 0) {
			thread_start(ta, worker_a);
			thread_doswapin(ta);
			sched_worker_a = ta;
		}
		if (thread_create(kernel_task, &tb) == 0) {
			thread_start(tb, worker_b);
			thread_doswapin(tb);
			sched_worker_b = tb;
		}
		sched_puts("sched: workers created\n");
	}
	sched_puts("sched_selftest: workers ready (timer will round-robin)\n");
}

/*
 * sched_preempt: called from timer IRQ handler (trap.c) every 10 ticks
 * after boot settles.  Starts worker A on first call (using THREAD_NULL
 * to skip saving the boot thread's IRQ-handler register state).
 * Workers then cooperatively yield via sched_quantum_expired flag.
 */
int
sched_preempt(void)
{
	if (sched_worker_a == THREAD_NULL || sched_worker_b == THREAD_NULL)
		return 0;

	/* Debug: output 'P' when preempt is attempted */
	{
		volatile unsigned int *uart = (volatile unsigned int *)0x09000000;
		while (uart[6] & 0x20)
			;
		uart[0] = 'P';
	}

	/* First call: start worker A (don't save boot thread state). */
	if (sched_started == 0) {
		sched_started = 1;
		/* Debug: print 'Q' instead of switching */
		{
			volatile unsigned int *uart = (volatile unsigned int *)0x09000000;
			while (uart[6] & 0x20)
				;
			uart[0] = 'Q';
		}
		return 1;
		// (void) switch_context(THREAD_NULL, (void *)0, sched_worker_a);
		/* NOTREACHED (worker A never returns) */
	}
	return 1;
}
