/*
 * user/tls/tls_bio.c -- BIO mbedTLS di atas syscall TCP client (73-77).
 *
 * Pola blocking dengan timeout: handshake/IO gagal eksplisit
 * (MBEDTLS_ERR_SSL_TIMEOUT) bila peer diam terlalu lama, agar
 * program user tak menggantung selamanya.
 */

#include "tls.h"
#include "ulib/ulib.h"

#define TLS_IO_TIMEOUT_S	20u

int
tls_bio_send(void *ctx, const unsigned char *buf, unsigned len)
{
	unsigned sent = 0u;
	unsigned t0 = sys_uptime();

	(void)ctx;	/* satu koneksi TCP global */
	
	while (sent < len) {
		unsigned chunk = len - sent;
		int n;

		if (chunk > 1200u)
			chunk = 1200u;	/* batas syscall SYS_TCP_SEND */
		n = sys_tcp_send(buf + sent, chunk);
		if (n < 0)
			return TLS_ERR_NET_CONN_RESET;
		if (n == 0) {
			/* Belum di-ack / ARP: tunggu sebentar. */
			if (sys_uptime() - t0 > TLS_IO_TIMEOUT_S)
				return MBEDTLS_ERR_SSL_TIMEOUT;
			sys_yield();
			continue;
		}
		sent += (unsigned)n;
		t0 = sys_uptime();
	}
	return (int)sent;
}

int
tls_bio_recv(void *ctx, unsigned char *buf, unsigned len)
{
	unsigned t0 = sys_uptime();

	(void)ctx;
	for (;;) {
		unsigned chunk = len > 4096u ? 4096u : (unsigned)len;
		int n = sys_tcp_recv(buf, chunk);

		if (n < 0)
			return TLS_ERR_NET_CONN_RESET;
		if (n > 0)
			return n;
		/* n == 0: belum ada data. */
		if (sys_tcp_status() == 0)
			return TLS_ERR_NET_CONN_RESET;	/* peer tutup */
		if (sys_uptime() - t0 > TLS_IO_TIMEOUT_S)
			return MBEDTLS_ERR_SSL_TIMEOUT;
		sys_yield();
	}
}
