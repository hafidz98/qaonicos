/*
 * user/utlscli.c -- Uji TLS client Q2b (daemon sementara).
 *
 * Handshake TLS 1.2 vs openssl s_server di host (10.0.2.2:18443),
 * validasi chain penuh ke CA test (di-embed), verifikasi hostname,
 * lalu GET / via TLS dan cek "HTTP/" + "200".
 * Hasil: "UTLSCLI: PASS" / "UTLSCLI: FAIL (...)".
 *
 * Jam: sandbox tak bisa NTP (UDP diblokir), jadi uji ini set jam
 * dinding fake via sys_time_set() agar validasi masa berlaku cert
 * berjalan realistis. Di produksi, jam diisi daemon ntp.
 *
 * SEMENTARA: dihapus setelah verifikasi (pola A4).
 */

#include "ulib/ulib.h"
#include "tls/tls.h"

__attribute__((section(".text.start")))
void _start(void);

#define	HOST_IP		0x0A000202u	/* 10.0.2.2 */
#define	HOST_PORT	18443u
#define	HOST_NAME	"qabot-test.local"

/* CA test (dibuat tools/pem2c.py saat build). */
extern const char qaonic_test_ca_pem[];

/* Jam fake untuk uji: 2026-09-29 04:00:00 UTC (di dalam masa cert). */
#define	FAKE_NOW	1790683200u	/* 2026-09-29 12:00 UTC (cert notBefore 2026-09-29) */

static struct tls_ctx ctx;

void
_start(void)
{
	static unsigned char resp[2048];
	unsigned rlen = 0u;
	unsigned t0;
	int r, ok = 0;
	unsigned i;
	static const unsigned char req[] = "GET / HTTP/1.0\r\n\r\n";


	sys_time_set(FAKE_NOW);
	tls_init();

	r = tls_connect(&ctx, HOST_IP, HOST_PORT,
			qaonic_test_ca_pem, HOST_NAME);
	if (r != 0) {
		char hb[12];
		const char *hx = "0123456789ABCDEF";
		unsigned ur = (unsigned)r;
		int i;
		hb[0] = '-'; hb[1] = '0'; hb[2] = 'x';
		for (i = 0; i < 8; i++)
			hb[3 + i] = hx[(ur >> (28 - 4 * i)) & 15u];
		hb[11] = 0;
		puts("UTLSCLI: FAIL handshake err=");
		puts(hb);
		puts("\n");
		goto idle;
	}

	r = tls_write(&ctx, req, sizeof(req) - 1u);
	if (r < 0) {
		puts("UTLSCLI: FAIL (write: ");
		puts(tls_err(r));
		puts(")\n");
		goto done;
	}

	t0 = sys_uptime();
	for (;;) {
		r = tls_read(&ctx, resp + rlen, sizeof(resp) - rlen);
		if (r > 0) {
			rlen += (unsigned)r;
			t0 = sys_uptime();
		} else if (r == 0) {
			/* close_notify dari server */
			break;
		} else {
			puts("UTLSCLI: FAIL (read: ");
			puts(tls_err(r));
			puts(")\n");
			goto done;
		}
		if (sys_uptime() - t0 > 10u)
			break;
		if (rlen >= sizeof(resp))
			break;
		sys_yield();
	}

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
		puts("UTLSCLI: PASS\n");
	else
		puts("UTLSCLI: FAIL (respons tak valid)\n");

	{
		char tmp[64];
		snprintf(tmp, sizeof(tmp),
			 "utlscli: puncak pool TLS %u byte\n", tls_pool_max());
		puts(tmp);
	}

done:
	tls_close(&ctx);
idle:
	for (;;)
		sys_yield();
}
