/*
 * mach3/kernel/arm/pmap.c -- ARMv7 pmap (identity-mapped kernel).
 *
 * The boot L1 (locore.s) section-maps all of RAM (0x40000000-0x44000000)
 * and the device window (0x08000000-0x0A000000) with VA == PA.
 * This pmap therefore treats kernel virtual addresses as physical:
 * pmap_enter() is a no-op for in-RAM addresses, and the early
 * allocation path (pmap_virtual_space / pmap_next_page / pmap_free_pages)
 * feeds the MI generic pmap_steal_memory/pmap_startup in vm_resident.c.
 *
 * Physical page pool : [phys_pool_start, 0x43000000)
 * Virtual steal range: [0x43000000, 0x44000000)  (identity-mapped)
 * The two regions never overlap: phys_pool_start is just past the
 * kernel image (a few MB), far below 0x43000000.
 */
#include <mach/machine/vm_types.h>
#include <mach/machine/vm_param.h>
#include <machine/pmap.h>
#include <machine/pte.h>
#include <kern/assert.h>
#include <vm/vm_page.h>
#include <mach/vm_attributes.h>
#include <vm/pmap.h>

extern char _end[];
extern vm_offset_t	avail_start, avail_end;	/* set by arm_init() */

/* Boot L1 table built by locore.s. */
extern l1_entry_t _l1_table[];

pmap_t		kernel_pmap;

/* Early allocation state. */
static vm_offset_t	phys_pool_start;	/* first free physical page */
static vm_offset_t	phys_pool_next;	/* next page to hand out */
#define	PHYS_POOL_END	((vm_offset_t)0x43000000)
#define	VIRT_STEAL_START ((vm_offset_t)0x43000000)
#define	VIRT_STEAL_END	((vm_offset_t)0x44000000)
static vm_offset_t	virt_steal_next = VIRT_STEAL_START;	/* next steal addr */

static struct pmap	kernel_pmap_store;

/*
 * pmap_bootstrap: adopt the boot L1, set up the physical page pool.
 * Called from arm_init() before machine_startup().
 */
void
pmap_bootstrap(void)
{
	kernel_pmap = &kernel_pmap_store;
	kernel_pmap->l1 = _l1_table;
	kernel_pmap->ref_count = 1;

	phys_pool_start = arm_round_page(avail_start);
	phys_pool_next = phys_pool_start;

	if (phys_pool_start >= PHYS_POOL_END)
		panic("pmap_bootstrap: kernel image too large");

	/*
	 * Invalidate the TLB once more now that the table is final,
	 * and make sure the shared domain is client.
	 */
	__asm__ volatile (
		"mcr p15, 0, %0, c8, c7, 0\n"	/* TLBIALL */
		"dsb\n"
		"isb"
		: : "r" (0) : "memory");
}

/*
 * pmap_virtual_space: virtual range for pmap_steal_memory.
 */
void
pmap_virtual_space(vm_offset_t *startp, vm_offset_t *endp)
{
	*startp = VIRT_STEAL_START;
	*endp = VIRT_STEAL_END;
}

/*
 * pmap_next_page: hand out the next free physical page (bootstrap only).
 */
boolean_t
pmap_next_page(vm_offset_t *paddrp)
{
	if (phys_pool_next + ARM_PGBYTES > PHYS_POOL_END)
		return FALSE;
	*paddrp = phys_pool_next;
	phys_pool_next += ARM_PGBYTES;
	return TRUE;
}

/*
 * pmap_free_pages: number of physical pages available to the pool.
 */
unsigned int
pmap_free_pages(void)
{
	return (PHYS_POOL_END - phys_pool_start) / ARM_PGBYTES;
}

/*
 * pmap_startup: initialize physical page structures.
 * Overrides MI generic version in vm_resident.c.
 *
 * Same population logic as the MI generic (steal page structs, then
 * pmap_next_page/vm_page_init/vm_page_release to build the free list),
 * except the kernel virtual range handed to kmem_init starts at
 * avail_start (first free page past the kernel image) instead of the
 * steal-memory pointer -- MI would hand kmem_init a range already
 * consumed by pmap_steal_memory, starving kmem_suballoc (ipc_map).
 */
void
pmap_startup(vm_offset_t *startp, vm_offset_t *endp)
{
	unsigned int i, npages, pages_initialized;
	vm_page_t pages;
	vm_offset_t paddr;

	npages = (ARM_PGBYTES * pmap_free_pages()) /
		 (ARM_PGBYTES + sizeof *pages);

	pages = (vm_page_t) pmap_steal_memory(npages * sizeof *pages);

	for (i = 0, pages_initialized = 0; i < npages; i++) {
		if (!pmap_next_page(&paddr))
			break;
		vm_page_init(&pages[i], paddr);
		pages_initialized++;
	}

	/*
	 * Release in reverse so physical pages allocate in ascending
	 * order (keeps devices needing consecutive pages happy).
	 */
	for (i = pages_initialized; i > 0; i--)
		vm_page_release(&pages[i - 1]);

	*startp = round_page(avail_start);
	*endp = (vm_offset_t)VM_MAX_KERNEL_ADDRESS;
}

