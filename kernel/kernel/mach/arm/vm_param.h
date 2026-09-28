/*
 * mach3/kernel/mach/arm/vm_param.h -- ARM machine-dependent VM parameters
 * (public interface).  QEMU -M virt: RAM 0x40000000-0x44000000 (64MB),
 * kernel linked at 0x40000000, identity-mapped.
 */
#ifndef	_MACH_ARM_VM_PARAM_H_
#define	_MACH_ARM_VM_PARAM_H_

#define BYTE_SIZE	8	/* byte size in bits */

#define ARM_PGBYTES	4096	/* bytes per ARM page */
#define ARM_PGSHIFT	12	/* number of bits to shift for pages */

#define arm_btop(x)		(((unsigned)(x)) >> ARM_PGSHIFT)
#define arm_ptob(x)		(((unsigned)(x)) << ARM_PGSHIFT)
#define arm_round_page(x)	((((unsigned)(x)) + ARM_PGBYTES - 1) & \
					~(ARM_PGBYTES-1))
#define arm_trunc_page(x)	(((unsigned)(x)) & ~(ARM_PGBYTES-1))

/* user address space: everything below RAM */
#define VM_MIN_ADDRESS	((vm_offset_t) 0x00010000)
#define VM_MAX_ADDRESS	((vm_offset_t) 0x40000000)

/* kernel address space: identity-mapped RAM */
#define ARM_KERNEL_SPACE_SIZE	((vm_size_t) 0x04000000)	/* 64 MB */
#define VM_MIN_KERNEL_ADDRESS	((vm_offset_t) 0x40000000)
#define VM_MAX_KERNEL_ADDRESS	((vm_offset_t) (VM_MIN_KERNEL_ADDRESS + \
						ARM_KERNEL_SPACE_SIZE))

#define KERNEL_STACK_SIZE	8192	/* 2 pages */
#define INTSTACK_SIZE		8192	/* only used for bootstrap */

#endif	/* _MACH_ARM_VM_PARAM_H_ */
