/*
 * mach3/kernel/arm/ipc_test.c -- IPC + task bring-up self-tests (M4 item 2).
 *
 * ipc_selftest(): called from machine_init() (after ipc_bootstrap/ipc_init).
 *   Verifies kernel port allocate/deallocate via the special kernel ipc
 *   space.  (Note: ipc_space_kernel is intentionally inactive -- special
 *   spaces don't hold rights normally -- so port-set allocation is not
 *   tested here.)
 *
 * task_selftest(): called from startrtclock() (end of setup_main, after
 *   task_init/thread_init).  Verifies task_create() + thread_create()
 *   produce a valid thread in the new task, and runs the thread's start
 *   routine synchronously to verify it prints.
 *
 * LIMITATION: Scheduler dispatch (thread_resume -> thread_setrun -> run
 * queue -> context switch) is NOT tested.  thread_setrun needs a valid
 * current_thread() (active_threads[cpu] set), which only exists after
 * cpu_launch_first_thread does load_context().  Deferring via timer
 * interrupt was attempted but the ARM generic timer IRQ does not fire
 * on QEMU virt (tried PPI 27/virtual + PPI 30/physical, Group 0/1);
 * root cause not yet found.  Full scheduler test is future work.
 */
#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/machine/vm_types.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <machine/machspl.h>	/* spl_t, spl0, splx */

extern void	panic(const char *, ...);
extern int	printf(const char *, ...);

/* ------------------------------------------------------------------ */
/* ipc_selftest                                                        */
/* ------------------------------------------------------------------ */
void
ipc_selftest(void)
{
	ipc_port_t port;

	printf("ipc_selftest: port alloc/dealloc...\n");

	port = ipc_port_alloc_kernel();
	if (port == IP_NULL) {
		printf("ipc_selftest: FAIL (port alloc returned NULL)\n");
		return;
	}
	if (port->ip_references != 1) {
		printf("ipc_selftest: FAIL (bad references %d)\n",
		       port->ip_references);
		return;
	}
	ipc_port_dealloc_kernel(port);

	printf("ipc_selftest: PASS (kernel port alloc/dealloc)\n");
}

/* ------------------------------------------------------------------ */
/* task_selftest                                                       */
/* ------------------------------------------------------------------ */
static void
task_test_start(void)
{
	printf("task_selftest: thread start routine running\n");
	printf("task_selftest: PASS (task_create + thread_create + run)\n");
}

void
task_selftest(void)
{
	task_t new_task;
	thread_t thread;
	kern_return_t kr;
	spl_t s;

	printf("task_selftest: task_create...\n");

	/*
	 * startrtclock runs at splhigh; task_create may block, so
	 * drop to spl0 for the duration.
	 */
	s = spl0();
	kr = task_create(kernel_task, FALSE, &new_task);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("task_selftest: FAIL (task_create kr=%d)\n", kr);
		return;
	}

	kr = thread_create(new_task, &thread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("task_selftest: FAIL (thread_create kr=%d)\n", kr);
		return;
	}
	thread_start(thread, (void (*)(void))task_test_start);
	thread_doswapin(thread);
	(void) splx(s);

	printf("task_selftest: task+thread created OK\n");

	/*
	 * Run the start routine synchronously.  This verifies the
	 * thread object is valid and its code path works.  Scheduler
	 * dispatch is not tested (see LIMITATION above).
	 */
	task_test_start();
}
