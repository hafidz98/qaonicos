/*
 * mach3/kernel/arm/machspl.h -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * Header machine-dependent, di-include kernel MI via <machine/machspl.h>.
 * Dimodelkan dari Prajna/mach kernel/mips/machspl.h.
 *
 * WAJIB disediakan:
 * - Level SPL + makro splhigh/splsched/spl0 untuk ARM (basis CPSR I-bit).
 *
 * Adaptasi dari: rv1103-bringup/irq.c
 */
