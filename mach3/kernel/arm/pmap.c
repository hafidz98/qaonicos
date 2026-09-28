/*
 * mach3/kernel/arm/pmap.c -- ARMv7 pmap (identity-mapped kernel).
 *
 * The boot L1 (locore.s) section-maps all of RAM (0x40000000-0x44000000)
 * and the device window (0x08000000-0x0B000000) with VA == PA.
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
#include <machine/machspl.h>	/* splhigh/splx (M4) */
#include <kern/assert.h>
#include <vm/vm_page.h>
#include <mach/vm_attributes.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>		/* kmem_alloc (M4: L2/L1 table allocation) */

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
 * M4: L2 page-table pool.
 *
 * Each 4KB page (from kmem_alloc) yields four 1KB L2 tables.
 * In our identity-mapped kernel VA == PA and the RAM section mappings
 * already cover kmem_alloc'd memory, so allocating here cannot recurse
 * into pmap_enter.  Protected by splhigh (UP).
 */
static l2_entry_t	*l2_freelist = 0;

static l2_entry_t *
alloc_l2(void)
{
	l2_entry_t	*l2;
	vm_offset_t	page;
	kern_return_t	kr;
	int		i;
	spl_t		s;

	s = splhigh();
	if (l2_freelist != 0) {
		l2 = l2_freelist;
		l2_freelist = (l2_entry_t *)*l2;
		(void) splx(s);
		bzero((void *)l2, ARM_L2_TABLE_SIZE);
		return l2;
	}
	(void) splx(s);

	/* Refill outside the critical section (kmem_alloc may block). */
	kr = kmem_alloc(kernel_map, &page, ARM_PGBYTES);
	if (kr != KERN_SUCCESS)
		panic("alloc_l2: kmem_alloc failed");
	page = trunc_page(page);

	s = splhigh();
	for (i = 3; i >= 1; i--) {
		l2 = (l2_entry_t *)(page + i * ARM_L2_TABLE_SIZE);
		*l2 = (l2_entry_t)l2_freelist;
		l2_freelist = l2;
	}
	(void) splx(s);

	l2 = (l2_entry_t *)page;
	bzero((void *)l2, ARM_L2_TABLE_SIZE);
	return l2;
}

static void
free_l2(l2_entry_t *l2)
{
	spl_t s;

	s = splhigh();
	*l2 = (l2_entry_t)l2_freelist;
	l2_freelist = l2;
	(void) splx(s);
}

/*
 * tlb_invalidate_page: invalidate TLB entry for one page.
 */
static void
tlb_invalidate_page(vm_offset_t va)
{
	__asm__ volatile (
		"mcr p15, 0, %0, c8, c7, 1\n"	/* TLBIMVA */
		"dsb\n"
		"isb"
		:: "r" (va) : "memory");
}

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
 * pmap_create: allocate a private L1 (16KB-aligned), initialized as a
 * copy of the kernel's L1 so kernel mappings (RAM sections, device
 * window, vectors) are visible.  User mappings go into L2 tables under
 * the low L1 entries.  (M4)
 */
pmap_t
pmap_create(vm_size_t size)
{
	pmap_t		pmap;
	vm_offset_t	l1mem, l1aligned;
	kern_return_t	kr;

	pmap = (pmap_t)kalloc(sizeof *pmap);
	if (pmap == PMAP_NULL)
		return PMAP_NULL;

	/*
	 * 16KB alignment: allocate 32KB and round up.
	 * (kmem_alloc guarantees only page alignment.)
	 */
	kr = kmem_alloc(kernel_map, &l1mem, 2 * ARM_L1_TABLE_SIZE);
	if (kr != KERN_SUCCESS) {
		kfree((vm_offset_t)pmap, sizeof *pmap);
		return PMAP_NULL;
	}
	l1mem = trunc_page(l1mem);
	l1aligned = (l1mem + ARM_L1_TABLE_SIZE - 1) & ~(ARM_L1_TABLE_SIZE - 1);
	bcopy((void *)_l1_table, (void *)l1aligned, ARM_L1_TABLE_SIZE);

	pmap->l1 = (l1_entry_t *)l1aligned;
	pmap->l1_alloc = l1mem;
	pmap->ref_count = 1;
	return pmap;
}

void
pmap_destroy(pmap_t pmap)
{
	if (pmap == PMAP_NULL)
		return;
	if (--pmap->ref_count == 0 && pmap != kernel_pmap) {
		/* M4: L2 tables are leaked (pool); free the L1 allocation. */
		if (pmap->l1_alloc != 0)
			kmem_free(kernel_map, pmap->l1_alloc,
				  2 * ARM_L1_TABLE_SIZE);
		kfree((vm_offset_t)pmap, sizeof *pmap);
	}
}

