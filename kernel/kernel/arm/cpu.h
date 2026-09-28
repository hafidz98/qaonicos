/*
 * mach3/kernel/arm/cpu.h -- ARM cpu data (UP).
 */
#ifndef	_MACHINE_CPU_H_
#define	_MACHINE_CPU_H_

#ifndef	ASSEMBLER
struct cpu_data {
	int	cpu_number;
};
extern struct cpu_data	cpu_data[];
#define	cpu_number()	(0)
#endif	/* ASSEMBLER */

#endif	/* _MACHINE_CPU_H_ */
