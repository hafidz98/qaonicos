/*
 * user/qabot/provider.c -- abstraksi provider + mock scripted (Q1).
 *
 * SEAM Q2: provider real akan (1) serialize history + skema tool ke JSON
 * format OpenAI-compatible, (2) POST via HTTPS (butuh TCP client + TLS
 * mbedTLS), (3) parse "tool_calls" dari respons menjadi struct
 * qb_toolcall. Kontrak chat() tidak berubah: satu baris "TOOL:..."/"FINAL:...".
 */
#include "qabot.h"

static const char *
qb_mock_chat(void *ctx, struct qb_history *h)
{
	struct qb_mock_ctx	*m = ctx;

	(void)h;
	if (m->i >= m->n)
		return "FINAL:(script habis)";
	return m->script[m->i++];
}

void
qb_mock_init(struct qb_mock_ctx *m, const char **script, unsigned n)
{
	m->script = script;
	m->n = n;
	m->i = 0;
}

struct qb_provider qb_mock_provider = {
	qb_mock_chat,
	0,
};
