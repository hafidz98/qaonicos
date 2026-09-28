/*
 * mach3/kernel/arm/uart.c -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - Console: cnputc/cngetc via PL011 (QEMU virt) / DW (RV1103).
 *
 * Adaptasi dari: rv1103-bringup/uart.c (sudah ada, 1:1)
 */
