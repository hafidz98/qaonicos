/*
 * mach3-port/src/md/arm/fpu.c -- ARM VFP init.
 *
 * M3: enable VFPv3/4 coprocessor access.  K1: per-thread
 * save/restore lives in ONE place, context.s's __Switch_context
 * (save old / restore new, keyed on the pcb) and _load_context
 * (restore on first entry) -- not here.  There are deliberately
 * no fpu_save/fpu_restore C wrappers: a second copy of the
 * sequence would drift from the switch path, and no caller
 * needs one (all switches funnel through __Switch_context).
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

/* No fpu_save/fpu_restore here by design (see header comment). */
