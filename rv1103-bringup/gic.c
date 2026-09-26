/*
 * gic.c - Driver GIC-400 minimal untuk Rockchip RV1103 (Cortex-A7).
 *
 * C99, -ffreestanding, tanpa libc. Semua akses register memakai
 * pointer volatile; sinkronisasi memakai inline asm "dsb ish".
 */
#include "gic.h"

/* ------------------------------------------------------------------ */
/* Offset register GIC-400 (standar ARM)                               */
/* ------------------------------------------------------------------ */
#define GICD_CTLR        0x000u  /* Distributor Control                 */
#define GICD_TYPER       0x004u  /* Interrupt Controller Type           */
#define GICD_IGROUPRn    0x080u  /* Interrupt Group                     */
#define GICD_ISENABLERn  0x100u  /* Interrupt Set-Enable                */
#define GICD_ICENABLERn  0x180u  /* Interrupt Clear-Enable              */
#define GICD_ICPENDRn    0x280u  /* Interrupt Clear-Pending             */
#define GICD_IPRIORITYRn 0x400u  /* Interrupt Priority                  */
#define GICD_ITARGETSRn  0x800u  /* Interrupt Processor Targets         */
#define GICD_ICFGRn      0xC00u  /* Interrupt Configuration             */

#define GICC_CTLR        0x00u   /* CPU Interface Control               */
#define GICC_PMR         0x04u   /* Priority Mask                       */
#define GICC_IAR         0x0Cu   /* Interrupt Acknowledge               */
#define GICC_EOIR        0x10u   /* End of Interrupt                    */

/* ------------------------------------------------------------------ */
/* Helper akses register + barrier                                     */
/* ------------------------------------------------------------------ */
#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG8(addr)  (*(volatile uint8_t  *)(addr))

/* Data Synchronization Barrier, inner shareable, + clobber memory. */
static inline void gic_dsb(void)
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

/* Data Memory Barrier, inner shareable, + clobber memory. */
static inline void gic_dmb(void)
{
    __asm__ __volatile__("dmb ish" ::: "memory");
}

/* Jumlah word 32-bit yang menutupi seluruh ID interrupt yang didukung. */
#define GICD_NR_WORDS (((GIC_MAX_IRQ) + 31u) / 32u)

/* ------------------------------------------------------------------ */
/* Inisialisasi                                                        */
/* ------------------------------------------------------------------ */
void gic_init(void)
{
    unsigned i;
    unsigned n;

    /* 1. Disable distributor sebelum dikonfigurasi. */
    REG32(GICD_BASE + GICD_CTLR) = 0u;
    gic_dsb();

    /*
     * 2. Semua SPI -> group 0.
     *    IGROUPR0 (SGI/PPI) tidak disentuh karena bisa read-only/banked;
     *    cukup mulai dari word indeks 1 (ID 32 ke atas).
     */
    for (i = 1u; i < GICD_NR_WORDS; i++) {
        REG32(GICD_BASE + GICD_IGROUPRn + (i * 4u)) = 0u;
    }

    /*
     * 3. Priority default untuk semua SPI.
     * 4. Target semua SPI ke CPU0.
     *    IPRIORITYR dan ITARGETSR punya satu byte per interrupt, jadi
     *    alamat byte untuk ID n adalah base + n.
     */
    for (n = 32u; n < GIC_MAX_IRQ; n++) {
        REG8(GICD_BASE + GICD_IPRIORITYRn + n) = 0xA0u; /* default priority */
        REG8(GICD_BASE + GICD_ITARGETSRn + n) = 0x01u; /* CPU0             */
    }

    /* 5. Clear pending untuk seluruh SPI (write-1-to-clear). */
    for (i = 1u; i < GICD_NR_WORDS; i++) {
        REG32(GICD_BASE + GICD_ICPENDRn + (i * 4u)) = 0xFFFFFFFFu;
    }

    /* 6. Enable distributor (EnableGrp0 = bit 0). */
    gic_dsb();
    REG32(GICD_BASE + GICD_CTLR) = 1u;
    gic_dsb();

    /*
     * 7. Init CPU interface.
     *    PMR = 0xff supaya semua prioritas lolos, lalu enable Group 0.
     *    Urutan penting: set PMR dulu, baru enable GICC_CTLR.
     */
    REG32(GICC_BASE + GICC_PMR) = 0xFFu;
    gic_dsb();
    REG32(GICC_BASE + GICC_CTLR) = 1u; /* EnableGrp0 */
    gic_dsb();
}

/* ------------------------------------------------------------------ */
/* Enable / disable interrupt                                          */
/* ------------------------------------------------------------------ */
void gic_enable_irq(unsigned n)
{
    if (n >= GIC_MAX_IRQ)
        return;

    gic_dsb(); /* pastikan konfigurasi sebelumnya terlihat */
    REG32(GICD_BASE + GICD_ISENABLERn + ((n / 32u) * 4u)) =
        (1u << (n % 32u)); /* write-1-to-set */
    gic_dsb();
}

void gic_disable_irq(unsigned n)
{
    if (n >= GIC_MAX_IRQ)
        return;

    gic_dsb();
    REG32(GICD_BASE + GICD_ICENABLERn + ((n / 32u) * 4u)) =
        (1u << (n % 32u)); /* write-1-to-clear */
    gic_dsb();
}

/* ------------------------------------------------------------------ */
/* Acknowledge / End Of Interrupt                                      */
/* ------------------------------------------------------------------ */
unsigned gic_ack(void)
{
    unsigned iar;

    gic_dmb(); /* pastikan state distributor terlihat sebelum membaca IAR */
    iar = REG32(GICC_BASE + GICC_IAR);
    gic_dmb();

    return iar & 0x3FFu;
}

void gic_eoi(unsigned n)
{
    /* ID >= 1020 adalah spurious; tidak boleh ditulis ke EOIR. */
    if ((n & 0x3FFu) >= GIC_MAX_IRQ)
        return;

    gic_dsb();
    REG32(GICC_BASE + GICC_EOIR) = (n & 0x3FFu);
    gic_dsb();
}