/*
 * user/tls/inc/mbedtls/mbedtls_config.h -- Konfigurasi mbedTLS 3.6 LTS
 * untuk QaonicOS (Qabot Q2b): TLS 1.2 client bare-metal, tanpa libc/OS.
 *
 * Prinsip: minimal. Hanya yang dibutuhkan handshake client
 * ECDHE-(ECDSA|RSA)-AES128-GCM-SHA256 + validasi chain X.509.
 * Lihat docs/rencana-qabot-harness/PLAN-Q2.md.
 */

#ifndef MBEDTLS_USER_CONFIG_H
#define MBEDTLS_USER_CONFIG_H

/* --- Platform bare-metal (implementasi di user/tls/tls_port.c) --- */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY		/* calloc/free -> bump allocator */
#define MBEDTLS_PLATFORM_STD_CALLOC	tls_calloc
#define MBEDTLS_PLATFORM_STD_FREE	tls_free
#define MBEDTLS_PLATFORM_TIME_ALT	/* mbedtls_time via pointer */
#define MBEDTLS_PLATFORM_MS_TIME_ALT	/* mbedtls_ms_time() -> sys_uptime */
#define MBEDTLS_HAVE_TIME		/* butuh untuk validasi masa cert */
/* Fungsi waktu standar: ganti 'time' bawaan dengan milik kita. */
#define MBEDTLS_PLATFORM_STD_TIME	tls_time_get
#define MBEDTLS_ENTROPY_HARDWARE_ALT	/* mbedtls_hardware_poll() -> timer */
#define MBEDTLS_NO_PLATFORM_ENTROPY	/* hanya hardware_poll sbg sumber */
/* Tak ada: FS_IO, NET_C (BIO sendiri), THREADING_C, TIMING_C. */

/* --- SSL: client TLS 1.2 saja --- */
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_MAX_CONTENT_LEN	8192

/* Ciphersuite yang didukung (server mock Q2 memakai ECDSA P-256). */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* --- Kripto simetris/hash --- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C	/* tanda tangan cert bisa SHA-384/512 */
#define MBEDTLS_MD_C
#define MBEDTLS_CIPHER_C

/* --- Kunci publik --- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* --- X.509 --- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* --- RNG --- */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* --- Util --- */
#define MBEDTLS_ERROR_C		/* pesan error terbaca di log */
#define MBEDTLS_VERSION_C
#define MBEDTLS_DEBUG_C		/* log handshake saat Q2 (bisa dimatikan) */

/* Hemat stack: RSA max 2048-bit, kurva max P-256. */
#define MBEDTLS_MPI_MAX_SIZE	256
#define MBEDTLS_ECP_MAX_BITS	256

#endif /* MBEDTLS_USER_CONFIG_H */
