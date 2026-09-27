/*
 * tcp.h - Server TCP minimal (satu koneksi, port 80) untuk HTTP, Fase 12.
 *
 * State machine: LISTEN -> SYN_RCVD -> ESTABLISHED -> FIN_SENT -> LISTEN.
 * Retransmit sederhana via tcp_tick() (dipanggil dari netstack_tick).
 * Data yang diterima diteruskan ke http_handle() (http.h).
 */
#ifndef MACH_TCP_H
#define MACH_TCP_H

#include <stdint.h>

#define TCP_PORT 80u

/* Dipanggil dari netstack untuk setiap segmen TCP yang valid checksum-nya.
 * ip = header IPv4, iplen = panjang paket IP, src = IP pengirim,
 * eth_src = MAC sumber (frame Ethernet). */
void tcp_on_ip(const uint8_t *ip, unsigned iplen, uint32_t src,
               const uint8_t *eth_src);

/* Dipanggil tiap loop thread net: retransmit + timeout koneksi buntu. */
void tcp_tick(void);

/* Statistik diagnostik. */
unsigned tcp_rx_segs(void);
unsigned tcp_tx_segs(void);
unsigned tcp_conns(void);

#endif /* MACH_TCP_H */
