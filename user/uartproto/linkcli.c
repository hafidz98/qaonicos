/*
 * linkcli.c - linkd client stub (C6L-5).
 *
 * Implements the uartproto.h client API over IPC to
 * linkd (LINKD-IPC v1, see user/uartproto/linkd.h)
 * instead of over a transport backend. Programs link
 * EITHER uartproto.c (direct backend) OR this stub
 * (via linkd), never both: the function signatures
 * are identical, so consumers (uiapp screens) switch
 * by changing the link line, not the call sites.
 *
 * Rules of the stub:
 *   - `link` is looked up fresh on every call
 *     (uipc_rpc_user); nothing is cached, so a
 *     linkd death/respawn (EDEST/EPEER, generation
 *     bump) recovers on the next call.
 *   - RPC len is both request size and reply budget
 *     (frozen uipc.h v1): short requests are padded
 *     to the op's reply budget with zero bytes.
 *   - Return-code mapping mirrors the direct
 *     helpers: cmd/kv/nethttp/voice 0/-1 (with the
 *     -2 truncation cases where the direct helper
 *     has them); tool_dispatch 0/-1/-2 with the
 *     wire code copied out; poll_inbound 1/0/-1
 *     (EVT only -- peer DISPATCH units are linkd's
 *     business, forwarded to `tools`, never
 *     delivered to clients).
 *   - The stub never retries: a _retry variant
 *     forwards its idem flag and linkd applies the
 *     C6L-4 classifier (DENIED comes back as -1,
 *     the wire untouched). uproto_retry_policy is
 *     deliberately NOT provided here: the classifier
 *     lives in linkd (via the real _retry wrappers);
 *     a second copy in the stub would diverge.
 *     Consumers that need the classifier link the
 *     direct layer or ask linkd.
 *   - WS family and dispatch_result are UNSUP via
 *     linkd (documented in linkd.h): they return -1
 *     without a wire exchange. WS stays direct-link
 *     only until a later phase designs per-client
 *     WS state + BIN draining in linkd.
 *   - voice_crc16 is a pure function (fixed
 *     algorithm); it is provided here so stub-only
 *     programs link without the direct layer.
 *   - No struct assignment / bulk copies (user
 *     programs link without __aeabi_memcpy4):
 *     the single static message buffer is reused
 *     in place, payloads are filled by loops.
 */
#include "ulib/uipc.h"
#include "uartproto/uartproto.h"
#include "uartproto/linkd.h"

static struct uipc_msg	msg;
static unsigned		evt_last_seq;
static unsigned		evt_dropped_stub;

/* --- Small helpers (byte loops, no bulk copies). --- */

static unsigned
slen_c(const char *s)
{
	unsigned i = 0u;

	while (s[i])
		i++;
	return i;
}

static void
wr32(unsigned char *b, unsigned off, unsigned v)
{
	b[off] = (unsigned char)(v & 0xFFu);
	b[off + 1u] = (unsigned char)((v >> 8) & 0xFFu);
	b[off + 2u] = (unsigned char)((v >> 16) & 0xFFu);
	b[off + 3u] = (unsigned char)((v >> 24) & 0xFFu);
}

static int
rd32(const unsigned char *b, unsigned blen, unsigned off,
     unsigned *out)
{
	if (off + 4u > blen)
		return -1;
	*out = (unsigned)b[off] |
	    ((unsigned)b[off + 1u] << 8) |
	    ((unsigned)b[off + 2u] << 16) |
	    ((unsigned)b[off + 3u] << 24);
	return 0;
}

/* Copy a NUL-terminated string from the reply body
 * starting at byte off. Returns its length, or -1
 * if no NUL is found within the body or the length
 * exceeds cap. The destination is always
 * NUL-terminated. */
static int
rdstr(const unsigned char *b, unsigned blen, unsigned off,
      char *dst, unsigned cap)
{
	unsigned i = 0u;

	while (off + i < blen && i <= cap) {
		dst[i] = (char)b[off + i];
		if (b[off + i] == 0)
			return (int)i;
		i++;
	}
	dst[cap] = 0;
	return -1;
}

static unsigned
wrstr(unsigned char *b, unsigned off, const char *s)
{
	unsigned i = 0u;

	while (s[i]) {
		b[off + i] = (unsigned char)s[i];
		i++;
	}
	b[off + i] = 0;
	return off + i + 1u;
}

