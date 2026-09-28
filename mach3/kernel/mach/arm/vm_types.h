/*
 * mach3/kernel/mach/arm/vm_types.h -- ARM machine-dependent VM types.
 * Modelled on CMU Mach 3.0 mach/mips/vm_types.h.
 */
#ifndef	_MACH_ARM_VM_TYPES_H_
#define	_MACH_ARM_VM_TYPES_H_	1

#ifdef	ASSEMBLER
#else	/* ASSEMBLER */
typedef unsigned int	natural_t;
typedef int		integer_t;
typedef int		int32;
typedef unsigned int	uint32;
typedef	natural_t	vm_offset_t;
typedef	natural_t	vm_size_t;
#endif	/* ASSEMBLER */

#define	MACH_MSG_TYPE_INTEGER_T	MACH_MSG_TYPE_INTEGER_32

#endif	/* _MACH_ARM_VM_TYPES_H_ */