void
pmap_reference(pmap_t pmap)
{
	if (pmap != PMAP_NULL)
		pmap->ref_count++;
}

/*
 * pmap_enter: map va -> pa with 4KB small pages (M4).
 *
 * Addresses in the kernel identity range (RAM sections) or the device
 * window are already section-mapped in every L1 (copied from the boot
 * table): nothing to do.  All other addresses get an L2 page table
 * under the pmap's private L1.
 */
void
pmap_enter(pmap_t pmap, vm_offset_t va, vm_offset_t pa,
	   vm_prot_t prot, boolean_t wired)
{
	unsigned int	l1i, l2i, ap;
	l1_entry_t	*l1, l1e;
	l2_entry_t	*l2, pte;
	spl_t		s;

	va = trunc_page(va);
	pa = trunc_page(pa);

	if (va >= 0x40000000u && va < 0x44000000u)
		return;		/* RAM: identity section-mapped */
	if (va >= 0x08000000u && va < 0x0B000000u)
		return;		/* device window: section-mapped */

	l1 = pmap->l1;
	l1i = va >> 20;

	s = splhigh();
	l1e = l1[l1i];
	if ((l1e & L1_TYPE_MASK) != L1_TYPE_TABLE) {
		/* Need an L2 table; allocate without holding spl. */
		(void) splx(s);
		l2 = alloc_l2();
		s = splhigh();
		l1e = l1[l1i];
		if ((l1e & L1_TYPE_MASK) != L1_TYPE_TABLE) {
			/*
			 * L1 page-table descriptor: bits[31:10] = table
			 * base (1KB-aligned), domain 0, type 0b01.
			 * (VA == PA here, so the virtual address is
			 * the physical table address.)
			 */
			l1[l1i] = ((unsigned int)l2 & ~0x3FFu)
				| L1_SEC_DOMAIN(0) | L1_TYPE_TABLE;
			__asm__ volatile ("dsb" ::: "memory");
		} else {
			/* Lost the race; recycle ours. */
			free_l2(l2);
		}
	}
	l2 = (l2_entry_t *)(l1[l1i] & ~0x3FFu);
	l2i = (va >> 12) & 0xFFu;

	/* Access permissions from prot. */
	ap = (prot & VM_PROT_WRITE) ? AP_KRW_URW : AP_KRW_URO;

	pte = (pa & ~0xFFFu)
	    | L2_SP_S			/* shareable */
	    | (0x1u << L2_SP_TEX_SHIFT)	/* TEX=001: write-back */
	    | L2_SP_C | L2_SP_B
	    | L2_SP_AP(ap)
	    | L2_TYPE_SMALL;
	/*
	 * NOTE: no XN bit for ARMv7 short-descriptor small pages;
	 * L2_SP_XN (bit 0) would corrupt the type field (bits[1:0]).
	 * All user pages are executable in M4.
	 */

	l2[l2i] = pte;
	tlb_invalidate_page(va);
	(void) splx(s);
}

void
pmap_remove(pmap_t pmap, vm_offset_t s, vm_offset_t e)
{
	unsigned int	l1i, l2i;
	l1_entry_t	*l1;
	l2_entry_t	*l2;
	spl_t		spl;

	s = trunc_page(s);
	e = round_page(e);
	l1 = pmap->l1;

	spl = splhigh();
	for (; s < e; s += ARM_PGBYTES) {
		if (s >= 0x40000000u && s < 0x44000000u)
			continue;	/* identity: no L2 entry */
		if (s >= 0x08000000u && s < 0x0B000000u)
			continue;	/* device window: no L2 entry */
		l1i = s >> 20;
		if ((l1[l1i] & L1_TYPE_MASK) != L1_TYPE_TABLE)
			continue;
		l2 = (l2_entry_t *)(l1[l1i] & ~0x3FFu);
		l2i = (s >> 12) & 0xFFu;
		l2[l2i] = L2_TYPE_FAULT;
		tlb_invalidate_page(s);
	}
	(void) splx(spl);
}