/* One RPC to linkd. op is the message id; the body
 * must already be built in msg.body starting at
 * byte 0 (word 0 = op); reqlen is the request body
 * length, budget is the padded total length (the
 * reply budget). Returns the reply total size (>0),
 * or a UIPC_E* code. On success the reply is in
 * msg (same buffer, overwritten). */
static int
rpc(unsigned op, unsigned reqlen, unsigned budget,
    unsigned timeout_ms)
{
	unsigned total, i;

	/* The budget is BOTH the send size and the
	 * reply budget: a request longer than the
	 * budget must be REJECTED, never silently
	 * truncated (truncation sent linkd a frame
	 * header promising 1024 PCM bytes with only
	 * ~96 attached -- every voice frame failed
	 * at the C6L-5 first run). Callers treat a
	 * 0 return as a send failure. */
	if (reqlen + UIPC_HDR_SIZE > budget)
		return 0u;
	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = (int)op;
	msg.size = budget;
	/* Zero-pad the request to the budget. */
	for (i = reqlen; i + UIPC_HDR_SIZE < budget; i++)
		msg.body[i] = 0;
	total = budget;
	return uipc_rpc_user(LINKD_NAME, &msg, total, (int)timeout_ms);
}

/* Reply word 0 (status), or -1 if the reply is not
 * a well-formed status reply. */
static int
rep_status(int rc)
{
	unsigned st;

	if (rc <= 0)
		return -1;
	if (rd32(msg.body, (unsigned)rc - UIPC_HDR_SIZE, 0u, &st) != 0)
		return -1;
	return (int)st;
}

/* --- Line commands --- */

/* K5: caller-timeout variant. The default 30 s
 * LINKD_RPC_CMD_MS is right for a user-initiated
 * screen refresh, but a background poll (the
 * status-bar radio poll) must bound its UI-loop
 * stall to a few seconds when the peer hangs
 * mid-exchange: the poll passes its own short
 * timeout and treats a timeout as OFF, retrying
 * on the next period. Same wire path, same
 * parsing -- only the client-side wait differs. */
static int
cmd_rpc_t(const char *line, unsigned mode, char *resp, unsigned rsz,
	  unsigned timeout_ms)
{
	unsigned n, rc, st;
	int sl;

	/* Build [op, mode, line NUL], truncating the
	 * line at LINKD_LINE_MAX exactly like the
	 * direct cpycat path does. */
	wr32(msg.body, 0u, LINKD_OP_CMD);
	wr32(msg.body, 4u, mode);
	n = 8u;
	{
		unsigned i = 0u;
		while (line[i] && i < LINKD_LINE_MAX) {
			msg.body[n++] = (unsigned char)line[i];
			i++;
		}
		msg.body[n++] = 0;
	}
	rc = (unsigned)rpc(LINKD_OP_CMD, n, LINKD_BUDGET_CMD,
			   timeout_ms);
	st = (unsigned)rep_status((int)rc);
	if (rc <= 0 || (int)st < 0)
		return -1;
	if (st != LINKD_OK)
		return -1;
	sl = rdstr(msg.body, rc - UIPC_HDR_SIZE, 4u, resp, rsz - 1u);
	if (sl < 0) {
		/* Over-long payload: direct uproto_cmd
		 * drops excess bytes and still returns
		 * 0, so copy what fits. */
		unsigned blen = rc - UIPC_HDR_SIZE, i;
		for (i = 0u; i + 1u < rsz && 4u + i < blen; i++)
			resp[i] = (char)msg.body[4u + i];
		resp[i] = 0;
	}
	return 0;
}

static int
cmd_rpc(const char *line, unsigned mode, char *resp, unsigned rsz)
{
	return cmd_rpc_t(line, mode, resp, rsz, LINKD_RPC_CMD_MS);
}

int
uproto_cmd(const char *cmd, char *resp, unsigned rsz)
{
	if (!cmd || !resp || rsz == 0u)
		return -1;
	return cmd_rpc(cmd, 0u, resp, rsz);
}

/* K5: uproto_cmd with a caller-supplied RPC timeout
 * (background poll use; see cmd_rpc_t). */
int
uproto_cmd_t(const char *cmd, char *resp, unsigned rsz,
	     unsigned timeout_ms)
{
	if (!cmd || !resp || rsz == 0u)
		return -1;
	return cmd_rpc_t(cmd, 0u, resp, rsz, timeout_ms);
}

