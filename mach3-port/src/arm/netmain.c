/*
 * mach3/kernel/arm/netmain.c -- Inisialisasi network + pump server HTTP
 * (Fase D, App A2).
 *
 * App A2: net_main() yang blocking dipecah dua:
 *  - net_init_all(): langkah 1-3 (sekali, boleh blocking bounded)
 *  - net_pump(): SATU iterasi langkah 4; dipanggil scheduler tiap
 *    SYS_YIELD sehingga HTTP tetap hidup di sela daemon face/uiapp.
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

/* Network siap dipump (net_init_all sukses). */
static int	net_ready = 0;

/*
 * net_init_all: langkah 1-3 (sekali).  Gagal -> net_ready tetap 0
 * (tak blocking selamanya; boot lanjut tanpa network).
 */
void
net_init_all(void)
{
	unsigned int tries;

	if (net_init() != 0) {
		printf("[net] net_init GAGAL; network dinonaktifkan\n");
		return;
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

	net_ready = 1;
}

/*
 * net_pump: satu iterasi loop server (RX + timer TCP).  Dipanggil dari
 * sched_yield_switch (konteks trap SVC, IRQ mati); driver net polling
 * murni tanpa IRQ jadi aman.
 */
void
net_pump(void)
{
	if (!net_ready)
		return;
	net_poll();
	netstack_tick();
}
