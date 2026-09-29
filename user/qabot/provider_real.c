/*
 * user/qabot/provider_real.c -- provider HTTPS+JSON (Q2c).
 *
 * chat(): serialize history -> JSON -> POST /v1/chat/completions via TLS
 * -> parse tool_calls/content -> kembalikan baris "TOOL:..."/"FINAL:..."
 * sesuai kontrak qb_provider. API key dari NVS KV "llm.key" (fallback
 * kunci uji bila NVS tak tersedia, mis. SD QEMU).
 */
#include "qabot.h"
#include "tls/tls.h"
#include "cfg/cfg.h"
#include "ulib/ulib.h"

/* CA test di-embed (dibangkitkan build-md.sh -> qaonic_test_ca_pem). */
extern const char	qaonic_test_ca_pem[];

#define QBR_IP		0x0A000202u	/* 10.0.2.2 (host via slirp) */
#define QBR_PORT	18444u		/* mock OpenAI HTTPS di host */
#define QBR_HOSTNAME	"qabot-test.local"
#define QBR_PATH	"/v1/chat/completions"
#define QBR_TESTKEY	"test-key-qabot"

/* Buffer statis: koneksi TLS + HTTP. */
static struct tls_ctx	qr_tls;
static char		qr_req[4096];
static char		qr_http[5120];
static char		qr_resp[8192];
static char		qr_line[640];
static int		qr_tls_up;

struct qb_provider	qb_real_provider;	/* diisi qb_real_init */

static const char	*qr_real_chat(void *ctx, struct qb_history *h);

/* Cari KV "llm.key" di NVS; 0 bila ketemu (key terisi). */
static int
qr_load_key(char *key, unsigned klen)
{
	static struct nvs_state	nvs;
	unsigned i, j;

	if (cfg_load(&nvs) != 0)
		return -1;
	for (i = 0; i < nvs.kv_count && i < NVS_KV_MAX; i++) {
		const char *k = nvs.kv[i].key;
		if (k[0] == 'l' && k[1] == 'l' && k[2] == 'm' &&
		    k[3] == '.' && k[4] == 'k' && k[5] == 'e' &&
		    k[6] == 'y' && k[7] == 0) {
			for (j = 0; j + 1u < klen &&
			    nvs.kv[i].val[j]; j++)
				key[j] = nvs.kv[i].val[j];
			key[j] = 0;
			return 0;
		}
	}
	return -1;
}

void
qb_real_init(struct qb_real_ctx *c, const char *key_or_null)
{
	unsigned i;

	for (i = 0; i < sizeof(c->key); i++)
		c->key[i] = 0;
	if (key_or_null && key_or_null[0]) {
		for (i = 0; i + 1u < sizeof(c->key) &&
		    key_or_null[i]; i++)
			c->key[i] = key_or_null[i];
	} else if (qr_load_key(c->key, sizeof(c->key)) != 0) {
		for (i = 0; QBR_TESTKEY[i] &&
		    i + 1u < sizeof(c->key); i++)
			c->key[i] = QBR_TESTKEY[i];
	}
	qb_real_provider.chat = qr_real_chat;
	qb_real_provider.ctx = c;
}

/* Baca satu respons HTTP lengkap (header + body Content-Length). */
static int
qr_http_roundtrip(const unsigned char *out, unsigned outlen,
		  char *resp, unsigned resplen)
{
	unsigned got = 0u, hdrlen = 0u, bodylen = 0u;
	int r, tries;

	r = tls_write(&qr_tls, out, outlen);
	if (r < 0)
		return r;
	tries = 0;
	while (tries++ < 20000) {
		if (got >= resplen - 1u)
			return -1;
		r = tls_read(&qr_tls, (unsigned char *)resp + got,
		    resplen - 1u - got);
		if (r < 0)
			return r;
		if (r == 0) {
			sys_yield();
			continue;
		}
		got += (unsigned)r;
		resp[got] = 0;
		if (!hdrlen) {
			unsigned i;
			for (i = 0; i + 3u < got; i++)
				if (resp[i] == '\r' && resp[i+1] == '\n' &&
				    resp[i+2] == '\r' && resp[i+3] == '\n') {
					hdrlen = i + 4u;
					break;
				}
			if (hdrlen) {
				/* cari Content-Length */
				const char *p = resp;
				bodylen = 0u;
				while ((unsigned)(p - resp) + 16u < hdrlen) {
					if ((p[0]=='C'||p[0]=='c') &&
					    qb_starts(p + 1,
						"ontent-Length:")) {
						const char *q =
						    p + 15;
						while (*q == ' ')
							q++;
						while (*q >= '0' &&
						    *q <= '9') {
							bodylen =
							    bodylen * 10u +
							    (unsigned)
							    (*q - '0');
							q++;
						}
						break;
					}
					p++;
				}
			}
		}
		if (hdrlen && got >= hdrlen + bodylen)
			break;
	}
	if (!hdrlen || got < hdrlen + bodylen)
		return -1;
	/* geser body ke awal buffer */
	{
		unsigned i;
		for (i = 0; i <= bodylen; i++)
			resp[i] = resp[hdrlen + i];
	}
	return (int)bodylen;
}

