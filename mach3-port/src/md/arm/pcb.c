/*
 * mach3/kernel/arm/pcb.c -- ARM pcb management and context switching.
 *
 * Kernel stacks are allocated with kmem_alloc (identity-mapped).
 * Context switch saves/restores callee-saved regs (r4-r11, sp, lr)
 * in the thread's pcb->kss (see context.s).
 */
#include <mach/machine/vm_types.h>
#include <mach/machine/vm_param.h>
#include <machine/thread.h>
#include <machine/pmap.h>
#include <machine/machspl.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <vm/vm_kern.h>

/* asm primitives (context.s) */
extern thread_t	__Switch_context(pcb_t old_pcb, continuation_t continuation,
				 pcb_t new_pcb, thread_t new_thread,
				 thread_t old_thread);
extern void	call_continuation(void (*continuation)(thread_t));
extern void	_load_context(pcb_t pcb, thread_t thread);

#define	stack_next(stack)	STACK_MSB(stack)->next

vm_offset_t	stack_free_list;
pcb_t		current_pcb;
unsigned int	stack_free_count = 0;
unsigned int	stack_free_limit = 1;	/* patchable */

void
pcb_init(thread_t thread)
{
	pcb_t pcb;

	pcb = (pcb_t)kalloc(sizeof(struct pcb));
	bzero((void *)pcb, sizeof(struct pcb));
	pcb->ast = 0;
	thread->pcb = pcb;
}

void
pcb_terminate(thread_t thread)
{
	if (thread->pcb) {
		kfree((vm_offset_t)thread->pcb, sizeof(struct pcb));
		thread->pcb = (pcb_t)0;
	}
}

void
pcb_collect(void)
{
}

/*
 * switch_context: C wrapper.  UP identity-mapped: no address-space
 * switch needed.  Delegates to the asm Switch_context.
 */
thread_t
switch_context(thread_t old, continuation_t continuation, thread_t new)
{
	pcb_t old_pcb = (old == THREAD_NULL) ? (pcb_t)0 : old->pcb;
	return __Switch_context(old_pcb, continuation, new->pcb,
				new, old);
}

/*
 * stack_attach: point the thread at a kernel stack and set up its
 * initial kernel state to start at `continuation`.
 */
void
stack_attach(thread_t thread, vm_offset_t stack, void (*continuation)(thread_t))
{
	struct arm_kernel_state *kss;

	thread->kernel_stack = stack;

	kss = &thread->pcb->kss;
	kss->r4 = 0;
	kss->r5 = 0;
	kss->r6 = 0;
	kss->r7 = 0;
	kss->r8 = 0;
	kss->r9 = 0;
	kss->r10 = 0;
	kss->r11 = 0;
	/* Leave room for the stack anchor; keep 8-byte aligned. */
	kss->sp = (unsigned int)(stack + KERNEL_STACK_SIZE - 16);
	kss->lr = (unsigned int)continuation;
}

/*
 * load_context: enter a thread for the first time (from
 * cpu_launch_first_thread).  Never returns.
 */
void
load_context(thread_t thread)
{
	_load_context(thread->pcb, thread);
	/* NOTREACHED */
	panic("load_context returned");
}

void
thread_bootstrap_return(void)
{
	panic("thread_bootstrap_return: no user tasks in M3");
}

/*
 * Kernel stack allocator (MACHINE_STACK).
 */
boolean_t
stack_alloc_try(thread_t thread, void (*continuation)(thread_t))
{
	vm_offset_t stack;
	spl_t s;

	s = splsched();
	stack = stack_free_list;
	if (stack != 0) {
		stack_free_list = stack_next(stack);
		stack_free_count--;
	}
	(void) splx(s);

	if (stack == 0)
		return FALSE;

	stack_attach(thread, stack, continuation);
	return TRUE;
}

void
stack_alloc(thread_t thread, void (*continuation)(thread_t))
{
	vm_offset_t stack;
	spl_t s;

	s = splsched();
	stack = stack_free_list;
	if (stack != 0) {
		stack_free_list = stack_next(stack);
		stack_free_count--;
	}
	(void) splx(s);

	if (stack == 0) {
		kern_return_t kr;
		kr = kmem_alloc(kernel_map, &stack, KERNEL_STACK_SIZE);
		if (kr != KERN_SUCCESS)
			panic("stack_alloc: kmem_alloc failed");
	}

	stack_attach(thread, stack, continuation);
}

void
stack_free(thread_t thread)
{
	vm_offset_t stack = thread->kernel_stack;
	spl_t s;

	thread->kernel_stack = 0;
	s = splsched();
	stack_next(stack) = stack_free_list;
	stack_free_list = stack;
	stack_free_count++;
	(void) splx(s);
}

vm_offset_t
user_stack_low(vm_size_t stack_size)
{
	/* M3: no user tasks; return dummy (bootstrap may call this). */
	return 0;
}

void
set_user_regs(vm_offset_t stack_base, vm_size_t stack_size,
	      int *entry, int arg_size)
{
	/* M3: no user tasks; panic if called. */
	panic("set_user_regs: no user tasks in M3");
}

void
thread_set_syscall_return(thread_t thread, kern_return_t retval)
{
	/* M3: no user tasks; no-op. */
}

void
pcb_module_init(void)
{
	/* M3: stacks come from kmem_alloc; nothing to pre-init. */
}

kern_return_t
thread_getstatus(thread_t thread, int flavor,
		 thread_state_t tstate, unsigned int *count)
{
	/* M3: minimal; report zeroed ARM_THREAD_STATE */
	if (flavor == 1 /* ARM_THREAD_STATE */ && *count >= 17) {
		unsigned int *s = (unsigned int *)tstate;
		int i;
		for (i = 0; i < 17; i++)
			s[i] = 0;
		*count = 17;
		return KERN_SUCCESS;
	}
	return KERN_INVALID_ARGUMENT;
}

kern_return_t
thread_setstatus(thread_t thread, int flavor,
		 thread_state_t tstate, unsigned int count)
{
	/* M3: accept and ignore (no user tasks yet) */
	return KERN_SUCCESS;
}

void
syscall_emulation_sync(task_t task)
{
	/* M3: no emulation vectors; no-op. */
}

/*
 * stack_handoff: switch to new thread's stack, possibly changing tasks.
 * UP identity-mapped: no address space switch needed in M3.
 * Modelled on mips stack_handoff (same task -> Switch_context path).
 */
void
stack_handoff(thread_t old_thread, thread_t new_thread)
{
	pcb_t old_pcb = old_thread->pcb;
	pcb_t new_pcb = new_thread->pcb;

	current_pcb = new_pcb;

	/* Same "task" in M3 boot (all kernel threads); use Switch_context. */
	__Switch_context(old_pcb, (continuation_t)0, new_pcb,
			 new_thread, old_thread);
}

/*
 * stack_collect: free excess kernel stacks (MACHINE_STACK).
 */
void
stack_collect(void)
{
	vm_offset_t stack;
	spl_t s;

	s = splsched();
	while (stack_free_count > stack_free_limit) {
		stack = stack_free_list;
		stack_free_list = stack_next(stack);
		stack_free_count--;
		(void) splx(s);

		kmem_free(kernel_map, stack, KERNEL_STACK_SIZE);

		s = splsched();
	}
	(void) splx(s);
}
