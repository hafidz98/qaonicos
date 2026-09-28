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
#include "sched.h"      /* sched_current_task() untuk pager_lookup */
#include "pager.h"      /* pager_lookup/pager_fetch_page (Fase 7) */
#include "lib.h"        /* memcpy untuk cow_break */

/* Pool sizes: room for a handful of tasks; bumped up when needed.
 * Fase 7: 8 space (kern + kern-thread + A + B + pager + C + cadangan),
 * halaman 48 (pager + COW butuh beberapa). */
#define VM_NSPACES  8u
#define VM_NL2      24u
/* Fase 14: 48 -> 64. Tiap program userspace butuh 6 halaman
 * (4 prog + 2 stack); program ke-7 (ugpio) menghabiskan pool 48. */
#define VM_NPAGES   96u   /* Fase 16: program userspace ke-9 (ufs);
                         * tiap program butuh 6 halaman (4 prog + 2 stack) */

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

/* Fase 12: statistik pool untuk HTTP /metrics. */
void vm_get_stats(struct vm_stats *s)
{
    if (!s)
        return;
    s->pages_used = page_next;
    s->pages_total = VM_NPAGES;
    s->l1_used = l1_next;
    s->l1_total = VM_NSPACES;
    s->l2_used = l2_next;
    s->l2_total = VM_NL2;
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
 * Copy-on-write (Fase 7).
 *
 * Tabel kecil {space, va} -> pa asal. vm_share_cow() memetakan pa yang
 * sama secara read-only di dua space; write pertama ke va memicu
 * permission fault (FS 0xD/0xF) dan cow_break() menyalin halaman ke pa
 * baru + remap read/write di space yang fault. Slot space yang break
 * dilepas; space lain tetap COW terhadap pa asal.
 */
#define VM_NCOW 8u
static struct {
    struct vm_space *sp;
    uint32_t va;
    uint32_t pa;
} cow_tab[VM_NCOW];

int vm_share_cow(struct task *dst, struct task *src, uint32_t va)
{
    uint32_t desc, pa;
    int i, done = 0;

    if (!dst || !src || dst == src || (va & VM_PAGE_MASK))
        return -1;
    desc = vm_lookup(&src->vm, va);
    if (desc == 0u)
        return -1;              /* src belum memetakan va */
    pa = desc & 0xFFFFF000u;

    /* Butuh 2 slot kosong (satu per space). */
    for (i = 0; i < (int)VM_NCOW; i++)
        if (cow_tab[i].sp == 0)
            done++;
    if (done < 2)
        return -1;
    done = 0;

    /* Kedua sisi jadi read-only; write pertama memicu COW break. */
    if (vm_map(&dst->vm, va, pa, VM_PROT_READ) != 0)
        return -1;
    if (vm_map(&src->vm, va, pa, VM_PROT_READ) != 0)
        return -1;
    /* vm_map tidak invalidate TLB: paksa sekarang. Tanpa ini entry RW
     * lama yang masih ke-cache membuat write tidak fault. */
    vm_tlbimva(va);
    vm_dsb();
    vm_isb();

    for (i = 0; i < (int)VM_NCOW && done < 2; i++) {
        if (cow_tab[i].sp == 0) {
            cow_tab[i].sp = (done == 0) ? &dst->vm : &src->vm;
            cow_tab[i].va = va;
            cow_tab[i].pa = pa;
            done++;
        }
    }
    return 0;
}

/* Break COW untuk (sp, va): salin halaman ke pa baru, remap RW.
 * Return 1 bila di-break, 0 bila va bukan halaman COW (genuine fault).
 * Dipanggil dari vm_page_fault saat permission fault. */
static int cow_break(struct vm_space *sp, uint32_t va)
{
    int i;
    uint32_t pa, npa;

    for (i = 0; i < (int)VM_NCOW; i++)
        if (cow_tab[i].sp == sp && cow_tab[i].va == va)
            break;
    if (i == (int)VM_NCOW)
        return 0;               /* bukan halaman COW */
    pa = cow_tab[i].pa;
    cow_tab[i].sp = 0;          /* slot bebas; space lain tidak tersentuh */

    /* Tidak ada refcount: halaman tidak pernah di-free di desain
     * bump-allocator ini, jadi pa asal tetap valid selama masih ada
     * space yang menunjuknya. */
    npa = vm_page_alloc();
    if (npa == 0u)
        return 0;
    memcpy((void *)npa, (void *)pa, VM_PAGE_SIZE);
    if (vm_map(sp, va, npa, VM_PROT_READ | VM_PROT_WRITE) != 0)
        return 0;
    if (sp == vm_cur_space) {
        vm_tlbimva(va);
        vm_dsb();
        vm_isb();
    }
    return 1;
}

/*
 * Pager: resolve a data abort when we can.
 * Returns 1 when the fault is fixed and the faulting instruction should
 * be retried, 0 when it is a genuine bug (caller reports and parks).
 *
 * Fase 5: translation fault (section 0x5 / page 0x7) di demand range
 * -> zero-fill (alokasi halaman zeroed + map RW).
 * Fase 7:
 *   - translation fault di VA yang terdaftar sebagai object-backed
 *     (vm_map_object) -> minta halaman ke pager eksternal via IPC
 *     (data_request/data_supply), map dengan prot tercatat, retry.
 *     Gagalnya pager = genuine fault (tidak di-zero-fill diam-diam).
 *   - permission fault (section 0xD / page 0xF) di VA yang terdaftar
 *     di tabel COW -> cow_break (salin privat + remap RW), retry.
 * Fault lain (alignment dsb.) tidak diselesaikan.
 *
 * CATATAN DEADLOCK (pelajaran Fase 6): abort handler masuk dengan IRQ
 * ter-mask (hardware ARMv7 men-set I=1 saat exception). pager_fetch_page
 * memanggil ipc_rpc yang blocking; ipc_recv Fase 6 melakukan cpsie i
 * sebelum spin dan me-restore mask setelah bangun, sehingga tick timer
 * tetap bisa menjadwalkan pager thread. Jangan spin dengan IRQ mati
 * di jalur ini.
 *
 * Keterbatasan: abort handler tidak reentrant antar thread (stack abort
 * global). Selama satu thread ter-block di dalam pager, thread lain
 * tidak boleh fault - pager thread di tes ini tidak menyentuh VA yang
 * bisa fault.
 */
int vm_page_fault(uint32_t far, uint32_t fsr)
{
    unsigned fs;
    uint32_t pa, va;
    struct vm_space *sp;

    /* ARMv7 short-descriptor FS: bits[3:0] + bit[10] -> FS[4]. */
    fs = (fsr & 0xFu) | ((fsr >> 6) & 0x10u);
    sp = vm_cur_space;
    if (!sp)
        return 0;
    va = far & ~VM_PAGE_MASK;

    if (fs == 0x5u || fs == 0x7u) {
        /* Translation fault: VA belum ter-map. */
        struct task *t;
        struct vm_object *o;
        unsigned oid, prot;
        uint32_t off;

        if (far < VM_DEMAND_BASE || far >= VM_DEMAND_END)
            return 0;           /* di luar demand range */
        if (vm_lookup(sp, va) != 0u)
            return 0;           /* sudah ter-map: aneh, jangan loop */

        /* Fase 7: object-backed? Minta isi halaman ke pager. */
        t = sched_current_task();
        if (t && pager_lookup(t, va, &o, &oid, &off, &prot)) {
            pa = vm_page_alloc();
            if (pa == 0u)
                return 0;
            if (pager_fetch_page(o, oid, off, (uint8_t *)pa) != 0)
                return 0;       /* pager gagal: genuine fault */
            if (vm_map(sp, va, pa, prot) != 0)
                return 0;
            /* vm_map hanya DSB: paksa entry terlihat walker sekarang. */
            vm_tlbimva(va);
            vm_dsb();
            vm_isb();
            return 1;
        }

        /* Bukan object-backed: zero-fill seperti Fase 5. */
        pa = vm_page_alloc();
        if (pa == 0u)
            return 0;           /* out of pages */
        if (vm_map(sp, va, pa, VM_PROT_READ | VM_PROT_WRITE) != 0)
            return 0;
        vm_tlbimva(va);
        vm_dsb();
        vm_isb();
        return 1;
    }

    if (fs == 0xDu || fs == 0xFu) {
        /* Permission fault: mungkin write ke halaman COW. */
        return cow_break(sp, va);
    }

    return 0;
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
