/*
 * sdmmc.h - Driver SDMMC asli RV1103 (DesignWare MSHC @0xffaa0000).
 *
 * Fase 15: COMPILE-CHECK SAJA (tidak di-link ke kernel QEMU).
 * API raw sector 512B, polling single-block (CMD17/CMD24).
 */
#ifndef MACH_SDMMC_H
#define MACH_SDMMC_H

#include <stdint.h>

/* Reset controller + siapkan clock/divider/power; kirim CMD0.
 * 0 = ok, <0 = gagal. Clock CRU + pinctrl diasumsikan sudah oleh
 * bootloader (lihat komentar di sdmmc.c). */
int sdmmc_init(void);

/* Baca/tulis 1 sektor (512B). Nomor sektor = block addressing
 * (untuk SDHC/SDXC). 0 = ok, <0 = gagal. */
int sdmmc_read_sector(uint32_t sector, uint8_t *buf);
int sdmmc_write_sector(uint32_t sector, const uint8_t *buf);

#endif /* MACH_SDMMC_H */
