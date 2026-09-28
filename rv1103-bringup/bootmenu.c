/*
 * bootmenu.c - Fase 13: boot menu via UART (QEMU virt PL011).
 *
 * Dipanggil paling awal dari kernel_main(), sebelum pmap_init().
 * Semua I/O polled; sumber waktu = ARM generic counter (CNTVCT),
 * yang selalu jalan di QEMU virt tanpa perlu timer_init().
 *
 * Tidak memakai BSS/global: semua state di stack (locals), jadi aman
 * dipanggil kapan pun setelah start.S menyiapkan stack SVC.
 */
#include "bootmenu.h"

#include <stdint.h>

/* --- PL011 (QEMU virt UART0) -------------------------------------- */
#define PL011_BASE 0x09000000u
#define PL011_DR   (*(volatile uint32_t *)(PL011_BASE + 0x00u))
#define PL011_FR   (*(volatile uint32_t *)(PL011_BASE + 0x18u))
#define PL011_FR_TXFF (1u << 5) /* transmit FIFO full  */
#define PL011_FR_RXFE (1u << 4) /* receive FIFO empty  */

static void bm_putc(char c)
{
    while ((PL011_FR & PL011_FR_TXFF) != 0u) {
        /* busy-wait */
    }
    PL011_DR = (uint32_t)(unsigned char)c;
    if (c == '\n') {
        bm_putc('\r');
    }
}

static void bm_puts(const char *s)
{
    while (*s != '\0') {
        bm_putc(*s);
        ++s;
    }
}

static void bm_putdec(unsigned v)
{
    char buf[12];
    int i = 0;

    if (v == 0u) {
        bm_putc('0');
        return;
    }
    while (v > 0u) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        bm_putc(buf[--i]);
    }
}

/* Kembalikan byte berikutnya bila ada, atau -1 bila FIFO RX kosong. */
static int bm_getc_nowait(void)
{
    if ((PL011_FR & PL011_FR_RXFE) != 0u) {
        return -1;
    }
    return (int)(PL011_DR & 0xFFu);
}

/* --- ARM generic counter (tanpa timer_init) ------------------------ */

/* CNTFRQ: mrc p15, 0, r0, c14, c0, 0 */
static inline uint32_t bm_cntfrq(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
    return v;
}

/* CNTVCT: mrrc p15, 1, r0, r1, c14  (low di r0, high di r1).
 * Boleh dibaca dari SVC/PL1 tanpa setup CNTKCTL. */
static inline uint64_t bm_cntvct(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* --- Menu ---------------------------------------------------------- */

int bootmenu_run(void)
{
    uint32_t freq;
    uint64_t deadline, now, next_dot;
    int mode = BOOTMODE_NORMAL;

    bm_puts("\n");
    bm_puts("========================================\n");
    bm_puts("  QaonicOS boot menu\n");
    bm_puts("========================================\n");
    bm_puts("  1) Boot QaonicOS\n");
    bm_puts("  2) Boot QaonicOS + extended self-test\n");
    bm_puts("----------------------------------------\n");
    bm_puts("Pilih [1/2] (auto-boot 1 dalam ");
    bm_putdec(BOOTMENU_TIMEOUT_S);
    bm_puts(" dtk): ");

    freq = bm_cntfrq();
    if (freq == 0u) {
        freq = 62500000u; /* fallback: frekuensi virt QEMU */
    }
    deadline = bm_cntvct() + (uint64_t)freq * (uint64_t)BOOTMENU_TIMEOUT_S;
    next_dot = bm_cntvct() + (uint64_t)freq / 2u; /* titik tiap 0.5 dtk */

    for (;;) {
        int c = bm_getc_nowait();

        if (c == '1' || c == '\r' || c == '\n') {
            bm_putc((char)c);
            bm_puts("\n");
            mode = BOOTMODE_NORMAL;
            break;
        }
        if (c == '2') {
            bm_putc((char)c);
            bm_puts("\n");
            mode = BOOTMODE_SELFTEST;
            break;
        }
        /* Tombol lain diabaikan (tetap menunggu/timeout). */

        now = bm_cntvct();
        if (now >= next_dot && now < deadline) {
            bm_putc('.');
            next_dot = now + (uint64_t)freq / 2u;
        }
        if (now >= deadline) {
            bm_puts("\n(timeout) ");
            mode = BOOTMODE_NORMAL;
            break;
        }
    }

    if (mode == BOOTMODE_SELFTEST) {
        bm_puts("Booting: QaonicOS + extended self-test\n");
    } else {
        bm_puts("Booting: QaonicOS\n");
    }
    return mode;
}
