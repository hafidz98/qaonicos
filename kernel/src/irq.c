/*
 * irq.c - Dispatch interrupt ARMv7-A untuk RV1103 (Cortex-A7), C99,
 *         -ffreestanding, tanpa libc.
 *
 * c_irq_handler() dipanggil oleh stub assembly irq_handler (vectors.S).
 * Fungsi ini:
 *   1. gic_ack()        -> ambil nomor interrupt yang aktif
 *   2. dispatch         -> panggil handler terdaftar untuk nomor tsb
 *   3. gic_eoi(n)       -> tandai selesai di GIC
 *
 * GIC-400 (gic.h/gic.c) adalah komponen terpisah (T2) dan diasumsikan
 * sudah menyediakan gic_ack()/gic_eoi(); di sini cukup #include "gic.h".
 */

#include "irq.h"
#include "gic.h"

/* Simbol tabel vektor dari vectors.S (section .vectors, 32-byte aligned). */
extern char _vectors[];

/* Jumlah entry tabel dispatch. */
#define IRQ_TABLE_SIZE 160u

/* Nomor interrupt di luar tabel (mis. GIC spurious 1023) diabaikan,
 * tetapi tetap di-EOI. */
static void (*irq_table[IRQ_TABLE_SIZE])(void);

void irq_init(void)
{
    unsigned i;

    /* Kosongkan tabel dispatch (tanpa memset/libc). */
    for (i = 0u; i < IRQ_TABLE_SIZE; i++)
        irq_table[i] = 0;

    /*
     * Tulis alamat vector table ke VBAR:
     *   mcr p15, 0, <Rt>, c12, c0, 0
     * Diikuti ISB agar perubahan langsung berlaku pada pipeline.
     */
    __asm__ volatile ("mcr p15, 0, %0, c12, c0, 0"
                      :: "r" (_vectors) : "memory");
    __asm__ volatile ("isb" ::: "memory");
}

void irq_register(unsigned n, void (*fn)(void))
{
    if (n < IRQ_TABLE_SIZE)
        irq_table[n] = fn;
}

void c_irq_handler(void)
{
    unsigned n = gic_ack();

    /* Nomor di luar tabel = default/spurious: abaikan. */
    if (n < IRQ_TABLE_SIZE && irq_table[n] != 0)
        irq_table[n]();

    gic_eoi(n);
}