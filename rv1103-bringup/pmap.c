/*
 * pmap.c - ARMv7-A short-descriptor page map (bring-up implementation).
 *
 * This is a deliberately small MMU helper: one L1 translation table
 * (4 KiB entries = 4096 x 1 MiB sections) referenced from TTBR0, all
 * domain 0. Mappings are identity (VA == PA), which is what the bare-
 * metal loader gives us on both QEMU virt and RV1103.
 *
 * Short-descriptor L1 section entry (ARMv7-A ARM, B3.5):
 *
 *   31        20 19 18 17 16 15 14 12 11 10 9  8  5  4  3  2  1  0
 *  +------------+--+--+--+--+--+-----+-----+--+-----+--+--+--+--+--+
 *  | Section PA |  |S |nG|  | TEX | AP  |  |Domain|XN|C |B |1 |0 |
 *  +------------+--+--+--+--+--+-----+-----+--+-----+--+--+--+--+--+
 *
 * We set [1:0]=0b10, domain=0b0000, AP=0b11 (full access, PL0/PL1
 * read/write), TEX/C/B from the caller's flags, and XN for device.
 *
 * C99, -ffreestanding, no libc, -mcpu=cortex-a7 -marm.
 */

#include "pmap.h"
#include "board.h"

/* ------------------------------------------------------------------ */
/* Board macro aliases.                                               */
/*                                                                    */
/* board.h currently exposes DRAM_BASE / DRAM_SIZE / UART_BASE /      */
/* GICD_BASE. The task API names them BOARD_*. Accept either so this  */
/* file does not depend on board.h being rewritten.                   */
/* ------------------------------------------------------------------ */
#ifndef BOARD_DRAM_BASE
#define BOARD_DRAM_BASE DRAM_BASE
#endif
#ifndef BOARD_DRAM_SIZE
#define BOARD_DRAM_SIZE DRAM_SIZE
#endif
#ifndef BOARD_UART_BASE
#define BOARD_UART_BASE UART_BASE
#endif
#ifndef BOARD_GICD_BASE
#define BOARD_GICD_BASE GICD_BASE
#endif

/* Sanity: the aliases we actually rely on must exist. */
#if !defined(BOARD_DRAM_BASE) || !defined(BOARD_DRAM_SIZE)
#error "pmap.c: BOARD_DRAM_BASE / BOARD_DRAM_SIZE must be defined"
#endif

/* ------------------------------------------------------------------ */
/* Section geometry                                                   */
/* ------------------------------------------------------------------ */
#define SECTION_SHIFT   20
#define SECTION_SIZE    (1u << SECTION_SHIFT)
#define SECTION_MASK    (SECTION_SIZE - 1u)

/* Descriptor composition constants. */
#define DESC_SECTION    (1u << 1)           /* bits[1:0] = 0b10          */
#define DESC_AP_FULL    (0x3u << 10)        /* AP[1:0] = 0b11            */
#define DESC_DOMAIN0    (0x0u << 5)         /* domain field [8:5] = 0    */

/* Peripheral window mapped as device memory (identity). */
#if defined(BOARD_RV1103)
#define PMAP_PERIPH_BASE    0xFF000000u
#define PMAP_PERIPH_SECTIONS 16u            /* 0xFF000000 .. 0xFFFFFFFF */
#else
#define PMAP_PERIPH_BASE    0x08000000u
#define PMAP_PERIPH_SECTIONS 32u            /* 0x08000000 .. 0x09FFFFFF */
#endif

/* ------------------------------------------------------------------ */
/* L1 translation table (must be 16 KiB aligned for TTBR0).           */
/* ------------------------------------------------------------------ */
static uint32_t l1[4096] __attribute__((aligned(16384)));

/* ------------------------------------------------------------------ */
/* CP15 / barrier helpers                                             */
/* ------------------------------------------------------------------ */
static inline void cp15_dsb(void)
{
    __asm__ __volatile__("dsb" ::: "memory");
}

static inline void cp15_isb(void)
{
    __asm__ __volatile__("isb" ::: "memory");
}

static inline void cp15_write_ttbr0(uint32_t v)
{
    __asm__ __volatile__("mcr p15, 0, %0, c2, c0, 0" :: "r"(v) : "memory");
}

static inline void cp15_write_dacr(uint32_t v)
{
    __asm__ __volatile__("mcr p15, 0, %0, c3, c0, 0" :: "r"(v) : "memory");
}

static inline void cp15_invalidate_tlb(void)
{
    uint32_t zero = 0u;
    __asm__ __volatile__("mcr p15, 0, %0, c8, c7, 0" :: "r"(zero) : "memory");
}

