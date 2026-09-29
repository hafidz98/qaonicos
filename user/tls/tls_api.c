/*
 * user/tls/tls_api.c -- API TLS client tingkat tinggi (Q2b).
 *
 * tls_connect(): TCP connect -> handshake TLS 1.2 -> verifikasi chain
 *   CA (VERIFY_REQUIRED) + hostname. Gagal -> kode negatif mbedTLS.
 */

#include "tls.h"
#include "ulib/ulib.h"
#include <string.h>

#define TLS_HANDSHAKE_TIMEOUT_S	30u

static void
tls_debug_cb(void *ctx, int level, const char *file, int line,
	     const char *str)
{
	(void)ctx;
	(void)level;
	puts(file);
	puts(":");
	/* line tak dicetak (hemat); str sudah ber-'\n'. */
	(void)line;
	puts(str);
}

void
tls_init(void)
{
	mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
	mbedtls_platform_set_time(tls_time_get);	/* Q2b: set time fn pointer */
}

const char *
tls_err(int r)
{
	static char buf[128];

	mbedtls_strerror(r, buf, sizeof(buf));
	return buf;
}

/* Tunggu status TCP == want (detik). */
static int
wait_tcp(int want, unsigned timeout_s)
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

int
tls_connect(struct tls_ctx *c, unsigned ip, unsigned port,
	    const char *ca_pem, const char *hostname)
{
	static const char pers[] = "qaonic-qabot";
	int r;
	unsigned t0;

	tls_pool_reset();
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_x509_crt_init(&c->ca);
	mbedtls_ctr_drbg_init(&c->rng);
	mbedtls_entropy_init(&c->entropy);
	c->tcp_up = 0;

	/* CA chain (PEM di-embed program). */
	r = mbedtls_x509_crt_parse(&c->ca,
		(const unsigned char *)ca_pem, strlen(ca_pem) + 1u);
	if (r != 0)
		goto out;

	/* RNG: CTR-DRBG di atas entropy (hardware_poll, lihat tls_port.c). */
	r = mbedtls_ctr_drbg_seed(&c->rng, mbedtls_entropy_func,
				  &c->entropy,
				  (const unsigned char *)pers, sizeof(pers) - 1);
	if (r != 0)
		goto out;
	r = mbedtls_ssl_config_defaults(&c->conf,
					MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT);
	if (r != 0)
		goto out;

	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->rng);
	mbedtls_ssl_conf_dbg(&c->conf, tls_debug_cb, NULL);
	mbedtls_debug_set_threshold(0);
	r = mbedtls_ssl_setup(&c->ssl, &c->conf);
	if (r != 0)
		goto out;

	/* SNI + verifikasi hostname vs CN/SAN cert. */
	r = mbedtls_ssl_set_hostname(&c->ssl, hostname);
	if (r != 0)
		goto out;

	mbedtls_ssl_set_bio(&c->ssl, NULL,
			     tls_bio_send, tls_bio_recv, NULL);

	/* TCP connect dulu (BIO butuh koneksi hidup). tcc kernel hanya satu
	 * koneksi client; daemon uji lain (utcpcli) mungkin masih memakai
	 * -> coba lagi sampai 60 dtk. */
	t0 = sys_uptime();
	for (;;) {
		if (sys_tcp_connect(ip, port) == 0)
			break;
		if (sys_uptime() - t0 > 60u) {
			r = TLS_ERR_NET_CONNECT_FAILED;
			goto out;
		}
		sys_yield();
	}
	c->tcp_up = 1;
	if (wait_tcp(2, 15) != 0) {
		r = TLS_ERR_NET_CONNECT_FAILED;
		goto out_close;
	}

	/* Handshake (non-blocking di dalam, WANT -> yield). */
	t0 = sys_uptime();
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ &&
		    r != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto out_close;
		if (sys_uptime() - t0 > TLS_HANDSHAKE_TIMEOUT_S) {
			r = MBEDTLS_ERR_SSL_TIMEOUT;
			goto out_close;
		}
		sys_yield();
	}

	/* Verifikasi chain harus bersih (CA test kita valid). */
	r = (int)mbedtls_ssl_get_verify_result(&c->ssl);
	if (r != 0) {
		r = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
		goto out_close;
	}
	return 0;

out_close:
	sys_tcp_close();
	c->tcp_up = 0;
out:
	return r;
}

int
tls_write(struct tls_ctx *c, const unsigned char *buf, unsigned len)
{
	unsigned sent = 0u;
	int r;

	while (sent < len) {
		r = mbedtls_ssl_write(&c->ssl, buf + sent, len - sent);
		if (r == MBEDTLS_ERR_SSL_WANT_READ ||
		    r == MBEDTLS_ERR_SSL_WANT_WRITE) {
			sys_yield();
			continue;
		}
		if (r <= 0)
			return r;
		sent += (unsigned)r;
	}
	return (int)sent;
}

int
tls_read(struct tls_ctx *c, unsigned char *buf, unsigned maxlen)
{
	int r = mbedtls_ssl_read(&c->ssl, buf, maxlen);

	if (r == MBEDTLS_ERR_SSL_WANT_READ ||
	    r == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;	/* panggil lagi */
	return r;		/* n / 0 (close_notify) / negatif (error) */
}

void
tls_close(struct tls_ctx *c)
{
	/* close_notify best-effort, lalu bebaskan semua. */
	(void)mbedtls_ssl_close_notify(&c->ssl);
	if (c->tcp_up) {
		sys_tcp_close();
		c->tcp_up = 0;
	}
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	mbedtls_x509_crt_free(&c->ca);
	mbedtls_ctr_drbg_free(&c->rng);
	mbedtls_entropy_free(&c->entropy);
	tls_pool_reset();
}
