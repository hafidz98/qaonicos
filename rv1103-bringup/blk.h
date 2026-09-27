/*
 * blk.h - Driver virtio-blk via virtio-mmio LEGACY (QEMU -M virt), Fase 12d.
 *
 * Menyediakan "storage" 128MB untuk emulasi SPI NAND Luckfox Pico Mini:
 * QEMU dipasang `-drive file=pico128.img,if=none,id=hd0,format=raw`
 * + `-device virtio-blk-device,drive=hd0`. Kapasitas dibaca dari config
 * space device (REAL, bukan konstanta). I/O sinkron 1 sektor (512B)
 * via polling used ring (tanpa IRQ).
 *
 * API:
 *   int      blk_init(void);          // 0 = ok, <0 = gagal
 *   uint32_t blk_total_sectors(void); // dari device (128MB -> 262144)
 *   uint32_t blk_used_sectors(void);  // sektor yang dipakai kernel (bump)
 *   uint64_t blk_alloc(void);         // alokasi 1 sektor via bump allocator
 *   int      blk_write(uint64_t sector, const uint8_t *data); // 512B
 *   int      blk_read(uint64_t sector, uint8_t *data);         // 512B
 *   void     blk_log(const char *s);  // logging UART
 */
#ifndef MACH_BLK_H
#define MACH_BLK_H

#include <stdint.h>

int      blk_init(void);
uint32_t blk_total_sectors(void);
uint32_t blk_used_sectors(void);
uint64_t blk_alloc(void);
int      blk_write(uint64_t sector, const uint8_t *data);
int      blk_read(uint64_t sector, uint8_t *data);
void     blk_log(const char *s);
void     blk_loghex(uint32_t v);
/* Fase 12d: catat 1 request HTTP ke access log di disk (best-effort). */
void     blk_log_request(uint32_t ticks, unsigned code);

#endif /* MACH_BLK_H */
