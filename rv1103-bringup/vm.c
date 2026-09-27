/*
 * vm.c - Per-task virtual address spaces, ARMv7 short descriptor.
 *
 * L1: 4096 entries. Entries copied from the kernel's L1 stay 1 MiB
 * sections; the user region gets L1 page-table descriptors (type 0b01)
 * pointing at 1 KiB L2 tables with 256 small-page (4 KiB) entries each.
 *
 * L2 small-page descriptor built here:
 *   bits[31:12] physical base, nG=0, S=0,
 *   AP:  RW -> AP[1:0]=0b11, APX=0 (priv+user read/write)
 *        RO -> AP[1:0]=0b10, APX=1 (priv+user read-only)
 *   TEX=0b001, C=1, B=1 (same write-back cacheable memory type the
 *   kernel's own DRAM sections use), bit[1]=1 (page), bit[0]=XN unless
 *   VM_PROT_EXEC.
 *
 * Tables and pages come from static BSS pools (bump allocation, no
 * reuse yet - honest for bring-up; a real page allocator replaces this
 * when the Mach VM system lands). BSS is 1:1 mapped by the kernel's
 * pmap, so table addresses are valid both as pointers and as TTBR0 /
 * descriptor base fields.
 */
#include "vm.h"

/* Pool sizes: room for a handful of tasks; bumped up when needed. */
#define VM_NSPACES  4u
#define VM_NL2      16u
#define VM_NPAGES   32u

static uint32_t l1_pool[VM_NSPACES][4096] __attribute__((aligned(16384)));
static uint32_t l2_pool[VM_NL2][256]      __attribute__((aligned(1024)));
static uint8_t  page_pool[VM_NPAGES][VM_PAGE_SIZE]
    __attribute__((aligned(4096)));

static unsigned l1_next;
static unsigned l2_next;
static unsigned page_next;

/* ------------------------------------------------------------------ */
/* CP15 / barrier helpers (same operations pmap.c uses).               */
/* ------------------------------------------------------------------ */
static inline void vm_dsb(void)
{
    __asm__ __volatile__("dsb" ::: "memory");
}

static inline void vm_isb(void)
{
    __asm__ __volatile__("isb" ::: "memory");
}

static inline uint32_t vm_read_ttbr0(void)
{
    uint32_t v;
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
    return v;
}

static inline void vm_write_ttbr0(uint32_t v)
{
    __asm__ __volatile__("mcr p15, 0, %0, c2, c0, 0" :: "r"(v) : "memory");
}

