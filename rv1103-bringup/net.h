/*
 * net.h - Driver virtio-net via virtio-mmio LEGACY (QEMU -M virt), Fase 11.
 *
 * QEMU 8.2.2 memberi transport virtio-mmio versi 1 (legacy): feature
 * 32-bit tanpa selector, queue = 1 alokasi kontinu via QueuePFN.
 * Caches CPU mati (pmap hanya aktifkan MMU) -> DMA-safe tanpa maintenance.
 *
 * API:
 *   int  net_init(void);            // 0 = ok, <0 = gagal
 *   unsigned net_poll(void);       // proses RX/TX completion; panggil rutin
 *                                  // return: jumlah paket yang diproses
 *   int  net_send(const uint8_t *f, unsigned len); // kirim 1 frame Ethernet
 *   const uint8_t *net_mac(void);   // MAC guest (dari config device)
 *   unsigned net_irq(void);         // GIC ID (valid setelah net_init)
 *   void net_on_rx(void (*cb)(const uint8_t *, unsigned)); // handler frame RX
 *   void net_isr(void);             // dipanggil dari dispatch IRQ GIC
 *   void net_log(const char *s);    // logging UART (dipakai netstack.c)
 *   void net_loghex(uint32_t v);
 *
 * TX sinkron (1 buffer); RX 32 buffer @2048, di-repost otomatis.
 */
#ifndef MACH_NET_H
#define MACH_NET_H

#include <stdint.h>

int net_init(void);
unsigned net_poll(void);   /* Fase 12d: return jumlah paket diproses */
int net_send(const uint8_t *frame, unsigned len);
const uint8_t *net_mac(void);
unsigned net_irq(void);
void net_on_rx(void (*cb)(const uint8_t *frame, unsigned len));
void net_isr(void);
void net_log(const char *s);
void net_loghex(uint32_t v);
/* Fase 12d: counter byte kumulatif untuk bandwidth real. */
uint64_t net_rx_bytes_get(void);
uint64_t net_tx_bytes_get(void);

#endif /* MACH_NET_H */
