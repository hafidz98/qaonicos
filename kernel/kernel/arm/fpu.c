/*
 * mach3/kernel/arm/fpu.c -- ARM VFP init.
 *
 * M3: enable VFPv3/4 coprocessor access; no per-thread state yet.
 */
#include <mach/machine/vm_types.h>

void
fpu_init(void)
{
	unsigned int v;

	/* Enable CP10/CP11 (VFP) in CPACR */
	__asm__ volatile ("mrc p15, 0, %0, c1, c0, 2" : "=r" (v));
	v |= (0xf << 20);
	__asm__ volatile ("mcr p15, 0, %0, c1, c0, 2" : : "r" (v));
	__asm__ volatile ("isb");

	/* Enable VFP in FPEXC */
	__asm__ volatile (
		"mrc p10, 7, %0, c8, c0, 0\n"
		"orr %0, %0, #(1<<30)\n"
		"mcr p10, 7, %0, c8, c0, 0"
		: "=&r" (v) : : "memory");
}

void
fpu_save(void *save_area)
{
}

void
fpu_restore(void *save_area)
{
}
