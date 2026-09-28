/*
 * mach3/kernel/arm/netmain.c -- Inisialisasi network + loop server HTTP
 * (Fase D).
 *
 * Dipanggil setelah program userspace selesai. Tidak pernah kembali:
 *  1. net_init()      -- driver virtio-net
 *  2. netstack_init() -- Ethernet/ARP/IPv4/ICMP
 *  3. Uji mandiri ping ke 10.0.2.1 (host QEMU user-net)
 *  4. Loop: net_poll() + netstack_tick() selamanya
 *
 * Server TCP/HTTP hidup di tcp.c/http.c; koneksi masuk diproses via
 * netstack_rx -> tcp_on_ip -> http_handle.
 */

extern int		net_init(void);
extern void		netstack_init(void);
extern unsigned		net_poll(void);
extern void		netstack_tick(void);
extern int		netstack_ping(unsigned int dst_ip);
extern int		netstack_ping_got(void);
extern int		printf(const char *, ...);
extern void		delay(int usec);

/* Loop server network. Tidak kembali. */
void
net_main(void)
{
	unsigned int tries;

	if (net_init() != 0) {
		printf("[net] net_init GAGAL; network dinonaktifkan\n");
		for (;;)
			delay((int)1000000);
	}
	netstack_init();
	printf("[net] IP 10.0.2.15, HTTP server port 80 (/ dan /metrics)\n");

	/* Uji mandiri: ping host 10.0.2.2 (QEMU user-net host).
	 * netstack_ping mengirim ARP request dulu bila MAC belum dikenal. */
	tries = 0u;
	for (;;) {
		net_poll();
		netstack_tick();
		if ((tries % 20000u) == 0u) {
			if (netstack_ping(0x0A000202u) == 0)
				printf("[net] ping -> 10.0.2.2\n");
		}
		tries++;
		if (netstack_ping_got() > 0)
			break;
		/* batasi loop uji agar tak selamanya bila host tak ada */
		if (tries > 2000000u)
			break;
	}
	if (netstack_ping_got() > 0)
		printf("[net] PING 10.0.2.2 BERHASIL\n");
	else
		printf("[net] ping timeout (lanjut mode listen)\n");

	/* Loop utama server. */
	for (;;) {
		net_poll();
		netstack_tick();
	}
}
