/*
 * user/utlskeep.c -- F1: TLS keep-alive RX test (daemon sementara).
 *
 * Handshake TLS 1.2 vs mock_openai_https.py di host (10.0.2.2:18444),
 * kirim GET / HTTP/1.1 (keep-alive), baca respons sesuai Content-Length.
 * Mock HARUS dalam mode keep-alive (close_connection=False) -- tanpa FIN.
 * Hasil: "UTLSKEEP: PASS" / "UTLSKEEP: FAIL (...)".
 *
 * SEMENTARA: alat investigasi F1.
 */
#include "ulib/ulib.h"
#include "tls/tls.h"

__attribute__((section(".text.start")))
void _start(void);

/* CA test (dibuat tools/pem2c.py saat build). */
extern const char qaonic_test_ca_pem[];

/* Jam fake: 2026-09-29 12:00 UTC (di dalam masa cert). */
#define	FAKE_NOW	1790683200u

#define	HOST_IP		0x0A000202u	/* 10.0.2.2 */
#define	HOST_PORT	18444u
#define	HOST_NAME	"qabot-test.local"

static struct tls_ctx ctx;

static void
putu(unsigned v)
{
	char tmp[11];
	int n = 0, i;

	if (v == 0u) {
		puts("0");
		return;
	}
	while (v > 0u && n < 11) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	for (i = n - 1; i >= 0; i--) {
		char c = tmp[i];

		sys_write(1, &c, 1u);
	}
}

void
_start(void)
{
	static unsigned char resp[4096];
	static const unsigned char req[] =
	    "POST /v1/chat/completions HTTP/1.1\r\n"
	    "Host: qabot-test.local\r\n"
	    "Content-Type: application/json\r\n"
	    "Content-Length: 27\r\n"
	    "Connection: keep-alive\r\n\r\n"
	    "{\"model\":\"x\",\"messages\":[]}";
	unsigned rlen = 0u, hdrlen = 0u, bodylen = 0u;
	unsigned t0, tlast, i, round;
	int r;

	puts("utlskeep: TLS keep-alive RX test -> 10.0.2.2:18444\n");
	sys_time_set(FAKE_NOW);
	tls_init();

	r = tls_connect(&ctx, HOST_IP, HOST_PORT,
			qaonic_test_ca_pem, HOST_NAME);
	if (r != 0) {
		puts("UTLSKEEP: FAIL (handshake err=");
		putu((unsigned)(-r));
		puts(")\n");
		goto idle;
	}
	puts("utlskeep: handshake OK\n");

	t0 = sys_uptime();
	tlast = t0;
	for (round = 0; round < 2; round++) {
		rlen = 0u;
		hdrlen = 0u;
		bodylen = 0u;
		puts("utlskeep: round ");
		putu(round + 1u);
		puts("\n");
		r = tls_write(&ctx, req, sizeof(req) - 1u);
		if (r < 0) {
			puts("UTLSKEEP: FAIL (write round ");
			putu(round + 1u);
			puts(" err=");
			putu((unsigned)(-r));
			puts(")\n");
			goto idle;
		}
	for (;;) {
		unsigned now = sys_uptime();

		if (now - tlast >= 1000u) {
			puts("  t+");
			putu((now - t0) / 1000u);
			puts("s rlen=");
			putu(rlen);
			puts("\n");
			tlast = now;
		}
		r = tls_read(&ctx, resp + rlen, sizeof(resp) - rlen);
		if (r > 0) {
			rlen += (unsigned)r;
			resp[rlen] = 0;
			t0 = now;
			if (!hdrlen) {
				for (i = 0; i + 3u < rlen; i++)
					if (resp[i] == '\r' &&
					    resp[i+1] == '\n' &&
					    resp[i+2] == '\r' &&
					    resp[i+3] == '\n') {
						hdrlen = i + 4u;
						break;
					}
				if (hdrlen) {
					/* Parse Content-Length (cocok persis). */
					for (i = 0; i + 15u < hdrlen; i++)
						if (((resp[i]=='C'||resp[i]=='c') &&
						    resp[i+1]=='o' &&
						    resp[i+2]=='n' &&
						    resp[i+3]=='t' &&
						    resp[i+4]=='e' &&
						    resp[i+5]=='n' &&
						    resp[i+6]=='t' &&
						    resp[i+7]=='-' &&
						    (resp[i+8]=='L'||resp[i+8]=='l')) ||
						    0) {
							unsigned j = i + 15u;

							while (j < hdrlen &&
							    resp[j] == ' ')
								j++;
							bodylen = 0u;
							while (j < hdrlen &&
							    resp[j] >= '0' &&
							    resp[j] <= '9') {
								bodylen =
								    bodylen * 10u +
								    (unsigned)
								    (resp[j] - '0');
								j++;
							}
							break;
						}
				}
			}
			if (hdrlen && rlen >= hdrlen + bodylen)
				break;
		} else if (r == 0) {
			puts("UTLSKEEP: FAIL (close_notify prematur)\n");
			goto idle;
		} else {
			puts("UTLSKEEP: FAIL (tls_read err=");
			putu((unsigned)(-r));
			puts(" round=");
			putu(round + 1u);
			puts(")\n");
			goto idle;
		}
		if (now - t0 > 20000u) {
			puts("UTLSKEEP: TIMEOUT (tcp_status=");
			putu((unsigned)sys_tcp_status());
			puts(")\n");
			goto fail;
		}
		sys_yield();
	}
	} /* round */
	/* Validasi: respons harus 200 (bukan 501/404). */
	{
		unsigned ok = 0u;

		for (i = 0; i + 2u < hdrlen; i++)
			if (resp[i] == '2' && resp[i+1] == '0' &&
			    resp[i+2] == '0') {
				ok = 1u;
				break;
			}
		puts(ok ? "UTLSKEEP: PASS\n" :
		    "UTLSKEEP: FAIL (bukan 200)\n");
	}
	goto idle;
fail:
	puts("UTLSKEEP: FAIL\n");
idle:
	for (;;)
		sys_yield();
}
