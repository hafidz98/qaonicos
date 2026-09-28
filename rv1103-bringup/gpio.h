/*
 * gpio.h - Driver GPIO QaonicOS (Fase 14).
 *
 * Dua backend, dipilih via board.h:
 *  - BOARD_RV1103: register ASLI Rockchip (compatible "rockchip,gpio-bank"
 *    di rv1106.dtsi, diwarisi rv1103.dtsi). 32 pin per bank:
 *      GPIO0 @ 0xff380000, GPIO1 @ 0xff530000, GPIO2 @ 0xff540000,
 *      GPIO3 @ 0xff550000, GPIO4 @ 0xff560000
 *    Layout register per bank (sesuai Linux drivers/gpio/gpio-rockchip.c):
 *      SW_PORTx_DR  = base + port*8        (x = A..D, port = pin/8)
 *      SW_PORTx_DDR = base + port*8 + 4     (0 = input, 1 = output)
 *      EXT_PORTx    = base + 0x50 + port*4  (baca level pin)
 *  - BOARD_VIRT: "mock" GPIO virtual untuk QEMU -M virt. board.h
 *    mendefinisikan range MMIO palsu GPIO_VIRT_BASE (0x0a100000, tidak
 *    terpakai di peta memori QEMU virt). AKSES MMIO FISIK TIDAK
 *    DILAKUKAN: alamat itu unmapped di QEMU dan akan data-abort.
 *    Sebagai gantinya register file di-RAM-kan dengan OFFSET YANG SAMA
 *    PERSIS seperti Rockchip, sehingga logika offset driver identik di
 *    kedua backend dan jalur driver->syscall->userspace bisa dites penuh
 *    di QEMU.
 *
 * CATATAN RV1103: di hardware asli, clock gate GPIO (CRU) harus dibuka
 * sebelum register disentuh (biasanya sudah oleh bootloader). Itu di
 * luar cakupan Fase 14; backend RV1103 wajib lolos compile clang saja.
 *
 * C99, freestanding, no libc.
 */
#ifndef QAONIC_GPIO_H
#define QAONIC_GPIO_H

#include "board.h"

/* Jumlah pin per bank (Rockchip: 4 port x 8 pin). */
#define GPIO_PINS_PER_BANK 32u

/*
 * gpio_init - siapkan driver. Di BOARD_VIRT menolkan register file
 * (BSS sudah nol; eksplisit biar jelas). Di BOARD_RV1103 tidak
 * menyentuh hardware (clock = urusan bootloader/CRU).
 */
void gpio_init(void);

/*
 * gpio_set - jadikan pin output dan drive ke `val` (0/1).
 * Kembalikan 0 bila ok, -1 bila bank/pin di luar jangkauan.
 */
int gpio_set(unsigned bank, unsigned pin, unsigned val);

/*
 * gpio_get - baca level pin (0/1). Kembalikan -1 bila bank/pin di luar
 * jangkauan.
 */
int gpio_get(unsigned bank, unsigned pin);

#endif /* QAONIC_GPIO_H */
