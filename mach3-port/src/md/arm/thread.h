/*
 * mach3-port/src/md/arm/thread.h -- ARM machine-dependent thread state.
 * Modelled on CMU Mach 3.0 mips/thread.h.
 */
#ifndef	_ARM_THREAD_H_
#define	_ARM_THREAD_H_

#if	!defined(ASSEMBLER)

/* Callee-saved state for kernel thread switch (Switch_context). */
struct arm_kernel_state {
	unsigned int	r4;
	unsigned int	r5;
	unsigned int	r6;
	unsigned int	r7;
	unsigned int	r8;
	unsigned int	r9;
	unsigned int	r10;
	unsigned int	r11;	/* fp */
	unsigned int	sp;
	unsigned int	lr;	/* return address (resumption pc) */
};

/* Full saved register state on kernel entry (trap frame). */
struct arm_saved_state {
	unsigned int	r0;
	unsigned int	r1;
	unsigned int	r2;
	unsigned int	r3;
	unsigned int	r4;
	unsigned int	r5;
	unsigned int	r6;
	unsigned int	r7;
	unsigned int	r8;
	unsigned int	r9;
	unsigned int	r10;
	unsigned int	r11;
	unsigned int	r12;
	unsigned int	sp;	/* banked sp of interrupted mode */
	unsigned int	lr;	/* banked lr of interrupted mode */
	unsigned int	pc;
	unsigned int	cpsr;
	unsigned int	trapno;
	unsigned int	fault_addr;
	unsigned int	fault_status;
};

/*
 * Per-thread VFP/NEON state (K1/THR-1).  Cortex-A7 VFPv4-D32: 32
 * double registers d0-d31 (aliased s0-s31 / q0-q15) + FPSCR.
 * FPEXC stays globally enabled by fpu_init(); it is not per-thread
 * state.  The 264-byte area lives in the pcb (mms) so the switch
 * path can save/restore it keyed on thread identity (the pcb),
 * never on the preempting frame's mode (Fase 10 lesson).
 * Offset of mms in struct pcb is 120 (iss 80 + kss 40);
 * context.s hardcodes PCB_FPU 120 and pcb.c asserts it.
 */
struct arm_fpu_state {
	unsigned int	d[64];	/* d0-d31 as 64 words */
	unsigned int	fpscr;
	unsigned int	pad;	/* align size to 8 */
};

struct arm_machine_state {
	struct arm_fpu_state	fpu;
};

typedef struct pcb {
	struct arm_saved_state	iss;	/* "interrupt" saved state */
	struct arm_kernel_state	kss;	/* kernel switch state */
	struct arm_machine_state	mms;
	int			ast;	/* pending AST */
} *pcb_t;	/* exported */

#define	USER_REGS(th)	(&(th)->pcb->iss)

/*
 * Kernel stack layout: stack grows down from (stack + KERNEL_STACK_SIZE).
 * We keep a small anchor at the top for the pcb pointer.
 */
struct arm_stack_base {
	vm_offset_t	next;	/* next stack on free list */
	struct vm_page	*page;	/* page structure for this stack */
};

#define	STACK_MSB(stack) \
	((struct arm_stack_base *)((stack) + KERNEL_STACK_SIZE) - 1)

/* We have our own ARM implementations of stack_alloc_try/stack_alloc/...
 * (in pcb.c), like the mips port. */
#define	MACHINE_STACK

/*
 * Routine definitions
 */
#include <mach/kern_return.h>

void		pcb_init(), pcb_terminate(), pcb_collect();
kern_return_t	thread_setstatus(), thread_getstatus();

#endif	/* !defined(ASSEMBLER) */

#ifdef	ASSEMBLER
#define	the_current_thread	active_threads
#endif	/* ASSEMBLER */

#endif	/* _ARM_THREAD_H_ */
