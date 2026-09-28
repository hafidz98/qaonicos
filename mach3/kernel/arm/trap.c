/*
 * mach3/kernel/arm/trap.c -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - Dispatch trap: trap(), syscall_entry, page_fault handler -> vm_fault.
 *
 * Adaptasi dari: rv1103-bringup/trap.c
 */
