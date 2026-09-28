/*
 * mach3/kernel/arm/locore.s -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - Vektor exception ARMv7 (reset/undef/svc/pabt/dabt/irq/fiq), _start, trampoline ke C.
 *
 * Adaptasi dari: rv1103-bringup/vectors.S + start.S
 */
