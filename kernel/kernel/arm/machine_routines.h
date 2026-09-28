/*
 * mach3/kernel/arm/machine_routines.h -- ARM MD routine declarations.
 */
#ifndef	_MACHINE_MACHINE_ROUTINES_H_
#define	_MACHINE_MACHINE_ROUTINES_H_

#include <mach/machine/vm_types.h>

extern void	arm_init(void);
extern void	machine_startup(void);
extern void	machine_init(void);
extern void	Debugger(const char *);
extern void	halt_cpu(void);
extern void	halt_all_cpus(void);
extern void	cnputc(char c, vm_offset_t arg);
extern int	cngetc(void);
extern void	pmap_bootstrap(void);
extern void	startrtclock(void);
extern void	inittodr(void);
extern void	resettodr(void);
extern void	microtime(struct timeval *tv);
extern void	delay(int usec);

#endif	/* _MACHINE_MACHINE_ROUTINES_H_ */
