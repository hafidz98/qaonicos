/*
 * user/utcpcli.c -- Uji TCP client Q2a (daemon sementara).
 *
 * Connect ke HTTP server di host (10.0.2.2:18080), kirim "GET /",
 * baca respons, verifikasi "HTTP/" + "200".
 * Hasil: "UTCPCLI: PASS" / "UTCPCLI: FAIL (...)".
 * Setelah uji selesai, yield selamanya (daemon idle).
 *
 * SEMENTARA: dihapus setelah verifikasi (pola A4).
 */

#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void _start(void);

#define	HOST_IP		0x0A000202u	/* 10.0.2.2 (slirp gateway) */
#define	HOST_PORT	18081u

/* Tunggu status == want (detik), -1 bila timeout/tertutup. */
static int
wait_status(int want, unsigned timeout_s)
{
	unsigned t0 = sys_uptime();

	for (;;) {
		int st = sys_tcp_status();

		if (st == want)
			return 0;
		if (st == 0 && want != 0)
			return -1;
		if (sys_uptime() - t0 >= timeout_s)
			return -1;
		sys_yield();
	}
}

void
_start(void)
{
	static unsigned char resp[2048];
	unsigned rlen = 0u;
	unsigned t0, sent, reqlen;
	int n, ok = 0;
	unsigned i;
	char req[] = "GET / HTTP/1.0\r\n\r\n";

	puts("utcpcli: uji TCP client -> 10.0.2.2:18080\n");

	if (sys_tcp_connect(HOST_IP, HOST_PORT) != 0) {
		puts("UTCPCLI: FAIL (connect)\n");
		goto idle;
	}
	if (wait_status(2, 15) != 0) {
		puts("UTCPCLI: FAIL (tak ESTABLISHED)\n");
		goto idle;
	}
	puts("utcpcli: ESTABLISHED\n");

	/* Kirim request (stop-and-wait: ulangi sampai semua terkirim). */
	reqlen = ustrlen(req);
	sent = 0u;
	t0 = sys_uptime();
	while (sent < reqlen) {
		n = sys_tcp_send(req + sent, reqlen - sent);
		if (n < 0)
			break;
		if (n > 0)
			sent += (unsigned)n;
		if (sys_uptime() - t0 > 10u)
			break;
		sys_yield();
	}
	if (sent != reqlen) {
		puts("UTCPCLI: FAIL (send)\n");
		goto idle;
	}

	/* Baca respons sampai peer tutup / idle 10 dtk. */
	t0 = sys_uptime();
	for (;;) {
		n = sys_tcp_recv(resp + rlen, sizeof(resp) - rlen);
		if (n > 0) {
			rlen += (unsigned)n;
			t0 = sys_uptime();
		}
		if (sys_tcp_status() == 0 && n == 0)
			break;		/* peer FIN */
		if (sys_uptime() - t0 > 10u)
			break;
		if (rlen >= sizeof(resp))
			break;
		sys_yield();
	}

	/* Verifikasi: "HTTP/" di awal dan ada "200". */
	if (rlen >= 5u && resp[0] == 'H' && resp[1] == 'T' &&
	    resp[2] == 'T' && resp[3] == 'P') {
		for (i = 0; i + 2u < rlen; i++) {
			if (resp[i] == '2' && resp[i + 1] == '0' &&
			    resp[i + 2] == '0') {
				ok = 1;
				break;
			}
		}
	}
	if (ok)
		puts("UTCPCLI: PASS\n");
	else
		puts("UTCPCLI: FAIL (respons tak valid)\n");

idle:
	sys_tcp_close();
	for (;;)
		sys_yield();
}
