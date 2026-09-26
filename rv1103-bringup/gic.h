/*
 * gic.h - Driver GIC-400 minimal (Cortex-A7).
 *
 * Board addresses come from board.h (-DBOARD_VIRT for QEMU virt,
 * -DBOARD_RV1103 for the Luckfox Pico Mini).
 *
 * Target: bare-metal / -ffreestanding, C99, tanpa libc.
 */
#ifndef MACH_GIC_H
#define MACH_GIC_H

#include <stdint.h>
#include "board.h"

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

/* Set priority byte untuk interrupt ID n. */
void gic_set_priority(unsigned n, uint8_t prio);

/* Konfigurasi interrupt ID n sebagai level-sensitive (bukan edge). */
void gic_set_level(unsigned n);

/* Acknowledge: baca GICC_IAR dan kembalikan ID interrupt (mask 0x3ff). */
unsigned gic_ack(void);

/* End of interrupt: tulis GICC_EOIR dengan ID interrupt. */
void gic_eoi(unsigned n);

#endif /* MACH_GIC_H */
