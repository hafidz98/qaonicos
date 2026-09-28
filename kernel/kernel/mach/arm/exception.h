/* mach3/kernel/mach/arm/exception.h -- ARM exception codes. */
#ifndef	_MACH_ARM_EXCEPTION_H_
#define	_MACH_ARM_EXCEPTION_H_
#define	EXC_ARM_UNDEF		0	/* undefined instruction */
#define	EXC_ARM_SVC		1	/* supervisor call */
#define	EXC_ARM_PABORT		2	/* prefetch abort */
#define	EXC_ARM_DABORT		3	/* data abort */
#define	EXC_ARM_IRQ		4	/* interrupt */
#define	EXC_ARM_FIQ		5	/* fast interrupt */
#define	EXC_ARM_SOFT_SEGV	16	/* software detected seg viol */
#define	EXC_ARM_PRIVINST	1
#define	EXC_ARM_RESOPND		2
#define	EXC_ARM_RESADDR		3
#define	EXC_ARM_BPT		1
#define	EXC_ARM_TRACE		2
#endif	/* _MACH_ARM_EXCEPTION_H_ */
