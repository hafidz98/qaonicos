/*
 * vm_types.h - Basic Mach machine types for the ARMv7 port.
 *
 * All types are fixed-width 32-bit, matching the ARMv7-A AArch32
 * programming model. No libc / kernel headers are required.
 */
#ifndef _ARMV7_VM_TYPES_H_
#define _ARMV7_VM_TYPES_H_

#include <stdint.h>

/* Unsigned address / size types used throughout the VM system. */
typedef uint32_t        vm_offset_t;    /* an address in a VM space   */
typedef uint32_t        vm_size_t;      /* a size in bytes            */
typedef uint32_t        vm_address_t;   /* an address (unsigned)      */

/* 64-bit object offsets (for large object support). */
typedef uint64_t        vm_object_offset_t;
typedef uint64_t        vm_object_size_t;

/*
 * Mach natural / integer words. On a 32-bit architecture natural_t is
 * a single machine word wide; integer_t is its signed counterpart.
 */
typedef uint32_t        natural_t;
typedef int32_t         integer_t;

/* Convenience machine-word aliases. */
typedef uint32_t        word_t;
typedef int32_t         sword_t;

/* Physical address as seen by the CPU. */
typedef uint32_t        phys_addr_t;

#endif /* _ARMV7_VM_TYPES_H_ */