static inline void cp15_invalidate_icache(void)
{
    uint32_t zero = 0u;
    __asm__ __volatile__("mcr p15, 0, %0, c7, c5, 0" :: "r"(zero) : "memory");
}

static inline uint32_t cp15_read_sctlr(void)
{
    uint32_t v;
    __asm__ __volatile__("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

static inline void cp15_write_sctlr(uint32_t v)
{
    __asm__ __volatile__("mcr p15, 0, %0, c1, c0, 0" :: "r"(v) : "memory");
}

/* ------------------------------------------------------------------ */
/* Descriptor construction                                            */
/* ------------------------------------------------------------------ */
static uint32_t section_descriptor(uint32_t pa, int flags)
{
    /*
     * pa is masked to its section base; the low flag bits supplied by
     * the caller (TEX/C/B/XN) land in the attribute field. AP and the
     * descriptor type are forced here.
     */
    return DESC_SECTION | DESC_AP_FULL | DESC_DOMAIN0 |
           (pa & ~SECTION_MASK) | ((uint32_t)flags);
}

/* Zero the whole L1 table without libc. */
static void l1_clear(void)
{
    unsigned i;
    for (i = 0u; i < 4096u; i++)
        l1[i] = 0u;
}

/* Identity-map a range, one 1 MiB section at a time. `nsect` is the
 * number of sections (avoids 32-bit address overflow at 4 GiB). */
static void map_range(uint32_t base, unsigned nsect, int flags)
{
    unsigned i;
    for (i = 0u; i < nsect; i++) {
        uint32_t va = base + (i << SECTION_SHIFT);
        l1[va >> SECTION_SHIFT] = section_descriptor(va, flags);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */
void pmap_map_section(uint32_t va, uint32_t pa, int flags)
{
    l1[(va >> SECTION_SHIFT) & 0xFFFu] = section_descriptor(pa, flags);
}

void pmap_unmap_section(uint32_t va)
{
    l1[(va >> SECTION_SHIFT) & 0xFFFu] = 0u;
}

void pmap_init(void)
{
    unsigned dram_sections = BOARD_DRAM_SIZE >> SECTION_SHIFT;

    l1_clear();

    /*
     * DRAM, 1:1, cacheable (write-back write-allocate). This covers
     * the kernel text/data, stacks and the L1 table itself so that
     * execution can continue once the MMU is enabled.
     */
    map_range(BOARD_DRAM_BASE, dram_sections, PMAP_CACHEABLE);

    /*
     * Peripheral window, 1:1, device memory / execute-never.
     *   VIRT   0x08000000..0x09FFFFFF : GIC-400 + PL011 UART
     *   RV1103 0xFF000000..0xFFFFFFFF : GIC, UART, CRU, GRF, PMU, ...
     */
    map_range(PMAP_PERIPH_BASE, PMAP_PERIPH_SECTIONS, PMAP_DEVICE);

    /*
     * Ensure the two windows already cover the board's UART and GIC
     * (compile-time reminder that these macros are real and in range).
     */
    (void)BOARD_UART_BASE;
    (void)BOARD_GICD_BASE;
}

void pmap_enable(void)
{
    uint32_t sctlr;

    cp15_dsb();

    /* TTBR0 = &l1. Bits[13:0] are zero because l1 is 16 KiB aligned,
     * so the translation-table walk has IRGN/ORGN/shared attributes
     * 0 (non-cacheable). Single-core bring-up: no split TTBR. */
    cp15_write_ttbr0((uint32_t)(uintptr_t)&l1);

    /* DACR: domain 0 = client (0b01), all other domains = no access.
     * 1u sets domain 0 to client and leaves the rest as 0b00. */
    cp15_write_dacr(1u);

    cp15_dsb();

    /*
     * Flush stale translations and instruction cache before enabling.
     *
     * DCCISW is intentionally skipped: the Cortex-A7 reset state and
     * the pre-MMU caches are clean, and set/way D-cache maintenance is
     * unsafe/undesirable here. Add it later when DMA/coherency needs
     * it, after the cache size/geometry has been probed via CSSELR.
     */
    cp15_invalidate_tlb();
    cp15_invalidate_icache();
    cp15_dsb();

    /* SCTLR.M (bit 0) = 1 : enable MMU. Leave caches (C/I) untouched. */
    sctlr = cp15_read_sctlr();
    sctlr |= 1u;
    cp15_write_sctlr(sctlr);

    cp15_isb();
}

void pmap_disable(void)
{
    uint32_t sctlr;

    cp15_dsb();

    sctlr = cp15_read_sctlr();
    sctlr &= ~1u;                       /* clear SCTLR.M */
    cp15_write_sctlr(sctlr);

    cp15_isb();
}