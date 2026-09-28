/*
 * mach3/kernel/arm/pcb.c -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * File machine-dependent, dimodelkan dari Prajna/mach kernel/mips/.
 *
 * WAJIB disediakan:
 * - PCB: pcb_init, switch stacks, save/restore konteks.
 *
 * Adaptasi dari: rv1103-bringup/pcb.c + switch.S
 */
