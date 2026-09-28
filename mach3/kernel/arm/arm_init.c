/*
 * mach3/kernel/arm/arm_init.c -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - machine_startup(): parse boot info, pmap_bootstrap, cpu_startup, buka console.
 *
 * Adaptasi dari: rv1103-bringup/main.c + kernel_main.c (alur boot)
 */
