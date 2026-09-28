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
#include <machine/machspl.h>
#include <machine/machine_routines.h>

struct arm_trap_frame {
	unsigned int	r[13];	/* r0-r12 */
	unsigned int	lr;	/* adjusted return address */
	unsigned int	spsr;
};

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

static const char *const trap_names[] = {
	"undefined instruction",
	"supervisor call",
	"prefetch abort",
	"data abort",
	"irq",
	"fiq",
};

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
			clock_interrupt(1000000 / 100, usermode, FALSE);
			arm_timer_eoi();
			timer_ticks++;	/* M4: verification counter */
		} else if (irq != 1023) {
			printf("arm_trap: unexpected irq %u\n", irq);
		}
		gic_eoi(irq);
		return;
	}

	case TRAP_DABT:
		panic("data abort: pc=0x%x dfar=0x%x dfsr=0x%x",
		      frame->lr, read_dfar(), read_dfsr());

	case TRAP_PABT:
		panic("prefetch abort: pc=0x%x ifsr=0x%x",
		      frame->lr, read_ifsr());

	case TRAP_UNDEF:
		panic("undefined instruction at pc=0x%x", frame->lr);

	case TRAP_SVC:
		panic("unexpected svc from %s mode (pc=0x%x)",
		      ((frame->spsr & 0x1fu) == 0x10u) ? "user" : "svc",
		      frame->lr);

	case TRAP_FIQ:
		panic("unexpected fiq");

	default:
		panic("unknown trap %d", trapno);
	}
}

/*
 * thread_exception_return / thread_syscall_return / thread_kdb_return:
 * return to user mode.  M3 has no user tasks yet; panic if reached.
 */
void
thread_exception_return(void)
{
	panic("thread_exception_return: no user tasks in M3");
}

void
thread_syscall_return(void)
{
	panic("thread_syscall_return: no user tasks in M3");
}

void
thread_kdb_return(void)
{
	panic("thread_kdb_return: no user tasks in M3");
}
