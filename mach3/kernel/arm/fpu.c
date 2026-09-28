/*
 * mach3/kernel/arm/fpu.c -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - Save/restore VFPv4 per thread (fpu_save/fpu_restore).
 *
 * Adaptasi dari: rv1103-bringup/fpu.c (sudah ada, 1:1)
 */
