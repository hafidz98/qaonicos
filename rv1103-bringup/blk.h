/*
 * blk.h - Driver virtio-blk via virtio-mmio LEGACY (QEMU -M virt).
 *
 * Fase 12d: satu device (pico128.img, storage internal emulasi SPI NAND).
 * Fase 15: abstraksi blkdev — DUA device:
 *   dev 0 = storage internal: QEMU dipasang
 *           `-drive file=pico128.img,if=none,id=hd0,format=raw`
 *           + `-device virtio-blk-device,drive=hd0` (slot MMIO lebih rendah).
 *           Kapasitas dari config space device (REAL). Superblock
 *           "QAONBLK1" sektor 0; API blk_* melayani dev 0 (semantik
 *           Fase 12d UTUH).
 *   dev 1 = kartu SD: `-drive file=sd128.img,if=none,id=sd0,format=raw`
 *           + `-device virtio-blk-device,drive=sd0`. Superblock
 *           "QAONSD01" sektor 0; raw sector I/O via sd_read/sd_write.
 *
 * I/O sinkron 1 sektor (512B) via polling used ring (tanpa IRQ).
 */
#ifndef MACH_BLK_H
#define MACH_BLK_H

#include <stdint.h>

/* Jumlah maksimum device blkdev (dev 0 storage internal, dev 1 SD). */
#define BLK_MAXDEV 2u

int      blk_init(void);          /* 0 = dev 0 ok (dev 1 best-effort) */
uint32_t blk_total_sectors(void); /* dev 0, dari device (128MB -> 262144) */
uint32_t blk_used_sectors(void);  /* dev 0, sektor yang dipakai kernel */
uint64_t blk_alloc(void);         /* dev 0, alokasi 1 sektor via bump */
int      blk_write(uint64_t sector, const uint8_t *data); /* dev 0, 512B */
int      blk_read(uint64_t sector, uint8_t *data);         /* dev 0, 512B */
void     blk_log(const char *s);  /* logging UART */
void     blk_loghex(uint32_t v);
/* Fase 12d: catat 1 request HTTP ke access log di disk (best-effort). */
void     blk_log_request(uint32_t ticks, unsigned code);

/* Fase 15: kartu SD (dev 1). 1 = ada, 0 = tidak ada. */
int      sd_present(void);
uint32_t sd_total_sectors(void);
int      sd_write(uint64_t sector, const uint8_t *data); /* 512B */
int      sd_read(uint64_t sector, uint8_t *data);        /* 512B */

#endif /* MACH_BLK_H */
