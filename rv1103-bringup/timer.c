/*
 * timer.c - ARMv7 Generic Timer driver untuk RV1103 (Cortex-A7), C99,
 *           -ffreestanding, tanpa libc.
 *
 * Semua akses register lewat inline asm CP15. Tidak ada MMIO.
 */

#include "timer.h"

/* Nilai kontrol COP15 untuk CP15 c14 (generic timer). */
#define CNTFRQ_OPC2   0u
#define CNTV_CTL_OPC2 1u
#define CNTV_TVAL_OPC2 0u
#define CNTVCT_OP1    1u

/* Frekuensi counter, diisi timer_init() dari CNTFRQ. */
static uint32_t timer_freq;

/* ------------------------------------------------------------------ */
/* Helper inline asm CP15                                              */
/* ------------------------------------------------------------------ */

/* CNTFRQ: mrc p15, 0, r0, c14, c0, 0 */
static inline uint32_t cp15_read_cntfrq(void)
{
    uint32_t v;
    __asm__ volatile ("mrc p15, 0, %0, c14, c0, 0" : "=r" (v));
    return v;
}

/* CNTV_CTL: mrc p15, 0, r0, c14, c3, 1 */
static inline uint32_t cp15_read_cntv_ctl(void)
{
    uint32_t v;
    __asm__ volatile ("mrc p15, 0, %0, c14, c3, 1" : "=r" (v));
    return v;
}

/* CNTV_CTL: mcr p15, 0, r0, c14, c3, 1 */
static inline void cp15_write_cntv_ctl(uint32_t v)
{
    __asm__ volatile ("mcr p15, 0, %0, c14, c3, 1" :: "r" (v));
}

/* CNTV_TVAL: mcr p15, 0, r0, c14, c3, 0 */
static inline void cp15_write_cntv_tval(uint32_t v)
{
    __asm__ volatile ("mcr p15, 0, %0, c14, c3, 0" :: "r" (v));
}

/* CNTVCT: mrrc p15, 1, r0, r1, c14  -> low di r0, high di r1 */
static inline uint64_t cp15_read_cntvct(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("mrrc p15, 1, %0, %1, c14"
                      : "=r" (lo), "=r" (hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ------------------------------------------------------------------ */
/* Konversi mikrodetik -> ticks                                        */
/* ------------------------------------------------------------------ */

/*
 * ticks = us * freq / 1e6.
 * Frekuensi dipisah menjadi bagian bulat + sisa agar tidak perlu
 * pembagian 64-bit penuh dan tetap akurat untuk frekuensi non-bulat
 * (mis. 24 MHz tetap 24 ticks/us).
 */
static uint64_t us_to_ticks(uint32_t us)
{
    uint32_t tpu = timer_freq / 1000000u;   /* ticks per us */
    uint32_t rem = timer_freq % 1000000u;   /* sisa Hz      */
    uint64_t ticks = (uint64_t)tpu * (uint64_t)us;
    uint64_t frac = (uint64_t)rem * (uint64_t)us;

    if (rem != 0u)
        ticks += frac / 1000000u;

    return ticks;
}

/* ------------------------------------------------------------------ */
/* API publik                                                          */
/* ------------------------------------------------------------------ */

void timer_init(void)
{
    uint32_t ctl;

    timer_freq = cp15_read_cntfrq();

    /*
     * Aktifkan virtual timer (ENABLE) tetapi masih mask interrupt
     * supaya belum ada IRQ sebelum timer_irq_every_us() dipanggil.
     */
    ctl = cp15_read_cntv_ctl();
    ctl |= TIMER_CTL_ENABLE;
    ctl |= TIMER_CTL_IMASK;
    cp15_write_cntv_ctl(ctl);
}

void timer_delay_us(uint32_t us)
{
    uint64_t start;
    uint64_t delta;

    if (us == 0u || timer_freq == 0u)
        return;

    start = cp15_read_cntvct();
    delta = us_to_ticks(us);

    /* Aritmetika 64-bit dengan wraparound alami. */
    while ((cp15_read_cntvct() - start) < delta)
        ;
}

void timer_irq_every_us(uint32_t us)
{
    uint32_t ctl;

    if (timer_freq == 0u)
        return;

    /* Muat periode; TVAL menghitung mundur dari nilai ini. */
    cp15_write_cntv_tval((uint32_t)us_to_ticks(us));

    /* Unmask interrupt dan pastikan timer aktif. */
    ctl = cp15_read_cntv_ctl();
    ctl |= TIMER_CTL_ENABLE;
    ctl &= ~TIMER_CTL_IMASK;
    cp15_write_cntv_ctl(ctl);
}

uint32_t timer_get_freq(void)
{
    return timer_freq;
}

uint64_t timer_read_cntvct(void)
{
    return cp15_read_cntvct();
}

void timer_set_tval(uint32_t ticks)
{
    cp15_write_cntv_tval(ticks);
}