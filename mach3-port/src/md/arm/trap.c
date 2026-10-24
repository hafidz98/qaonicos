/*
 * mach3-port/src/md/arm/trap.c -- ARM exception dispatch.
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
/* user.c (FND-3): health supervisor tick check */
extern void		wd_irq_tick(unsigned int usermode);
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
			/* FND-3: class (b) starvation check -- if
			 * the cooperative scheduler has not run
			 * for 30 s while a user-mode frame holds
			 * the CPU, the runner never yields and
			 * only this IRQ path can still act
			 * (watchdog mark + reset, in user.c). */
			wd_irq_tick(usermode ? 1u : 0u);
			/* M5 Phase 4: scheduler demo runs from startrtclock
			 * (thread context), not from IRQ.  Timer just ticks. */
			gic_eoi(irq);
		} else if (irq != 1023) {
			printf("arm_trap: unexpected irq %u\n", irq);
			gic_eoi(irq);
		}
		/* Fase C: jangan EOI untuk spurious (1023).  Menulis
		 * EOIR=1023 saat tidak ada interupsi aktif adalah
		 * UNPREDICTABLE pada GICv2 (implementasi boleh
		 * mengabaikan, tapi jangan andalkan). */
		return;
	}

	case TRAP_DABT:
		if ((frame->spsr & 0x1fu) == ARM_MODE_USR) {
			/* M4: user fault kills just the user context. */
			user_fault(frame, USER_EXIT_DABT);
			break;
		}
		/* #7: dump registers untuk debug. */
		extern unsigned int dbg_cur_syscall;
		printf("data abort: pc=0x%x dfar=0x%x dfsr=0x%x syscall=%u\n",
		    frame->lr, read_dfar(), read_dfsr(), dbg_cur_syscall);
		printf("  r0=%x r1=%x r2=%x r3=%x\n", frame->r[0],
		    frame->r[1], frame->r[2], frame->r[3]);
		printf("  r4=%x r5=%x r6=%x r7=%x\n", frame->r[4],
		    frame->r[5], frame->r[6], frame->r[7]);
		printf("  r8=%x r9=%x r10=%x r11=%x\n", frame->r[8],
		    frame->r[9], frame->r[10], frame->r[11]);
		printf("  r12=%x svc_lr=%x spsr=%x\n", frame->r[12],
		    frame->svc_lr, frame->spsr);
		{ extern void bootstate_panic_note(void);
		  bootstate_panic_note(); }
		panic("data abort halted");

	case TRAP_PABT:
		if ((frame->spsr & 0x1fu) == ARM_MODE_USR) {
			user_fault(frame, USER_EXIT_PABT);
			break;
		}
		{ extern void bootstate_panic_note(void);
		  bootstate_panic_note(); }
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