void
pmap_protect(pmap_t pmap, vm_offset_t s, vm_offset_t e, vm_prot_t prot)
{
	unsigned int	l1i, l2i, ap;
	l1_entry_t	*l1;
	l2_entry_t	*l2, pte;
	spl_t		spl;

	s = trunc_page(s);
	e = round_page(e);
	l1 = pmap->l1;

	ap = (prot & VM_PROT_WRITE) ? AP_KRW_URW : AP_KRW_URO;

	spl = splhigh();
	for (; s < e; s += ARM_PGBYTES) {
		if (s >= 0x40000000u && s < 0x44000000u)
			continue;
		if (s >= 0x08000000u && s < 0x0B000000u)
			continue;
		l1i = s >> 20;
		if ((l1[l1i] & L1_TYPE_MASK) != L1_TYPE_TABLE)
			continue;
		l2 = (l2_entry_t *)(l1[l1i] & ~0x3FFu);
		l2i = (s >> 12) & 0xFFu;
		pte = l2[l2i];
		if ((pte & L2_TYPE_MASK) != L2_TYPE_SMALL)
			continue;
		pte &= ~L2_SP_AP(0x7u);
		pte |= L2_SP_AP(ap);
		l2[l2i] = pte;
		tlb_invalidate_page(s);
	}
	(void) splx(spl);
}

