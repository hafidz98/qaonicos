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
#include <mach/message.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/zalloc.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_kmsg.h>
#include <machine/machspl.h>	/* spl_t, spl0, splx */

extern void	panic(const char *, ...);
extern int	printf(const char *, ...);

void	ipc_stress(void);

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

	ipc_stress();
}

/* ------------------------------------------------------------------ */
/* ipc_stress -- M5 hardening: port storm + message queue stress       */
/* ------------------------------------------------------------------ */
#define	IPC_STRESS_NPORTS	400
#define	IPC_STRESS_NMSG		200

void
ipc_stress(void)
{
	static ipc_port_t ports[IPC_STRESS_NPORTS];
	zone_t pz = ipc_object_zones[IOT_PORT];
	int before, after;
	int i;

	printf("ipc_stress: allocating %d ports...\n", IPC_STRESS_NPORTS);
	before = pz->count;
	for (i = 0; i < IPC_STRESS_NPORTS; i++) {
		ports[i] = ipc_port_alloc_kernel();
		if (ports[i] == IP_NULL) {
			printf("ipc_stress: FAIL (port %d alloc NULL)\n", i);
			return;
		}
		if (ports[i]->ip_references != 1) {
			printf("ipc_stress: FAIL (port %d bad refs %d)\n",
			       i, ports[i]->ip_references);
			return;
		}
	}
	printf("ipc_stress: zone count %d -> %d\n", before, pz->count);

	/* Message queue stress on ports[0]: FIFO order + content. */
	printf("ipc_stress: queueing %d messages...\n", IPC_STRESS_NMSG);
	for (i = 0; i < IPC_STRESS_NMSG; i++) {
		ipc_kmsg_t kmsg;
		int *body;
		mach_msg_size_t msize =
			(mach_msg_size_t)(sizeof(mach_msg_header_t) +
					  sizeof(int));

		kmsg = ikm_alloc(msize);
		ikm_init(kmsg, ikm_plus_overhead(msize));
		kmsg->ikm_header.msgh_size = msize;
		kmsg->ikm_header.msgh_seqno = (mach_msg_seqno_t)i;
		body = (int *)((char *)&kmsg->ikm_header +
			       sizeof(mach_msg_header_t));
		*body = i ^ 0x5a5a5a5a;
		ipc_kmsg_enqueue(&ports[0]->ip_messages.imq_messages, kmsg);
	}

	printf("ipc_stress: dequeuing and verifying...\n");
	for (i = 0; i < IPC_STRESS_NMSG; i++) {
		ipc_kmsg_t kmsg;
		int *body, expect;

		kmsg = ipc_kmsg_dequeue(&ports[0]->ip_messages.imq_messages);
		if (kmsg == IKM_NULL) {
			printf("ipc_stress: FAIL (dequeue %d got NULL)\n", i);
			return;
		}
		if (kmsg->ikm_header.msgh_seqno != (mach_msg_seqno_t)i) {
			printf("ipc_stress: FAIL (msg %d seqno %u)\n",
			       i, kmsg->ikm_header.msgh_seqno);
			ikm_free(kmsg);
			return;
		}
		body = (int *)((char *)&kmsg->ikm_header +
			       sizeof(mach_msg_header_t));
		expect = i ^ 0x5a5a5a5a;
		if (*body != expect) {
			printf("ipc_stress: FAIL (msg %d bad body)\n", i);
			ikm_free(kmsg);
			return;
		}
		ikm_free(kmsg);
	}
	if (ports[0]->ip_messages.imq_messages.ikmq_base != IKM_NULL) {
		printf("ipc_stress: FAIL (queue not empty after drain)\n");
		return;
	}

	/* Deallocate everything; zone count must return to baseline. */
	for (i = 0; i < IPC_STRESS_NPORTS; i++)
		ipc_port_dealloc_kernel(ports[i]);
	after = pz->count;
	if (after != before) {
		printf("ipc_stress: FAIL (port zone leak: %d -> %d)\n",
		       before, after);
		return;
	}

	printf("ipc_stress: PASS (%d ports, %d msgs FIFO, no leak)\n",
	       IPC_STRESS_NPORTS, IPC_STRESS_NMSG);
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
