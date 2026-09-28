/*
 * mach3/kernel/arm/lock.h -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * Header machine-dependent, di-include kernel MI via <machine/lock.h>.
 * Dimodelkan dari Prajna/mach kernel/mips/lock.h.
 *
 * WAJIB disediakan:
 * - Implementasi simple_lock ARM (ldrex/strex atau irq-mask unipro).
 *
 * Adaptasi dari: rv1103-bringup/irq.c (pola mask IRQ)
 */