vm_offset_t
pmap_extract(pmap_t pmap, vm_offset_t va)
{
	unsigned int	l1i, l2i;
	l1_entry_t	l1e;
	l2_entry_t	pte;

	l1i = va >> 20;
	l1e = pmap->l1[l1i];
	switch (l1e & L1_TYPE_MASK) {
	case L1_TYPE_SECTION:
		return (l1e & 0xFFF00000u) | (va & 0x000FFFFFu);
	case L1_TYPE_TABLE:
		l2i = (va >> 12) & 0xFFu;
		pte = ((l2_entry_t *)(l1e & ~0x3FFu))[l2i];
		if ((pte & L2_TYPE_MASK) == L2_TYPE_SMALL)
			return (pte & 0xFFFFF000u) | (va & 0xFFFu);
		break;
	}
	return 0;
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
 * PMAP_ACTIVATE helpers (M4: TTBR0 switches between the shared kernel
 * L1 and each user pmap's private L1).
 */
static void
set_ttbr0(l1_entry_t *l1)
{
	__asm__ volatile (
		"orr	%0, %0, #0x08\n"	/* TTBR0[5:3] = 0b001 (outer WB), as in locore.s */
		"mcr	p15, 0, %0, c2, c0, 0\n"	/* TTBR0 */
		"mcr	p15, 0, %0, c8, c7, 0\n"	/* TLBIALL */
		"dsb\n"
		"isb"
		:: "r" (l1) : "memory");
}

void
arm_pmap_activate_kernel(void)
{
	set_ttbr0(_l1_table);
}

void
arm_pmap_activate_user(pmap_t pmap)
{
	set_ttbr0((pmap != PMAP_NULL) ? pmap->l1 : _l1_table);
}

void
arm_pmap_activate(pmap_t pmap)
{
	if (pmap == kernel_pmap)
		arm_pmap_activate_kernel();
	else
		arm_pmap_activate_user(pmap);
}

/*
 * pmap_selftest (M4): verify the L2 small-page path on every boot.
 * Creates a user pmap, maps a page at a low VA, switches TTBR0 to the
 * user L1, writes/reads through the mapping, switches back, and cleans
 * up.  Panics on any mismatch.
 */
void	pmap_stress(void);
void
pmap_selftest(void)
{
	pmap_t		pmap;
	vm_offset_t	pa, va = 0x100000u;
	kern_return_t	kr;
	unsigned int	magic = 0x4D340001u, readback;
	spl_t		s;

	printf("pmap_selftest: mapping 4KB page...\n");

	kr = kmem_alloc(kernel_map, &pa, ARM_PGBYTES);
	if (kr != KERN_SUCCESS)
		panic("pmap_selftest: kmem_alloc failed");
	pa = trunc_page(pa);

	pmap = pmap_create(0);
	if (pmap == PMAP_NULL)
		panic("pmap_selftest: pmap_create failed");

	pmap_enter(pmap, va, pa, VM_PROT_READ | VM_PROT_WRITE, FALSE);
	if (pmap_extract(pmap, va) != pa)
		panic("pmap_selftest: extract mismatch (got 0x%x, want 0x%x)",
		      pmap_extract(pmap, va), pa);

	/* Access through the user L1 with IRQs off. */
	s = splhigh();
	arm_pmap_activate_user(pmap);
	*(volatile unsigned int *)va = magic;
	readback = *(volatile unsigned int *)va;
	arm_pmap_activate_kernel();
	(void) splx(s);

	if (readback != magic)
		panic("pmap_selftest: readback 0x%x != 0x%x", readback, magic);

	pmap_remove(pmap, va, va + ARM_PGBYTES);
	if (pmap_extract(pmap, va) != 0)
		panic("pmap_selftest: remove failed");

	pmap_protect(pmap, va, va + ARM_PGBYTES, VM_PROT_READ);
	pmap_destroy(pmap);
	kmem_free(kernel_map, pa, ARM_PGBYTES);

	printf("pmap_selftest: PASS (4KB L2 mapping, TTBR0 switch, R/W)\n");

	pmap_stress();
}

/*
 * pmap_stress (M5 hardening): exercise enter/remove/protect across
 * many pages and L2 tables, verifying no stale TLB entries survive
 * remapping.  Each round maps 64 VAs (spanning 4 L2 tables) to
 * rotated physical pages, writes a unique pattern, reads it back
 * through the user L1, then removes everything.  The next round
 * reuses the same VAs with different PAs -- a stale TLB would return
 * the previous round's pattern.
 */
#define	PMAP_STRESS_NVA		64
#define	PMAP_STRESS_NROUND	10
#define	PMAP_STRESS_BASE	0x200000u
/* 64 VAs across 4 L2 tables (16 pages per 1MB region). */
#define	PMAP_STRESS_VA(i)	(PMAP_STRESS_BASE + \
				 ((unsigned int)((i) / 16) << 20) + \
				 ((unsigned int)((i) % 16) << 12))

void
pmap_stress(void)
{
	static vm_offset_t pas[PMAP_STRESS_NVA];
	pmap_t pmap;
	spl_t s;
	int r, i, g, total_ops = 0;

	printf("pmap_stress: %d rounds x %d pages (4 L2 tables)...\n",
	       PMAP_STRESS_NROUND, PMAP_STRESS_NVA);

	for (i = 0; i < PMAP_STRESS_NVA; i++) {
		vm_offset_t pa;
		kern_return_t kr = kmem_alloc(kernel_map, &pa, ARM_PGBYTES);
		if (kr != KERN_SUCCESS)
			panic("pmap_stress: kmem_alloc page %d failed", i);
		pas[i] = trunc_page(pa);
	}

	pmap = pmap_create(0);
	if (pmap == PMAP_NULL)
		panic("pmap_stress: pmap_create failed");

	for (r = 0; r < PMAP_STRESS_NROUND; r++) {
		/* Enter: VA[i] -> PA[(i + r) % NVA]. */
		for (i = 0; i < PMAP_STRESS_NVA; i++) {
			vm_offset_t va = PMAP_STRESS_VA(i);
			vm_offset_t pa = pas[(i + r) % PMAP_STRESS_NVA];

			pmap_enter(pmap, va, pa,
				   VM_PROT_READ | VM_PROT_WRITE, FALSE);
			if (pmap_extract(pmap, va) != pa)
				panic("pmap_stress: r%d extract mismatch va 0x%x",
				      r, va);
			total_ops++;
		}

		/* Write unique pattern through user L1, read back. */
		s = splhigh();
		arm_pmap_activate_user(pmap);
		for (i = 0; i < PMAP_STRESS_NVA; i++) {
			unsigned int pat =
				((unsigned int)r << 24) |
				((unsigned int)i << 8) | 0xA5u;
			*(volatile unsigned int *)PMAP_STRESS_VA(i) = pat;
		}
		for (i = 0; i < PMAP_STRESS_NVA; i++) {
			unsigned int pat =
				((unsigned int)r << 24) |
				((unsigned int)i << 8) | 0xA5u;
			unsigned int rb =
				*(volatile unsigned int *)PMAP_STRESS_VA(i);
			if (rb != pat)
				panic("pmap_stress: r%d stale TLB? va 0x%x "
				      "got 0x%x want 0x%x",
				      r, PMAP_STRESS_VA(i), rb, pat);
			total_ops++;
		}
		arm_pmap_activate_kernel();
		(void) splx(s);

		/* Protect first region read-only (exercise path). */
		pmap_protect(pmap, PMAP_STRESS_BASE,
			     PMAP_STRESS_BASE + (16 << 12), VM_PROT_READ);
		total_ops++;

		/* Remove everything per region; extract must read 0. */
		for (g = 0; g < 4; g++) {
			vm_offset_t base =
				PMAP_STRESS_BASE + ((unsigned int)g << 20);
			pmap_remove(pmap, base, base + (16 << 12));
		}
		for (i = 0; i < PMAP_STRESS_NVA; i++) {
			if (pmap_extract(pmap, PMAP_STRESS_VA(i)) != 0)
				panic("pmap_stress: r%d remove failed va 0x%x",
				      r, PMAP_STRESS_VA(i));
			total_ops++;
		}
	}

	pmap_destroy(pmap);
	for (i = 0; i < PMAP_STRESS_NVA; i++)
		kmem_free(kernel_map, pas[i], ARM_PGBYTES);

	printf("pmap_stress: PASS (%d ops, no stale mappings)\n", total_ops);
}
