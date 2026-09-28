/*
 * mach3/kernel/arm/gic.c -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - Driver GIC-400: init, enable/disable IRQ, EOI.
 *
 * Adaptasi dari: rv1103-bringup/gic.c + irq.c (sudah ada, 1:1)
 */