static const char *
qr_real_chat(void *ctx, struct qb_history *h)
{
	struct qb_real_ctx *c = ctx;
	struct qb_toolcall tc;
	char final[QB_MSG_MAX];
	int r, reqlen, httplen, pr;
	unsigned i;

	if (!qr_tls_up) {
		tls_init();
		r = tls_connect(&qr_tls, QBR_IP, QBR_PORT,
		    qaonic_test_ca_pem, QBR_HOSTNAME);
		if (r != 0) {
			(void)qb_snprintf(qr_line, sizeof(qr_line),
			    "FINAL:(tls connect gagal %d)", r);
			return qr_line;
		}
		qr_tls_up = 1;
	}
	reqlen = qb_build_request(h, qr_req, sizeof(qr_req));
	if (reqlen < 0) {
		(void)qb_snprintf(qr_line, sizeof(qr_line),
		    "FINAL:(request JSON kepenuhan)");
		return qr_line;
	}
	httplen = qb_snprintf(qr_http, sizeof(qr_http),
	    "POST %s HTTP/1.1\r\n"
	    "Host: %s\r\n"
	    "Authorization: Bearer %s\r\n"
	    "Content-Type: application/json\r\n"
	    "Content-Length: %d\r\n"
	    "Connection: keep-alive\r\n"
	    "\r\n",
	    QBR_PATH, QBR_HOSTNAME, c->key, reqlen);
	if (httplen < 0 || (unsigned)httplen + (unsigned)reqlen >=
	    sizeof(qr_http)) {
		(void)qb_snprintf(qr_line, sizeof(qr_line),
		    "FINAL:(header HTTP kepenuhan)");
		return qr_line;
	}
	for (i = 0; i < (unsigned)reqlen; i++)
		qr_http[httplen + (int)i] = qr_req[i];
	r = qr_http_roundtrip((const unsigned char *)qr_http,
	    (unsigned)httplen + (unsigned)reqlen,
	    qr_resp, sizeof(qr_resp));
	if (r < 0) {
		/* Sekali reconnect + retry (server boleh tutup keep-alive). */
		tls_close(&qr_tls);
		qr_tls_up = 0;
		tls_init();
		if (tls_connect(&qr_tls, QBR_IP, QBR_PORT,
		    qaonic_test_ca_pem, QBR_HOSTNAME) == 0) {
			qr_tls_up = 1;
			r = qr_http_roundtrip(
			    (const unsigned char *)qr_http,
			    (unsigned)httplen + (unsigned)reqlen,
			    qr_resp, sizeof(qr_resp));
		}
	}
	if (r < 0) {
		(void)qb_snprintf(qr_line, sizeof(qr_line),
		    "FINAL:(http gagal %d)", r);
		return qr_line;
	}
	pr = qb_parse_response(qr_resp, &tc, final, sizeof(final));
	if (pr < 0) {
		(void)qb_snprintf(qr_line, sizeof(qr_line),
		    "FINAL:(parse JSON gagal)");
		return qr_line;
	}
	if (pr == 0) {
		(void)qb_snprintf(qr_line, sizeof(qr_line),
		    "FINAL:%s", final);
		return qr_line;
	}
	/* format TOOL:nama k=v ... */
	r = qb_snprintf(qr_line, sizeof(qr_line), "TOOL:%s", tc.name);
	for (i = 0; i < tc.nargs && r >= 0; i++)
		r = qb_snprintf(qr_line, sizeof(qr_line),
		    "%s %s=%s", qr_line,
		    tc.akey[i], tc.aval[i]);
	if (r < 0) {
		(void)qb_snprintf(qr_line, sizeof(qr_line),
		    "FINAL:(baris TOOL kepenuhan)");
		return qr_line;
	}
	return qr_line;
}

/* qr_real_chat didefinisikan di atas; chat dipasang di qb_real_init. */
