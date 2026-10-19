/*
 * user/utcpkeep.c -- F1 regression: TCP client RX vs keep-alive server.
 *
 * Connect ke mock HTTP/1.1 keep-alive di host (10.0.2.2:18082),
 * kirim GET, baca body sesuai Content-Length (4096B + header).
 * Server TIDAK menutup koneksi (tanpa FIN) selama 30 dtk.
 * Lapor progres tiap detik; hasil: UTCPKEEP: PASS/FAIL (n byte).
 *
 * SEMENTARA: alat investigasi F1.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void _start(void);

#define	HOST_IP		0x0A000202u	/* 10.0.2.2 */
#define	HOST_PORT	18082u
#define	WANT_BODY	4096u

static void
putu(unsigned v)
{
	char tmp[11];
	int n = 0, i;

	if (v == 0u) {
		sys_write(1, "0", 1u);
		return;
	}
	while (v > 0u && n < 11) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (i = n - 1; i >= 0; i--)
		sys_write(1, &tmp[i], 1u);
}

void
_start(void)
{
	static unsigned char resp[8192];
	static const char req[] =
	    "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
	unsigned got = 0u, hdrlen = 0u, bodylen = 0u;
	unsigned t0, tlast, i;
	int n, fails = 0;

	u_put("utcpkeep: TCP keep-alive RX test -> 10.0.2.2:18082\n");
	if (sys_tcp_connect(HOST_IP, HOST_PORT) != 0) {
		u_put("UTCPKEEP: FAIL (connect)\n");
		goto done;
	}
	t0 = sys_uptime();
	while (sys_tcp_status() != 2) {
		if (sys_tcp_status() == 0 ||
		    sys_uptime() - t0 > 15000u) {
			u_put("UTCPKEEP: FAIL (no ESTABLISHED)\n");
			goto done;
		}
		sys_yield();
	}
	sys_tcp_send((const unsigned char *)req, sizeof(req) - 1u);

	t0 = sys_uptime();
	tlast = t0;
	for (;;) {
		unsigned now = sys_uptime();

		if (now - tlast >= 1000u) {
			u_put("  t+");
			putu((now - t0) / 1000u);
			u_put("s got=");
			putu(got);
			u_put("\n");
			tlast = now;
		}
		if (got < sizeof(resp) - 1u) {
			unsigned want = sizeof(resp) - 1u - got;

			if (want > 4096u)	/* batas SYS_TCP_RECV kernel */
				want = 4096u;
			n = sys_tcp_recv(resp + got, want);
			if (n < 0) {
				u_put("UTCPKEEP: FAIL (recv err)\n");
				fails = 1;
				break;
			}
			if (n > 0) {
				got += (unsigned)n;
				resp[got] = 0;
				if (!hdrlen) {
					for (i = 0; i + 3u < got; i++)
						if (resp[i] == '\r' &&
						    resp[i+1] == '\n' &&
						    resp[i+2] == '\r' &&
						    resp[i+3] == '\n') {
							hdrlen = i + 4u;
							break;
						}
					if (hdrlen) {
						/* Content-Length ada di
						 * header mock (4096). */
						bodylen = WANT_BODY;
					}
				}
				if (hdrlen && got >= hdrlen + bodylen)
					break;
			}
		}
		if (now - t0 > 25000u) {
			u_put("UTCPKEEP: TIMEOUT\n");
			fails = 1;
			break;
		}
		sys_yield();
	}
	sys_tcp_close();
	u_put(fails == 0 && got >= hdrlen + bodylen ?
	    "UTCPKEEP: PASS\n" : "UTCPKEEP: FAIL\n");
done:
	for (;;)
		sys_yield();
}
