/*
 * netstack.h - Stack TCP/IP minimal: Ethernet + ARP + IPv4 + ICMP, Fase 11.
 *
 * IP guest: 10.0.2.15 (cocok default QEMU user-net & tap0 10.0.2.0/24).
 * Menyediakan ping (ICMP echo) + resolusi ARP. TCP/HTTP = fase berikut.
 */
#ifndef MACH_NETSTACK_H
#define MACH_NETSTACK_H

#include <stdint.h>

#define NET_IP  0x0A00020Fu   /* 10.0.2.15 */

/* Inisialisasi: pasang RX callback ke driver. */
void netstack_init(void);

/* Dipanggil tiap loop thread net (untuk retry ARP dsb). */
void netstack_tick(void);

/* Kirim ICMP echo request ke ip (untuk verifikasi dua arah). */
int netstack_ping(uint32_t ip);

/* Statistik diagnostik. */
unsigned netstack_rx_frames(void);
unsigned netstack_arp_hits(void);
unsigned netstack_ping_replies(void);
unsigned netstack_ping_got(void);

#endif /* MACH_NETSTACK_H */