int
uproto_cmd1(const char *cmd, const char *arg, char *resp,
	    unsigned rsz)
{
	char line[LINKD_LINE_MAX + 1u];
	unsigned n = 0u, i;

	if (!cmd || !arg || !resp || rsz == 0u)
		return -1;
	while (cmd[n] && n < LINKD_LINE_MAX) {
		line[n] = cmd[n];
		n++;
	}
	if (n + 1u < LINKD_LINE_MAX) {
		line[n++] = ' ';
		i = 0u;
		while (arg[i] && n < LINKD_LINE_MAX) {
			line[n++] = arg[i];
			i++;
		}
	}
	line[n] = 0;
	return cmd_rpc(line, 0u, resp, rsz);
}

int
uproto_cmd2(const char *cmd, const char *a1, const char *a2,
	    char *resp, unsigned rsz)
{
	char line[LINKD_LINE_MAX + 1u];
	unsigned n = 0u, i;

	if (!cmd || !a1 || !a2 || !resp || rsz == 0u)
		return -1;
	while (cmd[n] && n < LINKD_LINE_MAX) {
		line[n] = cmd[n];
		n++;
	}
	if (n + 1u < LINKD_LINE_MAX) {
		line[n++] = ' ';
		i = 0u;
		while (a1[i] && n < LINKD_LINE_MAX) {
			line[n++] = a1[i];
			i++;
		}
	}
	if (n + 1u < LINKD_LINE_MAX) {
		line[n++] = ' ';
		i = 0u;
		while (a2[i] && n < LINKD_LINE_MAX) {
			line[n++] = a2[i];
			i++;
		}
	}
	line[n] = 0;
	return cmd_rpc(line, 0u, resp, rsz);
}

int
uproto_cmd_retry(const char *cmd, char *resp, unsigned rsz,
		 int idem)
{
	/* idem matters only for KV.SET; line commands
	 * ignore it (the linkd classifier decides).
	 * Forward as mode 1; DENIED comes back as -1. */
	(void)idem;
	if (!cmd || !resp || rsz == 0u)
		return -1;
	return cmd_rpc(cmd, 1u, resp, rsz);
}

/* --- KV --- */

int
uproto_kv_set(const char *key, const char *val, unsigned vlen)
{
	unsigned n, rc;
	int st;

	if (!key || !key[0] || vlen > UPROTO_KV_VAL_MAX)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_KV_SET);
	wr32(msg.body, 4u, 0u);	/* plain */
	wr32(msg.body, 8u, vlen);
	n = wrstr(msg.body, 12u, key);
	{
		unsigned i;
		for (i = 0u; i < vlen; i++)
			msg.body[n + i] = (unsigned char)val[i];
		n += vlen;
	}
	rc = (unsigned)rpc(LINKD_OP_KV_SET, n, LINKD_BUDGET_KV_SET,
			   LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	return 0;
}

int
uproto_kv_set_retry(const char *key, const char *val,
		    unsigned vlen, int idem)
{
	unsigned n, rc;
	int st;

	if (!key || !key[0] || vlen > UPROTO_KV_VAL_MAX)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_KV_SET);
	wr32(msg.body, 4u, 1u);	/* retry-if-policy */
	wr32(msg.body, 8u, vlen);
	n = wrstr(msg.body, 12u, key);
	{
		unsigned i;
		for (i = 0u; i < vlen; i++)
			msg.body[n + i] = (unsigned char)val[i];
		n += vlen;
	}
	rc = (unsigned)rpc(LINKD_OP_KV_SET, n, LINKD_BUDGET_KV_SET,
			   LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	(void)idem;
	return 0;
}

int
uproto_kv_get(const char *key, char *val, unsigned vsz,
	      unsigned *vlen_out)
{
	unsigned n, rc, vlen, i;
	int st;

	if (!key || !val || vsz == 0u)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_KV_GET);
	n = wrstr(msg.body, 4u, key);
	rc = (unsigned)rpc(LINKD_OP_KV_GET, n, LINKD_BUDGET_KV_GET,
			   LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	if (rd32(msg.body, rc - UIPC_HDR_SIZE, 4u, &vlen) != 0)
		return -1;
	if (vlen_out)
		*vlen_out = vlen;
	if (vlen >= vsz) {
		/* Truncated into val, NUL-terminated,
		 * full length reported -- direct -2. */
		for (i = 0u; i + 1u < vsz; i++)
			val[i] = (char)msg.body[8u + i];
		val[i] = 0;
		return -2;
	}
	for (i = 0u; i < vlen; i++)
		val[i] = (char)msg.body[8u + i];
	val[vlen] = 0;
	return 0;
}

