/*
 * pmap.h - ARMv7-A short-descriptor page map for the Mach port.
 *
 * Minimal, from-scratch MMU layer used during bring-up on QEMU virt
 * (Cortex-A7) and the Rockchip RV1103 / Luckfox Pico Mini. It maps
 * memory using 1 MiB section descriptors in a single L1 translation
 * table (TTBR0), which is enough to turn on the MMU with identity
 * mappings before a real Mach pmap exists.
 *
 * Build assumptions: C99, -ffreestanding, no libc,
 * -mcpu=cortex-a7 -marm.
 */
#ifndef _ARMV7_PMAP_H_
#define _ARMV7_PMAP_H_

#include <stdint.h>

/*
 * Memory attributes for pmap_map_section().
 *
 * The values are positioned so they can be OR-ed straight into an
 * ARMv7 short-descriptor section entry. Only the TEX/C/B/XN attribute
 * bits belong to these flags; the descriptor type, domain and access
 * permission bits are supplied by pmap.c.
 *
 *   PMAP_CACHEABLE : Normal memory, TEX=0b001, C=1, B=1
 *                    (Outer and Inner Write-Back, Write-Allocate).
 *   PMAP_DEVICE    : Device/strongly-ordered, TEX=0, C=0, B=0, XN=1
 *                    (execute-never, as required for MMIO).
 */
#define PMAP_CACHEABLE  0x0000100Cu     /* TEX[0] | C | B              */
#define PMAP_DEVICE     0x00000010u     /* TEX=000, C=0, B=0, XN       */

/* Bring up the L1 table: clear it and install the default identity
 * mappings (DRAM cacheable, peripheral space device). No MMU change. */
void pmap_init(void);

/* Install a 1 MiB section mapping. va and pa must be 1 MiB aligned.
 * flags is PMAP_CACHEABLE or PMAP_DEVICE. */
void pmap_map_section(uint32_t va, uint32_t pa, int flags);

/* Remove the 1 MiB section mapping covering va. */
void pmap_unmap_section(uint32_t va);

/* Install TTBR0/DACR and set SCTLR.M, turning the MMU on. */
void pmap_enable(void);

/* Clear SCTLR.M, turning the MMU off. */
void pmap_disable(void);

#endif /* _ARMV7_PMAP_H_ */