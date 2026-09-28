/*
 * mach3/kernel/arm/machspl.h -- ARM interrupt priority levels.
 * UP ARMv7: all spl levels map to IRQ+FIQ disable; splx restores CPSR.
 */
#ifndef	_MACHINE_MACHSPL_H_
#define	_MACHINE_MACHSPL_H_

typedef unsigned int	spl_t;

static __inline spl_t
_arm_get_cpsr(void)
{
	unsigned int cpsr;
	__asm__ volatile ("mrs %0, cpsr" : "=r" (cpsr));
	return cpsr;
}

static __inline void
_arm_set_cpsr_c(unsigned int cpsr)
{
	__asm__ volatile ("msr cpsr_c, %0" : : "r" (cpsr) : "memory");
}

/* splhigh: disable IRQ and FIQ, return old CPSR */
static __inline spl_t
splhigh(void)
{
	spl_t old = _arm_get_cpsr();
	_arm_set_cpsr_c(old | 0xC0);
	return old;
}

/* spl0: enable IRQ and FIQ, return old CPSR */
static __inline spl_t
spl0(void)
{
	spl_t old = _arm_get_cpsr();
	_arm_set_cpsr_c(old & ~0xC0);
	return old;
}

static __inline void
splx(spl_t s)
{
	/* restore I/F bits from saved CPSR */
	spl_t cur = _arm_get_cpsr();
	_arm_set_cpsr_c((cur & ~0xC0) | (s & 0xC0));
}

#define	splsoftclock()	splhigh()
#define	splnet()	splhigh()
#define	splimp()	splhigh()
#define	splbio()	splhigh()
#define	spltty()	splhigh()
#define	splclock()	splhigh()
#define	splvm()		splhigh()
#define	splsched()	splhigh()

#endif	/* _MACHINE_MACHSPL_H_ */
