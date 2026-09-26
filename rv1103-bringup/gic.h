/*
 * gic.h - Driver GIC-400 minimal untuk Rockchip RV1103 (Cortex-A7).
 *
 * Target: bare-metal / -ffreestanding, C99, tanpa libc.
 * Alamat diambil dari hw-addrs.md (rv1106.dtsi, compatible "arm,gic-400"):
 *   GICD : 0xff1f1000, size 0x1000
 *   GICC : 0xff1f2000, size 0x2000
 */
#ifndef RV1103_GIC_H
#define RV1103_GIC_H

#include <stdint.h>

/* Base register blok GIC-400 (RV1103). */
#define GICD_BASE   0xFF1F1000u
#define GICC_BASE   0xFF1F2000u

/* Jumlah interrupt ID yang didukung GICv2 (ID 0..1019). */
#define GIC_MAX_IRQ 1020u

/* ID interrupt palsu (spurious) >= 1020; jangan di-EOI. */
#define GIC_SPURIOUS_IRQ 0x3FFu

/*
 * gic_init - inisialisasi distributor + CPU interface.
 *
 * Langkah:
 *   1. Disable distributor.
 *   2. Semua SPI -> group 0.
 *   3. Priority default untuk semua SPI.
 *   4. Target semua SPI ke CPU0.
 *   5. Clear pending semua SPI.
 *   6. Enable distributor (Group 0).
 *   7. Init CPU interface: GICC_PMR = 0xff, GICC_CTLR enable (Group 0).
 */
void gic_init(void);

/* Enable interrupt ID n (SGI/PPI maupun SPI) di distributor. */
void gic_enable_irq(unsigned n);

/* Disable interrupt ID n di distributor. */
void gic_disable_irq(unsigned n);

/* Acknowledge: baca GICC_IAR dan kembalikan ID interrupt (mask 0x3ff). */
unsigned gic_ack(void);

/* End of interrupt: tulis GICC_EOIR dengan ID interrupt. */
void gic_eoi(unsigned n);

#endif /* RV1103_GIC_H */