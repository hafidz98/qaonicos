/*
 * mach3/kernel/arm/arm_init.c -- ARM machine-dependent initialization.
 *
 * Entry from locore.s (_reset -> arm_init).  Order mirrors mips_init():
 * console, memory sizing, pmap_bootstrap, machine_startup.
 */
#include <mach/machine/vm_types.h>
#include <mach/machine/vm_param.h>
#include <machine/machspl.h>
#include <machine/cpu.h>
#include <machine/machine_routines.h>

/* Symbols from the linker script. */
extern char _bss_start[], _bss_end[], _end[];

/*
 * MD globals (cf. mips_init.c).
 */
vm_offset_t	mem_size;		/* total RAM, bytes */
vm_offset_t	avail_start;		/* first free physical page */
vm_offset_t	avail_end;		/* last free physical page + 1 */
vm_offset_t	virtual_avail;		/* (unused: identity map) */
vm_offset_t	virtual_end;		/* (unused: identity map) */
int		boothowto = 0;		/* boot flags */
int		cold = 1;		/* cold boot indicator */
int		master_cpu = 0;
struct cpu_data	cpu_data[1];

char		version[] = "Mach 3.0 (ARMv7 port, QEMU virt)\n";

void		uart_init(void);
void		pmap_bootstrap(void);
void		setup_main(void);	/* MI: kern/startup.c */

/*
 * arm_init: called from locore.s with MMU on, BSS clear, SVC stack set.
 */
void
arm_init(void)
{
	uart_init();

	/* Physical memory: 64 MB at 0x40000000 (QEMU -M virt -m 64). */
	mem_size = (vm_offset_t)0x04000000;

	/* Free pages start after the kernel image (rounded up). */
	avail_start = (vm_offset_t)arm_round_page(_end);
	avail_end = (vm_offset_t)0x44000000;

	pmap_bootstrap();

	machine_startup();
	/* NOTREACHED */
}

void
machine_startup(void)
{
	printf("%s", version);
	setup_main();
	/* NOTREACHED */
}
