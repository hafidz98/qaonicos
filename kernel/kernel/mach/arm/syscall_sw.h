/* mach3/kernel/mach/arm/syscall_sw.h -- ARM software trap table.
 * For M3 boot (no userspace yet) only the macro shapes matter;
 * real user syscall stubs come with the userspace bring-up. */
#ifndef	_MACH_ARM_SYSCALL_SW_H_
#define	_MACH_ARM_SYSCALL_SW_H_	1
#define kernel_trap_0(trap_name,trap_number)
#define kernel_trap_1(trap_name,trap_number)
#define kernel_trap_2(trap_name,trap_number)
#define kernel_trap_3(trap_name,trap_number)
#define kernel_trap_4(trap_name,trap_number)
#define kernel_trap_5(trap_name,trap_number)
#define kernel_trap_6(trap_name,trap_number)
#define kernel_trap_7(trap_name,trap_number)
#define kernel_trap_8(trap_name,trap_number)
#define kernel_trap_9(trap_name,trap_number)
#endif	/* _MACH_ARM_SYSCALL_SW_H_ */
