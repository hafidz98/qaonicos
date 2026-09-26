/*
 * irq.h - API dispatch interrupt ARMv7-A untuk RV1103 (Cortex-A7).
 *
 * Skema: tabel vektor di vectors.S (section .vectors) menangani trap CPU,
 * lalu entry IRQ memanggil c_irq_handler() yang meng-ack GIC, mendispatch
 * ke handler terdaftar, dan meng-EOI.
 */

#ifndef RV1103_IRQ_H
#define RV1103_IRQ_H

/* Inisialisasi: kosongkan tabel handler dan tulis alamat vector table ke
 * VBAR (mcr p15, 0, <addr>, c12, c0, 0). Vektor ada di simbol _vectors. */
void irq_init(void);

/* Handler level-C yang dipanggil dari stub assembly irq_handler.
 * Meng-ack GIC, memanggil handler terdaftar, lalu EOI. */
void c_irq_handler(void);

/* Daftarkan handler untuk nomor interrupt n. n di luar rentang tabel
 * (0..159) diabaikan. fn NULL mengosongkan entry. */
void irq_register(unsigned n, void (*fn)(void));

#endif /* RV1103_IRQ_H */