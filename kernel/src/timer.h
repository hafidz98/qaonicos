/*
 * timer.h - ARMv7 Generic Timer driver untuk RV1103 (Cortex-A7).
 *
 * RV1103 TIDAK punya timer MMIO terpisah (lihat hw-addrs.md). Satu-satunya
 * timer adalah ARM generic timer (compatible "arm,armv7-timer") yang diakses
 * lewat system registers CP15 dan di-deliver sebagai interrupt PPI.
 *
 * Register yang dipakai:
 *   CNTFRQ    : frekuensi counter (Hz), read-only.
 *   CNTVCT    : virtual counter, 64-bit, read via MRRC.
 *   CNTV_TVAL : virtual timer value/tick-down reload.
 *   CNTV_CTL  : virtual timer control (ENABLE/IMASK/ISTATUS).
 *
 * Catatan PPI: device tree rv1106.dtsi mencantumkan GIC_PPI 13/14 untuk
 * armv7-timer. Target bring-up ini memakai PPI 27 (lihat spesifikasi T3).
 */

#ifndef RV1103_TIMER_H
#define RV1103_TIMER_H

#include <stdint.h>

/* Nomor interrupt PPI untuk timer pada target ini. */
#define TIMER_PPI_IRQ 27u

/* Bit CNTV_CTL / CNTP_CTL. */
#define TIMER_CTL_ENABLE  (1u << 0) /* timer aktif            */
#define TIMER_CTL_IMASK   (1u << 1) /* 1 = interrupt di-mask  */
#define TIMER_CTL_ISTATUS (1u << 2) /* 1 = interrupt pending  */

/* Inisialisasi: baca CNTFRQ, simpan, aktifkan virtual timer. */
void timer_init(void);

/* Busy-wait selama us mikrodetik memakai CNTVCT. */
void timer_delay_us(uint32_t us);

/*
 * Program virtual timer agar interrupt berbunyi setiap us mikrodetik.
 * Set CNTV_TVAL dan unmask interrupt pada CNTV_CTL.
 */
void timer_irq_every_us(uint32_t us);

/* Frekuensi counter (Hz) hasil pembacaan CNTFRQ. */
uint32_t timer_get_freq(void);

/* Baca CNTVCT 64-bit (ekspos untuk pengujian/debug). */
uint64_t timer_read_cntvct(void);

/* Tulis CNTV_TVAL langsung (mikrodetik dihitung oleh pemanggil -> ticks). */
void timer_set_tval(uint32_t ticks);

#endif /* RV1103_TIMER_H */