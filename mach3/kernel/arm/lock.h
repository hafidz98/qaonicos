/*
 * mach3/kernel/arm/lock.h -- ARM simple locks.
 * NOTE: with NCPUS==1 && !MACH_LDEBUG, MACH_SLOCKS==0 and kern/lock.h
 * does not use this file at all.  Provided for completeness.
 */
#ifndef	_MACHINE_LOCK_H_
#define	_MACHINE_LOCK_H_

#ifndef	ASSEMBLER
typedef volatile int	hw_lock_t;
typedef hw_lock_t	* hw_lock_p_t;
#define	HW_LOCK_NULL	((hw_lock_t *)0)

static __inline void
hw_lock_init(hw_lock_t *lock)
{
	*lock = 0;
}

static __inline void
hw_lock_lock(hw_lock_t *lock)
{
	int tmp;
	__asm__ volatile (
		"1: ldrex %0, [%1]\n"
		"   teq   %0, #0\n"
		"   strexeq %0, %2, [%1]\n"
		"   teq   %0, #0\n"
		"   bne   1b"
		: "=&r" (tmp) : "r" (lock), "r" (1) : "memory", "cc");
}

static __inline void
hw_lock_unlock(hw_lock_t *lock)
{
	__asm__ volatile ("" ::: "memory");
	*lock = 0;
}

static __inline boolean_t
hw_lock_try(hw_lock_t *lock)
{
	int tmp, res;
	__asm__ volatile (
		"   ldrex %0, [%2]\n"
		"   teq   %0, #0\n"
		"   strexeq %1, %3, [%2]\n"
		: "=&r" (tmp), "=&r" (res) : "r" (lock), "r" (1)
		: "memory", "cc");
	return (res == 0);
}
#endif	/* ASSEMBLER */

#endif	/* _MACHINE_LOCK_H_ */
