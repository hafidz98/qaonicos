/*
 * mach3/kernel/arm/trap.c -- ARM exception dispatch.
 *
 * Trap frame is built by locore.s on the SVC stack:
 *
 *   struct arm_trap_frame { r0..r12, lr (return addr), spsr };
 */
#include <mach/machine/vm_types.h>
#include <mach/boolean.h>
#include <mach/machine/vm_param.h>
#include <machine/trap_frame.h>
#include <machine/machspl.h>
#include <machine/machine_routines.h>
#include <kern/cpu_number.h>	/* M5: cpu_number */

#define	TRAP_UNDEF	0
#define	TRAP_SVC	1
#define	TRAP_PABT	2
#define	TRAP_DABT	3
#define	TRAP_IRQ	4
#define	TRAP_FIQ	5

#define	ARM_TIMER_PPI	27	/* virtual timer PPI (CNTV) */

/* from gic.c */
extern unsigned int	gic_get_irq(void);
extern void		gic_eoi(unsigned int);
/* from clock.c */
extern void		arm_timer_eoi(void);
/* MI */
extern void		clock_interrupt(int, boolean_t, boolean_t);
extern void		panic(const char *, ...);
/* M5 Phase 4: scheduler test tick */
extern void		sched_test_tick(void);

static const char *const trap_names[] = {
	"undefined instruction",
	"supervisor call",
	"prefetch abort",
	"data abort",
	"irq",
	"fiq",
};

/* user.c (M4 item 4) */
extern unsigned int	user_syscall(struct arm_trap_frame *frame);
extern void		user_fault(struct arm_trap_frame *frame,
				   unsigned int code);
#define	USER_EXIT_DABT	0xDAB7u
#define	USER_EXIT_PABT	0x9AB7u
#define	USER_EXIT_UNDEF	0x5EEDu

static unsigned int
read_dfsr(void)
{
	unsigned int v;
	__asm__ volatile ("mrc p15, 0, %0, c5, c0, 0" : "=r" (v));
	return v;
}

static unsigned int
read_dfar(void)
{
	unsigned int v;
	__asm__ volatile ("mrc p15, 0, %0, c6, c0, 0" : "=r" (v));
	return v;
}

static unsigned int
read_ifsr(void)
{
	unsigned int v;
	__asm__ volatile ("mrc p15, 0, %0, c5, c0, 1" : "=r" (v));
	return v;
}

static unsigned int	timer_ticks;	/* M4: verification counter */

/* Fase B: user-visible tick counter for SYS_YIELD. */
unsigned int
arm_timer_ticks(void)
{
	return timer_ticks;
}

/*
 * arm_trap_handler: C entry for all exceptions (from locore.s).
 */
void
arm_trap_handler(struct arm_trap_frame *frame, int trapno)
{
	switch (trapno) {
	case TRAP_IRQ: {
		unsigned int irq = gic_get_irq();
		boolean_t usermode;
		if (irq == ARM_TIMER_PPI) {
			usermode = ((frame->spsr & 0x1fu) == 0x10u);
			(void)usermode;
			/* Fase B: minimal tick only.  Do NOT call MI
			 * clock_interrupt(): its quantum update drives
			 * the MI scheduler (thread_select), but this
			 * port's threads are managed manually (M6
			 * cooperative switch_context; MI scheduler
			 * deferred).  With empty MI run queues,
			 * thread_select dereferences NULL -> panic.
			 * The timer just ticks; timer_ticks drives
			 * SYS_YIELD. */
			arm_timer_eoi();
			timer_ticks++;	/* M4: verification counter */
			/* M5 Phase 4: scheduler demo runs from startrtclock
			 * (thread context), not from IRQ.  Timer just ticks. */
		} else if (irq != 1023) {
			printf("arm_trap: unexpected irq %u\n", irq);
		}
		gic_eoi(irq);
		return;
	}

	case TRAP_DABT:
		if ((frame->spsr & 0x1fu) == ARM_MODE_USR) {
			/* M4: user fault kills just the user context. */
			user_fault(frame, USER_EXIT_DABT);
			break;
		}
		panic("data abort: pc=0x%x dfar=0x%x dfsr=0x%x",
		      frame->lr, read_dfar(), read_dfsr());

	case TRAP_PABT:
		if ((frame->spsr & 0x1fu) == ARM_MODE_USR) {
			user_fault(frame, USER_EXIT_PABT);
			break;
		}
		panic("prefetch abort: pc=0x%x ifsr=0x%x",
		      frame->lr, read_ifsr());

	case TRAP_UNDEF:
		if ((frame->spsr & 0x1fu) == ARM_MODE_USR) {
			user_fault(frame, USER_EXIT_UNDEF);
			break;
		}
		panic("undefined instruction at pc=0x%x", frame->lr);

	case TRAP_SVC: {
		unsigned int mode = frame->spsr & 0x1fu;
		if (mode != ARM_MODE_USR)
			panic("unexpected svc from svc mode (pc=0x%x)",
			      frame->lr);
		/* M4: user syscall; number in r7, return value -> r0.
		 * user_syscall may redirect the frame (SYS_EXIT). */
		frame->r[0] = user_syscall(frame);
		break;
	}

	case TRAP_FIQ:
		panic("unexpected fiq");

	default:
		panic("unknown trap %d", trapno);
	}
}

/*
 * thread_exception_return: return to user mode after trap.
 * M6: Check for pending ASTs (e.g., preemption) before returning.
 * Called from trap handler when returning to user mode.
 */
void
thread_exception_return(void)
{
	/* M6: AST check point for user trap return.
	 * Full preemption via ast_taken() requires per-thread trap
	 * frames (future work). For now, just return; the trap
	 * handler's assembly restores the frame. */
}

/* MI expects noreturn; not used by M4 custom syscalls. */
void
thread_syscall_return(int kr)
{
	(void)kr;
	panic("thread_syscall_return: not implemented for ARM");
}

void
thread_kdb_return(void)
{
	panic("thread_kdb_return: no user tasks in M3");
}