int
uproto_kv_del(const char *key)
{
	unsigned n, rc;
	int st;

	if (!key)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_KV_DEL);
	n = wrstr(msg.body, 4u, key);
	rc = (unsigned)rpc(LINKD_OP_KV_DEL, n, LINKD_BUDGET_KV_DEL,
			   LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	return 0;
}

/* --- NET.HTTP --- */

int
uproto_nethttp_get(const char *url, unsigned char *body,
		   unsigned cap, unsigned *len_out)
{
	unsigned n, rc, len, i;
	int st;

	if (!url || !body || cap == 0u)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_NETHTTP_GET);
	n = wrstr(msg.body, 4u, url);
	rc = (unsigned)rpc(LINKD_OP_NETHTTP_GET, n,
			   LINKD_BUDGET_NETHTTP, LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0)
		return -1;
	if (st == (int)LINKD_TRUNC)
		return -2;
	if (st != (int)LINKD_OK)
		return -1;
	if (rd32(msg.body, rc - UIPC_HDR_SIZE, 4u, &len) != 0)
		return -1;
	if (len_out)
		*len_out = len;
	if (len >= cap) {
		for (i = 0u; i + 1u < cap; i++)
			body[i] = msg.body[8u + i];
		body[i] = 0;
		return -2;
	}
	for (i = 0u; i < len; i++)
		body[i] = msg.body[8u + i];
	return 0;
}

/* --- Voice --- */