/*
 * pmap_steal_memory: allocate from virtual steal range (for MI
 * vm_page_bootstrap data structures).  Overrides MI generic version.
 */
vm_offset_t
pmap_steal_memory(vm_size_t size)
{
	vm_offset_t addr;

	size = (size + 3) & ~3;
	if (virt_steal_next + size > VIRT_STEAL_END)
		panic("pmap_steal_memory: out of virtual steal space");
	addr = virt_steal_next;
	virt_steal_next += size;
	return addr;
}

/*
 * pmap_init: nothing to do (boot L1 already active).
 */
void
pmap_init(void)
{
}

/*
 * pmap_create: user pmaps share the boot L1 on this UP port
 * (no user address spaces in M3).
 */
pmap_t
pmap_create(vm_size_t size)
{
	pmap_t pmap;

	pmap = (pmap_t)kalloc(sizeof *pmap);
	if (pmap == PMAP_NULL)
		return PMAP_NULL;
	pmap->l1 = _l1_table;
	pmap->ref_count = 1;
	return pmap;
}

void
pmap_destroy(pmap_t pmap)
{
	if (pmap == PMAP_NULL)
		return;
	if (--pmap->ref_count == 0 && pmap != kernel_pmap)
		kfree((vm_offset_t)pmap, sizeof *pmap);
}

void
pmap_reference(pmap_t pmap)
{
	if (pmap != PMAP_NULL)
		pmap->ref_count++;
}

/*
 * pmap_enter: identity map -- addresses in RAM/device window are already
 * valid.  Anything else is a port limitation in M3.
 */
void
pmap_enter(pmap_t pmap, vm_offset_t va, vm_offset_t pa,
	   vm_prot_t prot, boolean_t wired)
{
	if (va >= 0x40000000u && va < 0x44000000u)
		return;		/* RAM: identity-mapped */
	if (va >= 0x08000000u && va < 0x0A000000u)
		return;		/* device window: section-mapped */
	panic("pmap_enter: va 0x%x not identity-mapped (M3 limitation)",
	      va);
}

void
pmap_remove(pmap_t pmap, vm_offset_t s, vm_offset_t e)
{
	/* identity-mapped: nothing to remove */
}

void
pmap_protect(pmap_t pmap, vm_offset_t s, vm_offset_t e, vm_prot_t prot)
{
	/* identity-mapped: protection is fixed at section granularity */
}

vm_offset_t
pmap_extract(pmap_t pmap, vm_offset_t va)
{
	/* identity map */
	return va;
}

vm_offset_t
pmap_phys_address(vm_offset_t phys)
{
	return phys;
}

/*
 * Page operations (identity: operate directly).
 */
void
pmap_zero_page(vm_offset_t pa)
{
	bzero((void *)pa, ARM_PGBYTES);
}

void
pmap_copy_page(vm_offset_t src, vm_offset_t dst)
{
	bcopy((void *)src, (void *)dst, ARM_PGBYTES);
}

void
pmap_pageable(pmap_t pmap, vm_offset_t s, vm_offset_t e, boolean_t pageable)
{
}

void
pmap_change_wiring(pmap_t pmap, vm_offset_t va, boolean_t wired)
{
}

void
pmap_collect(pmap_t pmap)
{
}

kern_return_t
pmap_attribute(pmap_t pmap, vm_offset_t va, vm_size_t size,
	       vm_machine_attribute_t attribute,
	       vm_machine_attribute_val_t *value)
{
	return KERN_INVALID_ARGUMENT;
}

void
pmap_clear_modify(vm_offset_t pa)
{
}

boolean_t
pmap_is_modified(vm_offset_t pa)
{
	return FALSE;
}

void
pmap_clear_reference(vm_offset_t pa)
{
}

boolean_t
pmap_is_referenced(vm_offset_t pa)
{
	return FALSE;
}

pmap_t
pmap_kernel(void)
{
	return kernel_pmap;
}

void
pmap_page_protect(vm_offset_t pa, vm_prot_t prot)
{
	/* identity-mapped: protection fixed at section granularity */
}

int
pmap_resident_count(pmap_t pmap)
{
	/* M3: identity-mapped; approximate as 0 (not tracked). */
	return 0;
}

void
pmap_copy(pmap_t dst_pmap, pmap_t src_pmap, vm_offset_t dst_va,
	  vm_size_t len, vm_offset_t src_va)
{
	/* identity-mapped: nothing to do */
}

/*
 * PMAP_ACTIVATE helpers (UP: TTBR0 never changes in M3).
 */
void
arm_pmap_activate_kernel(void)
{
}

void
arm_pmap_activate_user(pmap_t pmap)
{
}

void
arm_pmap_activate(pmap_t pmap)
{
}
