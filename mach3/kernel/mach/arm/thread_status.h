/*
 * mach3/kernel/mach/arm/thread_status.h -- ARM thread state flavors.
 * Modelled on CMU Mach 3.0 mach/mips/thread_status.h.
 */
#ifndef	_MACH_ARM_THREAD_STATUS_H_
#define	_MACH_ARM_THREAD_STATUS_H_

#include <mach/machine/vm_types.h>

#define	ARM_THREAD_STATE	(1)
#define	ARM_VFP_STATE		(2)
#define	ARM_EXC_STATE		(3)

struct arm_thread_state {
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
	unsigned int	r11;	/* fp */
	unsigned int	r12;	/* ip */
	unsigned int	sp;
	unsigned int	lr;
	unsigned int	pc;	/* user-mode PC */
	unsigned int	cpsr;
};
#define	ARM_THREAD_STATE_COUNT \
	(sizeof(struct arm_thread_state)/sizeof(unsigned int))

/* VFPv3/v4: 32 double registers + fpscr */
struct arm_vfp_state {
	unsigned long long	fpregs[32];
	unsigned int		fpscr;
	unsigned int		fpexc;
};
#define	ARM_VFP_STATE_COUNT \
	(sizeof(struct arm_vfp_state)/sizeof(unsigned int))

struct arm_exc_state {
	unsigned int	trapno;
	unsigned int	err;
	unsigned int	fault_addr;
};

#endif	/* _MACH_ARM_THREAD_STATUS_H_ */
