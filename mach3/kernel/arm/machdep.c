/*
 * mach3/kernel/arm/machdep.c -- ARM machine-dependent misc routines.
 */
#include <mach/machine/vm_types.h>
#include <machine/machine_routines.h>
#include <machine/machspl.h>

/* from uart.c */
extern void	uart_putc(char c);

/* MI */
extern int	cold;

unsigned int
htonl(unsigned int x)
{
	return ((x & 0xffu) << 24) | ((x & 0xff00u) << 8) |
	       ((x & 0xff0000u) >> 8) | ((x & 0xff000000u) >> 24);
}

unsigned int
ntohl(unsigned int x)
{
	return htonl(x);
}

unsigned short
htons(unsigned short x)
{
	return (unsigned short)(((x & 0xffu) << 8) | ((x & 0xff00u) >> 8));
}

unsigned short
ntohs(unsigned short x)
{
	return htons(x);
}
extern void	panic(const char *, ...);
extern void	printf(const char *, ...);

void
machine_init(void)
{
	extern void	fpu_init(void);
	extern void	pmap_selftest(void);
	extern void	ipc_selftest(void);
	extern void	blk_selftest(void);

	fpu_init();
	pmap_selftest();	/* M4: verify L2 small-page path */
	ipc_selftest();		/* M4: verify IPC ports/port sets */
	blk_selftest();		/* M4: verify virtio-blk read/write */
	cold = 0;
}

void
halt_cpu(void)
{
	(void) splhigh();
	printf("halting cpu\n");
	for (;;)
		__asm__ volatile ("wfi");
}

void
halt_all_cpus(void)
{
	halt_cpu();
}