static inline void vm_invalidate_tlb(void)
{
    __asm__ __volatile__("mcr p15, 0, %0, c8, c7, 0" :: "r"(0u) : "memory");
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */
void vm_init(void)
{
    l1_next = 0u;
    l2_next = 0u;
    page_next = 0u;
    /* Pools are BSS: already zeroed. */
}

uint32_t *vm_current_l1(void)
{
    return (uint32_t *)(vm_read_ttbr0() & 0xFFFFC000u);
}

int vm_space_init(struct vm_space *sp)
{
    uint32_t *l1, *kern;
    unsigned i;

    if (!sp || l1_next >= VM_NSPACES)
        return -1;
    l1 = l1_pool[l1_next++];

    /* Inherit every kernel mapping (DRAM 1:1 sections, device window)
     * so code, stacks, UART and GIC keep working after a switch. */
    kern = vm_current_l1();
    for (i = 0u; i < 4096u; i++)
        l1[i] = kern[i];
    vm_dsb();

    sp->l1 = l1;
    return 0;
}

int vm_map(struct vm_space *sp, uint32_t va, uint32_t pa, unsigned prot)
{
    uint32_t l1i, l2i, e, desc, ap_lo, apx;
    uint32_t *l2;
    unsigned i;

    if (!sp || !sp->l1)
        return -1;
    if ((va & VM_PAGE_MASK) || (pa & VM_PAGE_MASK))
        return -1;

    l1i = va >> 20;
    e = sp->l1[l1i];
    if ((e & 0x3u) == 0x1u) {
        /* L2 table already exists for this 1 MiB region. */
        l2 = (uint32_t *)(e & 0xFFFFFC00u);
    } else if ((e & 0x3u) == 0x0u) {
        /* First mapping in this region: allocate an L2 table. */
        if (l2_next >= VM_NL2)
            return -1;
        l2 = l2_pool[l2_next++];
        for (i = 0u; i < 256u; i++)
            l2[i] = 0u;
        /* L1 page-table descriptor: base[31:10], domain 0, type 0b01. */
        sp->l1[l1i] = ((uint32_t)l2 & 0xFFFFFC00u) | 0x1u;
    } else {
        /* A 1 MiB section already covers va - refuse to clobber it. */
        return -1;
    }

    /* Access permissions. Everything still runs privileged, but encode
     * prot honestly for the day user mode arrives. */
    if ((prot & (VM_PROT_READ | VM_PROT_WRITE)) ==
        (VM_PROT_READ | VM_PROT_WRITE)) {
        ap_lo = 0x3u; apx = 0u;   /* priv+user read/write */
    } else {
        ap_lo = 0x2u; apx = 1u;   /* priv+user read-only  */
    }

    desc = (pa & 0xFFFFF000u)
         | (ap_lo << 4) | (apx << 9)      /* AP */
         | (0x1u << 6)                    /* TEX = 0b001 */
         | (1u << 3) | (1u << 2)          /* C, B: write-back cacheable */
         | (1u << 1);                     /* small page */
    if (!(prot & VM_PROT_EXEC))
        desc |= 0x1u;                     /* XN */

    l2i = (va >> 12) & 0xFFu;
    l2[l2i] = desc;
    vm_dsb();   /* table write must be visible before any walk uses it */
    return 0;
}

uint32_t vm_page_alloc(void)
{
    uint8_t *pg;
    unsigned i;

    if (page_next >= VM_NPAGES)
        return 0u;
    pg = page_pool[page_next++];
    for (i = 0u; i < VM_PAGE_SIZE; i++)
        pg[i] = 0u;
    return (uint32_t)pg;
}

/* The space the CPU is currently running in. Set by vm_space_switch;
 * the pager maps faulted pages into this space. */
static struct vm_space *vm_cur_space;

static inline void vm_tlbimva(uint32_t va)
{
    __asm__ __volatile__("mcr p15, 0, %0, c8, c7, 1" :: "r"(va) : "memory");
}

void vm_space_switch(struct vm_space *sp)
{
    vm_dsb();
    /* Same TTBR0 format pmap_enable() uses: bare table base, the low
     * 14 bits are zero from the 16 KiB alignment. */
    vm_write_ttbr0((uint32_t)sp->l1);
    vm_cur_space = sp;
    /* ISB (bukan cuma DSB) setelah tulis TTBR0: pastikan nilai baru
     * terlihat sebelum TLB invalidate. Tanpa ini QEMU Cortex-A7 kadang
     * (1/15) data abort sporadis pasca-switch. */
    vm_isb();
    vm_dsb();
    vm_invalidate_tlb();   /* old ASID-less entries must not survive */
    vm_dsb();
    vm_isb();
}

struct vm_space *vm_current_space(void)
{
    return vm_cur_space;
}

/* Look up the L2 small-page descriptor for va in sp, or 0 if va is not
 * mapped as a page (unmapped, or covered by a 1 MiB section). */
uint32_t vm_lookup(struct vm_space *sp, uint32_t va)
{
    uint32_t l1i, e, l2i;
    uint32_t *l2;

    if (!sp || !sp->l1 || (va & VM_PAGE_MASK))
        return 0u;
    l1i = va >> 20;
    e = sp->l1[l1i];
    if ((e & 0x3u) != 0x1u)
        return 0u;              /* no L2 table: unmapped or a section */
    l2 = (uint32_t *)(e & 0xFFFFFC00u);
    l2i = (va >> 12) & 0xFFu;
    return l2[l2i];
}

int vm_unmap(struct vm_space *sp, uint32_t va)
{
    uint32_t l1i, e, l2i;
    uint32_t *l2;

    if (!sp || !sp->l1 || (va & VM_PAGE_MASK))
        return -1;
    l1i = va >> 20;
    e = sp->l1[l1i];
    if ((e & 0x3u) != 0x1u)
        return -1;              /* no L2 table here */
    l2 = (uint32_t *)(e & 0xFFFFFC00u);
    l2i = (va >> 12) & 0xFFu;
    if (l2[l2i] == 0u)
        return -1;              /* already unmapped */
    l2[l2i] = 0u;
    vm_dsb();
    /* If this is the live space, drop the stale TLB entry now; a
     * full TLBIALL from thread context trips a QEMU race (see Fase 5
     * notes), so invalidate just this page. */
    if (sp == vm_cur_space) {
        vm_tlbimva(va);
        vm_dsb();
        vm_isb();
    }
    return 0;
}

/*
 * Pager: resolve a data abort when we can.
 * Returns 1 when the fault is fixed and the faulting instruction should
 * be retried, 0 when it is a genuine bug (caller reports and parks).
 *
 * Handled case: translation fault (section 0x5 / page 0x7) on an address
 * inside VM_DEMAND_BASE..VM_DEMAND_END. We allocate a zeroed page and
 * map it read/write into the current space - demand paging, the seed of
 * every Mach VM feature (lazy allocation, and later COW / pager-backed
 * mappings). Permission faults, alignment faults, and faults outside the
 * demand range are NOT resolved.
 */
int vm_page_fault(uint32_t far, uint32_t fsr)
{
    unsigned fs;
    uint32_t pa, va;
    struct vm_space *sp;

    /* ARMv7 short-descriptor FS: bits[3:0] + bit[10] -> FS[4]. */
    fs = (fsr & 0xFu) | ((fsr >> 6) & 0x10u);
    if (fs != 0x5u && fs != 0x7u)
        return 0;               /* not a translation fault */
    if (far < VM_DEMAND_BASE || far >= VM_DEMAND_END)
        return 0;               /* outside the demand range */

    sp = vm_cur_space;
    if (!sp)
        return 0;
    va = far & ~VM_PAGE_MASK;
    if (vm_lookup(sp, va) != 0u)
        return 0;               /* already mapped: weird, don't loop */
    pa = vm_page_alloc();
    if (pa == 0u)
        return 0;               /* out of pages */
    if (vm_map(sp, va, pa, VM_PROT_READ | VM_PROT_WRITE) != 0)
        return 0;
    /* vm_map did a DSB; make the new entry visible to the walker now. */
    vm_tlbimva(va);
    vm_dsb();
    vm_isb();
    return 1;
}

int vm_probe(struct vm_space *sp, uint32_t va, uint32_t pattern)
{
    unsigned cpsr;
    uint32_t back;
    volatile uint32_t *p = (volatile uint32_t *)va;
    uint32_t old_l1;
    int miss;

    /* Mask IRQs across switch+write+read: a timer tick in the middle
     * could otherwise move the CPU to another thread's space and the
     * read would (correctly!) see the other task's data. */
    __asm__ __volatile__("mrs %0, cpsr\n\tcpsid i"
                         : "=r"(cpsr) :: "memory");
    old_l1 = (uint32_t)vm_current_l1();
    vm_space_switch(sp);
    *p = pattern;
    back = *p;
    /* Restore the previous address space BEFORE re-enabling IRQs, so a
     * pending timer tick never runs the scheduler in a thread's space. */
    vm_write_ttbr0(old_l1);
    vm_dsb();
    vm_invalidate_tlb();
    vm_dsb();
    vm_isb();
    __asm__ __volatile__("msr cpsr_c, %0" :: "r"(cpsr) : "memory");

    miss = (back != pattern);
    return miss;
}