int
uproto_voice_start(unsigned rate, unsigned bits)
{
	unsigned rc;
	int st;

	wr32(msg.body, 0u, LINKD_OP_VOICE_START);
	wr32(msg.body, 4u, rate);
	wr32(msg.body, 8u, bits);
	rc = (unsigned)rpc(LINKD_OP_VOICE_START, 12u,
			   LINKD_BUDGET_VOICE, LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	return 0;
}

int
uproto_voice_stop(void)
{
	unsigned rc, frames;
	int st;

	wr32(msg.body, 0u, LINKD_OP_VOICE_STOP);
	rc = (unsigned)rpc(LINKD_OP_VOICE_STOP, 4u,
			   LINKD_BUDGET_VOICE, LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	if (rd32(msg.body, rc - UIPC_HDR_SIZE, 4u, &frames) != 0)
		return -1;
	return (int)frames;
}

int
uproto_aud_frame(const unsigned char *pcm, unsigned len)
{
	unsigned n, rc, i;
	int st;

	if (!pcm || len == 0u || len > LINK_MAX_PAYLOAD)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_AUD_FRAME);
	wr32(msg.body, 4u, len);
	for (i = 0u; i < len; i++)
		msg.body[8u + i] = pcm[i];
	n = 8u + len;
	rc = (unsigned)rpc(LINKD_OP_AUD_FRAME, n, LINKD_BUDGET_FRAME,
			   LINKD_RPC_FRAME_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return -1;
	return 0;
}

/* --- TOOL.DISPATCH --- */

int
uproto_tool_dispatch(const char *tool, const char *args_json,
		     char *result, unsigned rsz,
		     char *err, unsigned esz)
{
	unsigned n, rc, i;
	int st;

	if (!tool || !args_json || !result || rsz == 0u)
		return -1;
	if (slen_c(tool) > LINKD_FIELD_MAX ||
	    slen_c(args_json) > LINKD_DISPATCH_ARGS_MAX)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_TOOL_DISPATCH);
	n = wrstr(msg.body, 4u, tool);
	n = wrstr(msg.body, n, args_json);
	rc = (unsigned)rpc(LINKD_OP_TOOL_DISPATCH, n,
			   LINKD_BUDGET_TOOL, LINKD_RPC_CMD_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0)
		return -1;
	if (st == (int)LINKD_OK) {
		int sl = rdstr(msg.body, rc - UIPC_HDR_SIZE, 4u,
			       result, rsz - 1u);
		if (sl < 0) {
			if (err && esz > 0) {
				const char *t = "TRUNC";
				for (i = 0u; t[i] && i + 1u < esz; i++)
					err[i] = t[i];
				err[i] = 0;
			}
			return -1;
		}
		return 0;
	}
	if (st == (int)LINKD_PEER_ERR) {
		if (err && esz > 0) {
			int sl = rdstr(msg.body, rc - UIPC_HDR_SIZE,
				       4u, err, esz - 1u);
			if (sl < 0)
				err[0] = 0;
		}
		return -2;
	}
	if (st == (int)LINKD_TRUNC) {
		if (err && esz > 0) {
			const char *t = "TRUNC";
			for (i = 0u; t[i] && i + 1u < esz; i++)
				err[i] = t[i];
			err[i] = 0;
		}
		return -1;
	}
	return -1;
}

int
uproto_dispatch_result(unsigned id, const char *result_json)
{
	/* Peer dispatches go to `tools`, never to
	 * clients. There is nothing to answer here. */
	(void)id;
	(void)result_json;
	return -1;
}

/* --- WS: UNSUP via linkd (see linkd.h). --- */

int
uproto_ws_open(const char *url, unsigned *sid_out)
{
	(void)url;
	(void)sid_out;
	return -1;
}

int
uproto_ws_send(const unsigned char *msgp, unsigned len)
{
	(void)msgp;
	(void)len;
	return -1;
}

int
uproto_ws_recv(unsigned char *buf, unsigned cap,
	       unsigned *len_out)
{
	(void)buf;
	(void)cap;
	(void)len_out;
	return -1;
}

int
uproto_ws_close(unsigned sid)
{
	(void)sid;
	return -1;
}

/* --- Inbound (EVT only) --- */

int
uproto_poll_inbound(struct uproto_inb *inb, unsigned budget_ms)
{
	unsigned rc, st, seq, len;
	unsigned timeout;

	if (!inb)
		return -1;
	wr32(msg.body, 0u, LINKD_OP_EVT_WAIT);
	wr32(msg.body, 4u, evt_last_seq);
	wr32(msg.body, 8u, budget_ms);
	/* The parked wait resolves by event or by the
	 * client's own RPC timeout: budget + slack so
	 * linkd's loop turn, not the RPC, is the
	 * binding wait. Budget 0 still needs a full
	 * turn for linkd to answer TIMEOUT (linkd may
	 * be behind a slow command), so it gets a
	 * generous window rather than a 0 ms race. */
	timeout = budget_ms + 1000u;
	if (budget_ms == 0u)
		timeout = 5000u;
	rc = (unsigned)rpc(LINKD_OP_EVT_WAIT, 12u, LINKD_BUDGET_EVT,
			   timeout);
	if ((int)rc == UIPC_ETIME)
		return 0;	/* no event within budget */
	st = (unsigned)rep_status((int)rc);
	if (rc <= 0)
		return -1;	/* linkd gone, queue full, ... */
	if (st == LINKD_OK) {
		int sl;

		if (rd32(msg.body, rc - UIPC_HDR_SIZE, 4u, &seq) != 0)
			return -1;
		if (seq > evt_last_seq + 1u && evt_last_seq != 0u)
			evt_dropped_stub++;
		evt_last_seq = seq;
		sl = rdstr(msg.body, rc - UIPC_HDR_SIZE, 8u,
			   (char *)inb->payload,
			   UPROTO_INB_EVT_MAX);
		if (sl < 0)
			return -1;
		inb->kind = UPROTO_INB_EVT;
		inb->id = 0;
		inb->tool[0] = 0;
		inb->len = (unsigned)sl;
		(void)len;
		return 1;
	}
	if (st == LINKD_TIMEOUT || st == LINKD_BUSY)
		return 0;
	/* RPC ETIME (the parked wait expired client-
	 * side) arrives here as a negative rc, handled
	 * above; any other status is a link error. */
	return -1;
}

void
uproto_inb_flush(void)
{
	/* Skip to the newest ring event: ask linkd
	 * for STATUS and adopt its evt_seq. A failed
	 * STATUS leaves the cursor alone. */
	unsigned rc, seq;
	int st;

	wr32(msg.body, 0u, LINKD_OP_STATUS);
	rc = (unsigned)rpc(LINKD_OP_STATUS, 4u, LINKD_BUDGET_STATUS,
			   LINKD_RPC_STATUS_MS);
	st = rep_status((int)rc);
	if (rc <= 0 || st < 0 || st != (int)LINKD_OK)
		return;
	/* STATUS layout: [st, state, reason, 12 counters,
	 * inb_dropped, evt_dropped, evt_seq, waiters,
	 * qlimit] -- evt_seq is word 17. */
	if (rd32(msg.body, rc - UIPC_HDR_SIZE, 17u * 4u, &seq) == 0)
		evt_last_seq = seq;
}

unsigned
uproto_inb_dropped(void)
{
	return evt_dropped_stub;
}

/* --- Link state / counters (via STATUS) --- */

int
uproto_link_state(void)
{
	unsigned rc, st;
	int s;

	wr32(msg.body, 0u, LINKD_OP_STATUS);
	rc = (unsigned)rpc(LINKD_OP_STATUS, 4u, LINKD_BUDGET_STATUS,
			   LINKD_RPC_STATUS_MS);
	s = rep_status((int)rc);
	if (rc <= 0 || s < 0 || s != (int)LINKD_OK)
		return 1;	/* linkd gone: DOWN is honest */
	st = 0u;
	if (rd32(msg.body, rc - UIPC_HDR_SIZE, 4u, &st) != 0)
		return 1;
	return (int)st;
}

unsigned
uproto_link_reason(void)
{
	unsigned rc, reason;
	int s;

	wr32(msg.body, 0u, LINKD_OP_STATUS);
	rc = (unsigned)rpc(LINKD_OP_STATUS, 4u, LINKD_BUDGET_STATUS,
			   LINKD_RPC_STATUS_MS);
	s = rep_status((int)rc);
	if (rc <= 0 || s < 0 || s != (int)LINKD_OK)
		return UPROTO_LINK_REASON_DEAD;
	reason = 0u;
	if (rd32(msg.body, rc - UIPC_HDR_SIZE, 8u, &reason) != 0)
		return UPROTO_LINK_REASON_DEAD;
	return reason;
}

void
uproto_link_counters(unsigned *ok, unsigned *peer_err,
		     unsigned *timeout, unsigned *dead,
		     unsigned *crc_bad, unsigned *oversize,
		     unsigned *malformed, unsigned *resets,
		     unsigned *retries, unsigned *retry_denied,
		     unsigned *frames_sent,
		     unsigned *frames_send_fail)
{
	unsigned rc, v;
	int s;

	/* STATUS words: 0 status, 1 state, 2 reason,
	 * 3 ok, 4 peer_err, 5 timeout, 6 dead, 7 crc,
	 * 8 oversize, 9 malformed, 10 resets,
	 * 11 retries, 12 denied, 13 frames_sent,
	 * 14 frames_send_fail, ... */
	wr32(msg.body, 0u, LINKD_OP_STATUS);
	rc = (unsigned)rpc(LINKD_OP_STATUS, 4u, LINKD_BUDGET_STATUS,
			   LINKD_RPC_STATUS_MS);
	s = rep_status((int)rc);
	if (rc <= 0 || s < 0 || s != (int)LINKD_OK) {
		if (ok) *ok = 0;
		if (peer_err) *peer_err = 0;
		if (timeout) *timeout = 0;
		if (dead) *dead = 0;
		if (crc_bad) *crc_bad = 0;
		if (oversize) *oversize = 0;
		if (malformed) *malformed = 0;
		if (resets) *resets = 0;
		if (retries) *retries = 0;
		if (retry_denied) *retry_denied = 0;
		if (frames_sent) *frames_sent = 0;
		if (frames_send_fail) *frames_send_fail = 0;
		return;
	}
#define	GETCNT(idx, dst) do { \
		if (dst) { \
			v = 0u; \
			if (rd32(msg.body, rc - UIPC_HDR_SIZE, \
				 (idx) * 4u, &v) == 0) \
				*(dst) = v; \
			else \
				*(dst) = 0; \
		} \
	} while (0)
	GETCNT(3, ok);
	GETCNT(4, peer_err);
	GETCNT(5, timeout);
	GETCNT(6, dead);
	GETCNT(7, crc_bad);
	GETCNT(8, oversize);
	GETCNT(9, malformed);
	GETCNT(10, resets);
	GETCNT(11, retries);
	GETCNT(12, retry_denied);
	GETCNT(13, frames_sent);
	GETCNT(14, frames_send_fail);
#undef GETCNT
}

/* --- Pure CRC (fixed algorithm, no layer state). --- */

unsigned
voice_crc16(const unsigned char *p, unsigned len)
{
	unsigned crc = 0xffffu;
	unsigned i, j;

	for (i = 0u; i < len; i++) {
		crc ^= (unsigned)p[i] << 8;
		for (j = 0u; j < 8u; j++)
			crc = (crc & 0x8000u) ? (crc << 1) ^ 0x1021u :
			    crc << 1;
	}
	return crc & 0xffffu;
}
