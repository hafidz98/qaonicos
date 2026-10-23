/*
 * kernel/kernel/arm/pl330.h -- Driver minimal ARM PL330 DMAC (D2b).
 */
#ifndef PL330_H
#define PL330_H

/* Inisialisasi DMAC. Return 0/-1. */
int	pl330_init(void);

/* Rakit microcode DMA I2S RX -> RAM. Return 0/-1. */
int	pl330_audio_rx(unsigned int dst, unsigned int nwords);

/* Test helpers (verifikasi struktur microcode di QEMU). */
unsigned int	pl330_ucode_len(void);
unsigned int	pl330_ucode_byte(unsigned int idx);

#endif /* PL330_H */
