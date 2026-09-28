/*
 * vm_param.h - Virtual memory parameters for the Mach ARMv7 port.
 *
 * Style follows i386/vm_param.h in GNU Mach: the canonical page macros
 * (PAGE_SIZE, PAGE_SHIFT, PAGE_MASK) plus the round/trunc helpers, and
 * the 32-bit address-space limits of the ARMv7 architecture.
 */
#ifndef _ARMV7_VM_PARAM_H_
#define _ARMV7_VM_PARAM_H_

#include "vm_types.h"

/*
 * A byte is one addressable unit. Kept for Mach source compatibility,
 * where BYTE_SIZE appears alongside PAGE_SIZE / PAGE_SHIFT.
 */
#define BYTE_SIZE       1

/* Base page size (4 KiB). */
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1u << PAGE_SHIFT)
#define PAGESIZE        PAGE_SIZE
#define PAGE_MASK       (PAGE_SIZE - 1u)

/*
 * Address-space limits for a 32-bit ARMv7 task.
 *
 * The kernel and user maps each live inside the 4 GiB flat address
 * space. VM_MIN_ADDRESS leaves the bottom page unmapped so that a
 * NULL pointer dereference faults instead of hitting valid memory.
 * VM_MAX_ADDRESS is the first address past the usable space.
 */
#define VM_MIN_ADDRESS  ((vm_offset_t) PAGE_SIZE)
#define VM_MAX_ADDRESS  ((vm_offset_t) 0xffffffffu)

/* Canonical Mach address type bounds. */
#define VM_MIN_ADDRESS_VALUE    ((vm_offset_t) PAGE_SIZE)
#define VM_MAX_ADDRESS_VALUE    ((vm_offset_t) 0xffffffffu)

/* Round x up / down to a page boundary. Argument evaluated once. */
#define round_page(x)   (((x) + PAGE_MASK) & ~((vm_offset_t) PAGE_MASK))
#define trunc_page(x)   ((x) & ~((vm_offset_t) PAGE_MASK))
#define round_page_up(x)    round_page(x)
#define round_page_down(x)  trunc_page(x)

/* Byte <-> page conversions. */
#define atop(x)         ((vm_offset_t) (x) >> PAGE_SHIFT)
#define ptoa(x)         ((vm_offset_t) (x) << PAGE_SHIFT)

/* True when x is page-aligned. */
#define page_aligned(x) (((x) & ((vm_offset_t) PAGE_MASK)) == 0)

#endif /* _ARMV7_VM_PARAM_H_ */