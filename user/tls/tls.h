/*
 * user/tls/tls.h -- API TLS client untuk program user QaonicOS (Q2b).
 *
 * Lapisan tipis di atas mbedTLS: TCP via syscall 73-77, handshake
 * TLS 1.2, baca/tulis record. Satu koneksi dalam satu waktu
 * (cermin batasan TCP client kernel).
 */
#ifndef QAON_TLS_H
#define QAON_TLS_H

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"
#include "mbedtls/platform.h"

/* Konteks satu koneksi TLS (taruh di BSS/static, jangan di stack). */
struct tls_ctx {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	mbedtls_ctr_drbg_context rng;
	mbedtls_entropy_context entropy;
	int tcp_up;		/* 1 bila TCP tersambung */
};

/* Port (tls_port.c). */
void *tls_calloc(unsigned n, unsigned size);
void tls_free(void *p);
void tls_pool_reset(void);
unsigned tls_pool_max(void);	/* puncak pemakaian pool (diagnostik) */

/* BIO (tls_bio.c): dipasang via mbedtls_ssl_set_bio. */
int tls_bio_send(void *ctx, const unsigned char *buf, unsigned len);
int tls_bio_recv(void *ctx, unsigned char *buf, unsigned len);

/* API (tls_api.c). */
void tls_init(void);		/* sekali: platform_set_calloc_free */
int tls_connect(struct tls_ctx *c, unsigned ip, unsigned port,
		const char *ca_pem, const char *hostname);
int tls_write(struct tls_ctx *c, const unsigned char *buf, unsigned len);
int tls_read(struct tls_ctx *c, unsigned char *buf, unsigned maxlen);
void tls_close(struct tls_ctx *c);
/* Kode error BIO (nilai resmi mbedtls/net_sockets.h; MBEDTLS_NET_C tak
 * dipakai di bare-metal). */
#define TLS_ERR_NET_CONN_RESET		(-0x0050)
#define TLS_ERR_NET_CONNECT_FAILED	(-0x0044)
const char *tls_err(int r);	/* string error mbedTLS (buffer statis) */

#endif /* QAON_TLS_H */
