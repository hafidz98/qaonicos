/*
 * mach3/kernel/arm/pmap.h -- kerangka M1 (port Mach 3 asli ke ARMv7).
 *
 * Header machine-dependent, di-include kernel MI via <machine/pmap.h>.
 * Dimodelkan dari Prajna/mach kernel/mips/pmap.h.
 *
 * WAJIB disediakan:
 * - API pmap: pmap_t, kernel_pmap/active_pmap, pmap_pte(), pmap_protect/enter/remove, statistik.
 *
 * Adaptasi dari: rv1103-bringup/pmap.h (struktur sudah mirip!)
 */
