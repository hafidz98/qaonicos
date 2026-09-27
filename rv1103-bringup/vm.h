/*
 * vm.h - Per-task virtual address spaces (bring-up).
 *
 * Fase 5 of the Mach-x-Luckfox port: small (4 KiB) pages on top of the
 * ARMv7 short-descriptor MMU. Each vm_space owns a 16 KiB L1 table that
 * starts as a copy of the kernel's L1 (so the 1:1 DRAM + device mappings
 * keep working in every space) and gains per-task L2 page-table mappings
 * in the user region. Switching spaces = TTBR0 write + full TLB flush.
 *
 * This is deliberately NOT Mach's VM system yet: no pmap_enter/pmap_remove
 * with PV lists, no VM objects, no copy-on-write, no page faults handled
 * by a pager. Those come later. What this proves: two tasks can map the
 * same virtual address to different physical pages and not see each
 * other's data - the mechanism every Mach VM feature builds on.
 *
 * Build assumptions: C99, -ffreestanding, no libc, ARMv7-A.
 */
#ifndef _VM_H_
#define _VM_H_

#include <stdint.h>

#define VM_PAGE_SIZE    4096u
#define VM_PAGE_MASK    (VM_PAGE_SIZE - 1u)

/* Demand-paging range: the pager (vm_page_fault) resolves translation
 * faults here by mapping a fresh zeroed page on demand. */
#define VM_DEMAND_BASE  0x10000000u
#define VM_DEMAND_END   0x11000000u

/* Protection flags for vm_map(). */
#define VM_PROT_READ    0x1u
#define VM_PROT_WRITE   0x2u
#define VM_PROT_EXEC    0x4u    /* without it the page is execute-never */

/* Forward: struct task didefinisikan di task.h (task.h me-include
 * vm.h, jadi di sini cukup forward declaration). */
struct task;

/* A task's address space: its own L1 translation table.
 * The table lives in kernel BSS (1:1 mapped), so l1 works as both
 * the virtual address for table edits and the physical address for
 * TTBR0. */
struct vm_space {
    uint32_t *l1;
};

/* Reset the table/page pools. Call once, before pmap is even needed -
 * the pools live in BSS so they are valid as soon as C runs. */
void vm_init(void);

/* Current TTBR0 L1 base (the kernel's table after pmap_enable()). */
uint32_t *vm_current_l1(void);

/* Allocate an L1 from the pool and copy the kernel's mappings into it.
 * Returns 0 on success, -1 when the space pool is exhausted. */
int vm_space_init(struct vm_space *sp);

/* Map one 4 KiB page: va -> pa with prot. va and pa must be page
 * aligned. Allocates an L2 table on first use of the 1 MiB region.
 * Returns 0 on success, -1 on conflict (a section already covers va),
 * misalignment, or pool exhaustion. */
int vm_map(struct vm_space *sp, uint32_t va, uint32_t pa, unsigned prot);

/* Allocate one zeroed 4 KiB physical page from the pool.
 * Returns its address (== usable as pa for vm_map), or 0 when empty. */
uint32_t vm_page_alloc(void);

/* Switch the CPU to sp's address space. Safe to call with the MMU on:
 * kernel mappings are identical in every space, and the tables live in
 * 1:1-mapped BSS. */
void vm_space_switch(struct vm_space *sp);

/* The space the CPU is currently running in (set by vm_space_switch),
 * or NULL if no switch has happened yet. The pager maps into this. */
struct vm_space *vm_current_space(void);

/* Return the L2 small-page descriptor for va in sp, or 0 when va is not
 * mapped as a page. */
uint32_t vm_lookup(struct vm_space *sp, uint32_t va);

/* Remove the page mapping for va in sp. Returns 0 on success, -1 when
 * va is not page-mapped. */
int vm_unmap(struct vm_space *sp, uint32_t va);

/*
 * Copy-on-write (Fase 7, bring-up).
 *
 * Petakan pa FISIK YANG SAMA dari va milik src ke dst, keduanya
 * read-only, dan catat pasangan (space, va) di tabel COW. Write
 * pertama ke va di salah satu space memicu permission fault;
 * vm_page_fault lalu menyalin halaman ke pa baru + remap read/write
 * di space yang fault (cow_break). Space yang lain tetap menunjuk pa
 * asal (masih COW sampai ia sendiri menulis).
 *
 * Keterbatasan jujur: halaman tidak pernah di-free di desain ini
 * (pool bump-allocator; vm_unmap pun tidak mengembalikan pa), jadi
 * tidak ada refcount - halaman COW asal tetap hidup selama masih ada
 * yang menunjuknya. Jangan panggil vm_unmap pada va COW lalu pakai
 * lagi: entry COW-nya tidak dibersihkan (didokumentasikan, bukan bug
 * tersembunyi).
 *
 * 0 = ok, -1 = gagal (va tidak ter-map di src / tidak sejajar /
 * tabel COW penuh).
 */
int vm_share_cow(struct task *dst, struct task *src, uint32_t va);

/* Pager: try to resolve a data abort (far = DFAR, fsr = DFSR).
 * Returns 1 when the faulting instruction should be retried, 0 when
 * the fault is genuine. Resolves translation faults inside the demand
 * range by mapping a fresh zeroed page. */
int vm_page_fault(uint32_t far, uint32_t fsr);

/*
 * Atomic isolation probe, for threads: mask IRQs, switch to sp, write
 * pattern to va, read it back, restore IRQs. Returns 0 when the value
 * read back equals pattern (this space is intact), 1 on mismatch.
 * The IRQ masking matters: without it a timer tick could switch the
 * CPU to another thread's space between our write and our read.
 */
int vm_probe(struct vm_space *sp, uint32_t va, uint32_t pattern);

/* Fase 12: statistik pool untuk HTTP /metrics. */
struct vm_stats {
    unsigned pages_used, pages_total;
    unsigned l1_used, l1_total;
    unsigned l2_used, l2_total;
};
void vm_get_stats(struct vm_stats *s);

#endif /* _VM_H_ */
