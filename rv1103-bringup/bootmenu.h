/*
 * bootmenu.h - Fase 13: boot menu via UART.
 *
 * Menampilkan menu pilihan boot sebelum kernel jalan penuh, dengan
 * timeout auto-boot. Aman dipanggil di awal kernel_main(), sebelum
 * pmap_init() (MMU off, pemetaan 1:1).
 *
 * Diuji di: qemu-system-arm -M virt -cpu cortex-a7 (console PL011
 * UART0 @ 0x09000000). Di hardware RV1103 asli, ganti backend
 * konsol ke uart.c polled (UART2 0xff4c0000).
 *
 * C99, freestanding, no libc.
 */
#ifndef RV1103_BOOTMENU_H
#define RV1103_BOOTMENU_H

/* Hasil pilihan boot. */
#define BOOTMODE_NORMAL   0  /* Boot QaonicOS (default, dipakai saat timeout). */
#define BOOTMODE_SELFTEST 1  /* Boot + extended self-test. */

/* Detik sebelum auto-boot opsi default. */
#define BOOTMENU_TIMEOUT_S 3u

/*
 * bootmenu_run - tampilkan menu, tunggu tombol 1/2 sampai timeout,
 * kembalikan BOOTMODE_NORMAL atau BOOTMODE_SELFTEST.
 *
 * Tombol: '1' = boot normal, '2' = boot + self-test, Enter = default.
 * Polled UART; tidak menyentuh GIC/timer (aman sebelum init).
 */
int bootmenu_run(void);

#endif /* RV1103_BOOTMENU_H */
