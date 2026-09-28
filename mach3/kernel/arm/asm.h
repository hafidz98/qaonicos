/*
 * mach3/kernel/arm/asm.h -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * Header machine-dependent, di-include kernel MI via <machine/asm.h>.
 * Dimodelkan dari Prajna/mach kernel/mips/asm.h.
 *
 * WAJIB disediakan:
 * - Makro assembler: ENTRY/LEAF/END, eksport simbol ke C.
 *
 * Adaptasi dari: rv1103-bringup/vectors.S, start.S (konvensi label)
 */
