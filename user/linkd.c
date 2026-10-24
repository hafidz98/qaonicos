/*
 * linkd.c - link daemon: the single transport owner (C6L-5).
 *
 * linkd claims the reserved IPC name `link` (kernel-
 * assigned progid 4, daemon_images index 4) and serves
 * guest clients over LINKD-IPC v1 (see
 * user/uartproto/linkd.h for the full contract). It
 * owns the single backend -- bridge in QEMU, UART on
 * hardware -- and no other daemon touches the
 * transport after this phase. Clients (uiapp screens
 * via the linkcli stub, voice clients, the shell
 * `link` command, tests) send commands, KV ops,
 * TOOL.DISPATCH requests, voice frames, EVT waits,
 * and STATUS queries as IPC messages; linkd
 * serialises them onto the strictly alternating wire
 * and answers each on the requester's own send-once
 * reply name, so responses can never cross clients.
 *
 * Peer-initiated dispatches (wire DISPATCH units) are
 * forwarded to qabotd over IPC name `tools`
 * (SYS_RPC 12, LINKD_TOOL_RPC_ID below); linkd
 * never executes, confirms, or refuses a peer tool
 * itself. If no tools consumer answers, the peer
 * gets {"ok":false,"error":"tools_unavailable"} --
 * never silence. EVT units fan out to parked
 * EVT_WAIT waiters and to the broadcast ring.
 *
 * Loop shape (the watchdog rule): park in blocking
 * IPC receive with a 100 ms deadline, drain inbound
 * units between receives (at most 8 per turn), yield
 * every turn. A transport reopen attempt is throttled
 * to at most one per 2000 ms while the link is DOWN,
 * so a dead peer cannot make the idle loop burn open
 * attempts; commands still attempt their own lazy
 * reopen per C6L-4 (bounded open cost, never a hang).
 *
 * Service port qlimit: the IPC-5 default (5). The
 * voice-rate measurement that justifies keeping it
 * (or raising it to 16) is recorded in
 * docs/rencana-komunikasi-c6/DEVLOG-C6L5.md.
 *
 * No struct assignment / bulk copies anywhere
 * (user programs link without __aeabi_memcpy4):
 * payloads are filled by loops, words by byte-wise
 * helpers, and the two message buffers are static
 * and reused in place (the tools RPC buffer is
 * separate from the request buffer because
 * forwarding runs outside request handling, but
 * never nested inside it).
 */
#include "ulib/uipc.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"
#include "uartproto/linkd.h"

/* Tools-consumer protocol (linkd -> `tools`):
 * RPC id 0x3101, body = tool NUL + args-json NUL;
 * reply id echoes 0x3101, body = result JSON NUL.
 * The consumer applies the C6L-3 policy mirror
 * (ALLOW/CONFIRM-needs_confirm/BLOCK-refused);
 * production qabotd implements this same shape. */
#define	LINKD_TOOL_RPC_ID	0x3101u

static struct uipc_msg	msg;	/* IPC request + reply, in place */
static struct uipc_msg	tmsg;	/* tools forward RPC */
static unsigned		port;

/* EVT broadcast ring + waiters (see linkd.h). */
static unsigned		evt_seq;	/* last assigned, from 1 */
static unsigned		evt_dropped;	/* ring overwrites */
static unsigned		evt_head;	/* next write slot */
static unsigned		evt_count;
static unsigned		evt_len[LINKD_EVT_RING_N];
static unsigned		evt_seqn[LINKD_EVT_RING_N];
static unsigned char	evt_pay[LINKD_EVT_RING_N][UPROTO_INB_EVT_MAX];
static unsigned		wait_once[LINKD_WAITERS_MAX];
static unsigned		wait_last[LINKD_WAITERS_MAX];
static unsigned		nwaiters;
static unsigned		last_open_try;

static void
putu(unsigned v)
{
	char b[12];
	int i = 0;
	if (v == 0u) {
		u_put("0");
		return;
	}
	while (v != 0u) {
		b[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	}
	while (i > 0) {
		char s[2];
		s[0] = b[--i];
		s[1] = '\0';
		u_put(s);
	}
}

static void
puti(int rc)
{
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
}

/* --- Message field helpers (byte-wise; no alignment
 * assumptions, no bulk copies). Offsets are byte
 * offsets into the message BODY (after the 24-byte
 * header). --- */

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

static void
wr32(unsigned char *b, unsigned off, unsigned v)
{
	b[off] = (unsigned char)(v & 0xFFu);
	b[off + 1u] = (unsigned char)((v >> 8) & 0xFFu);
	b[off + 2u] = (unsigned char)((v >> 16) & 0xFFu);
	b[off + 3u] = (unsigned char)((v >> 24) & 0xFFu);
}

/* Copy a NUL-terminated string starting at byte off.
 * Returns its length (excluding NUL), or -1 if no
 * NUL is found within the body or the length exceeds
 * cap. The destination is always NUL-terminated. */
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

/* Reply to the request in msg, reusing its buffer.
 * len is the TOTAL reply size. Returns the reply rc
 * (nonzero only when the requester is gone -- a dead
 * client is not linkd's failure). */
static int
reply_now(unsigned len)
{
	unsigned once = msg.reply;
	unsigned op = (unsigned)msg.id;
	int rc;

	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = (int)op;
	msg.size = len;
	rc = uipc_reply(once, &msg, len);
	if (rc != 0) {
		/* A reply fails (only) when the client
		 * died after sending: log it, never
		 * treat it as a linkd error. The dead
		 * client's request was already
		 * executed; other clients are
		 * unaffected (C6L-5 death leg). */
		u_put("LINKD: reply fail op=");
		putu(op);
		u_put(" rc=");
		puti(rc);
		u_put("\n");
	}
	return rc;
}

/* Reply [status] only. */
static int
reply_status(unsigned status)
{
	wr32(msg.body, 0u, status);
	return reply_now(UIPC_HDR_SIZE + 4u);
}

/* Reply [status, string NUL]. */
static int
reply_status_str(unsigned status, const char *s)
{
	unsigned n;

	wr32(msg.body, 0u, status);
	n = wrstr(msg.body, 4u, s);
	return reply_now(UIPC_HDR_SIZE + n);
}

/* Current link-failure reason as a string. */
static const char *
reason_str(void)
{
	switch (uproto_link_reason()) {
	case UPROTO_LINK_REASON_TIMEOUT:
		return "TIMEOUT";
	case UPROTO_LINK_REASON_DEAD:
		return "DEAD";
	case UPROTO_LINK_REASON_CRC:
		return "CRC";
	case UPROTO_LINK_REASON_OVERSIZE:
		return "OVERSIZE";
	case UPROTO_LINK_REASON_MALFORMED:
		return "MALFORMED";
	default:
		return "NONE";
	}
}

/* Reply for a failed wire call: DOWN -> LINK_FAIL +
 * reason; UP -> PEER_ERR with an empty code (the
 * direct helpers do not carry the wire code). */
static int
reply_wire_fail(void)
{
	if (uproto_link_state() == UPROTO_LINK_DOWN)
		return reply_status_str(LINKD_LINK_FAIL,
					reason_str());
	return reply_status_str(LINKD_PEER_ERR, "");
}

/* --- EVT ring + waiters --- */

static void
evt_ring_push(const unsigned char *pay, unsigned len)
{
	unsigned s = evt_head;

	if (evt_count >= LINKD_EVT_RING_N)
		evt_dropped++;
	else
		evt_count++;
	evt_seq++;
	evt_seqn[s] = evt_seq;
	evt_len[s] = len;
	{
		unsigned i;
		for (i = 0u; i < len; i++)
			evt_pay[s][i] = pay[i];
	}
	evt_head = (evt_head + 1u) % LINKD_EVT_RING_N;
}

/* Oldest ring event with seq > last, or -1. The
 * caller copies the payload out before any send
 * (the ring is static, the reply buffer is not). */
static int
evt_ring_next(unsigned last, unsigned *seq_out,
	      unsigned char *pay, unsigned *len_out)
{
	unsigned best = 0u, bi = 0u, i;
	int found = 0;

	for (i = 0u; i < evt_count; i++) {
		unsigned s = (evt_head + LINKD_EVT_RING_N -
		    evt_count + i) % LINKD_EVT_RING_N;
		if (evt_seqn[s] > last &&
		    (!found || evt_seqn[s] < best)) {
			found = 1;
			best = evt_seqn[s];
			bi = s;
		}
	}
	if (!found)
		return -1;
	*seq_out = best;
	*len_out = evt_len[bi];
	{
		unsigned j;
		for (j = 0u; j < evt_len[bi]; j++)
			pay[j] = evt_pay[bi][j];
	}
	return 0;
}

static void
waiter_drop(unsigned idx)
{
	unsigned i;

	for (i = idx + 1u; i < nwaiters; i++) {
		wait_once[i - 1u] = wait_once[i];
		wait_last[i - 1u] = wait_last[i];
	}
	nwaiters--;
}

/* Fan an event out: ring first, then every parked
 * waiter. A failed reply drops that waiter silently
 * (stale waiter, not an error); the event stays in
 * the ring for the next poll. */
static void
evt_fanout(const unsigned char *pay, unsigned len)
{
	unsigned seq, i;
	unsigned char line[UPROTO_INB_EVT_MAX + 1u];

	for (i = 0u; i < len; i++)
		line[i] = pay[i];
	line[len] = 0;
	evt_ring_push(pay, len);
	seq = evt_seq;
	i = 0u;
	while (i < nwaiters) {
		struct uipc_msg *r = &msg;
		unsigned once = wait_once[i];
		unsigned n, rc;

		r->reply = 0u;
		r->dest = 0u;
		r->id = (int)LINKD_OP_EVT_WAIT;
		wr32(r->body, 0u, LINKD_OK);
		wr32(r->body, 4u, seq);
		n = wrstr(r->body, 8u, (const char *)line);
		r->size = UIPC_HDR_SIZE + n;
		rc = uipc_reply(once, r, UIPC_HDR_SIZE + n);
		if ((int)rc != 0) {
			waiter_drop(i);
			continue;
		}
		waiter_drop(i);
	}
}

/* --- Peer-dispatch forwarding --- */

static void
forward_dispatch(unsigned id, const char *tool,
		 const unsigned char *args, unsigned alen)
{
	char result[1024];
	char *rp = result;
	unsigned n, i, rc;
	int rrc;

	/* Build the tools RPC: [tool NUL][args NUL].
	 * The RPC len is BOTH the send size and the
	 * reply budget: passing the bare request
	 * length (e.g. 36 for get_info) made every
	 * tools reply E2BIG -- the result JSON
	 * (~65 B) exceeded it, the reply was lost,
	 * and linkd answered tools_unavailable for
	 * every dispatch even with a live tools
	 * server (C6L-5 third run: 480/480
	 * unavailable). Pad to a budget covering
	 * the largest result this function accepts
	 * (1024 B); the consumer parses by NUL
	 * termination and ignores trailing zeros. */
	n = 0u;
	while (tool[n] && n < 63u) {
		tmsg.body[n] = (unsigned char)tool[n];
		n++;
	}
	tmsg.body[n++] = 0;
	for (i = 0u; i < alen && n + 1u < 1024u; i++)
		tmsg.body[n++] = args[i];
	tmsg.body[n++] = 0;
	for (i = n; i < 1024u; i++)
		tmsg.body[i] = 0;
	tmsg.reply = 0u;
	tmsg.dest = 0u;
	tmsg.id = (int)LINKD_TOOL_RPC_ID;
	tmsg.size = UIPC_HDR_SIZE + 1024u;
	rrc = uipc_rpc_user("tools", &tmsg, UIPC_HDR_SIZE + 1024u,
			    LINKD_TOOLS_RPC_MS);
	if (rrc > 0 && tmsg.id == (int)LINKD_TOOL_RPC_ID) {
		unsigned blen = (unsigned)rrc - UIPC_HDR_SIZE;
		int sl = rdstr(tmsg.body, blen, 0u, result, 1023u);
		if (sl >= 0) {
			/* result JSON in hand; answer the peer. */
			if (uproto_dispatch_result(id, result) != 0) {
				u_put("LINKD: dispatch result send fail\n");
			}
			return;
		}
	}
	/* No tools consumer, or a malformed reply: the
	 * peer still gets an answer, never silence. */
	{
		const char *un = "{\"ok\":false,\"error\":\"tools_unavailable\"}";
		for (i = 0u; un[i]; i++)
			rp[i] = un[i];
		rp[i] = 0;
	}
	if (uproto_dispatch_result(id, result) != 0)
		u_put("LINKD: dispatch result send fail\n");
}

/* --- Request handlers (each replies exactly once,
 * except EVT_WAIT parks, which replies later). --- */

static void
handle_cmd(const unsigned char *b, unsigned blen)
{
	char line[LINKD_LINE_MAX + 1u];
	char resp[2048];
	unsigned mode;
	int rc;

	if (rd32(b, blen, 4u, &mode) != 0 ||
	    rdstr(b, blen, 8u, line, LINKD_LINE_MAX) < 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	if (mode == 1u)
		rc = uproto_cmd_retry(line, resp, sizeof(resp), 0);
	else
		rc = uproto_cmd(line, resp, sizeof(resp));
	if (rc == 0) {
		reply_status_str(LINKD_OK, resp);
		return;
	}
	if (mode == 1u && uproto_link_state() == UPROTO_LINK_DOWN &&
	    !uproto_retry_policy(line, 0)) {
		reply_status(LINKD_DENIED);
		return;
	}
	reply_wire_fail();
}

static void
handle_kv_set(const unsigned char *b, unsigned blen)
{
	char key[LINKD_FIELD_MAX + 1u];
	char val[LINKD_KV_VAL_MAX];
	unsigned mode, vlen, klen, i;
	int rc;

	if (rd32(b, blen, 4u, &mode) != 0 ||
	    rd32(b, blen, 8u, &vlen) != 0 ||
	    vlen > LINKD_KV_VAL_MAX) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	klen = 0u;
	{
		int kl = rdstr(b, blen, 12u, key, LINKD_FIELD_MAX);
		if (kl < 0) {
			reply_status_str(LINKD_PEER_ERR, "ARG");
			return;
		}
		klen = (unsigned)kl;
	}
	if (12u + klen + 1u + vlen > blen) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	for (i = 0u; i < vlen; i++)
		val[i] = (char)b[12u + klen + 1u + i];
	if (mode == 1u)
		rc = uproto_kv_set_retry(key, val, vlen, 1);
	else
		rc = uproto_kv_set(key, val, vlen);
	if (rc == 0) {
		reply_status(LINKD_OK);
		return;
	}
	if (mode == 1u && uproto_link_state() == UPROTO_LINK_DOWN &&
	    !uproto_retry_policy("KV.SET", 1)) {
		reply_status(LINKD_DENIED);
		return;
	}
	reply_wire_fail();
}

static void
handle_kv_get(const unsigned char *b, unsigned blen)
{
	char key[LINKD_FIELD_MAX + 1u];
	char val[128];
	unsigned vlen = 0u, i;
	int rc;

	if (rdstr(b, blen, 4u, key, LINKD_FIELD_MAX) < 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	rc = uproto_kv_get(key, val, sizeof(val), &vlen);
	if (rc == 0 || rc == -2) {
		/* -2 cannot occur: linkd's buffer (128)
		 * exceeds the store cap (64). Treat it
		 * as TRUNC if it ever does. */
		unsigned n;

		if (rc == -2) {
			reply_status(LINKD_TRUNC);
			return;
		}
		wr32(msg.body, 0u, LINKD_OK);
		wr32(msg.body, 4u, vlen);
		for (i = 0u; i < vlen; i++)
			msg.body[8u + i] = (unsigned char)val[i];
		n = 8u + vlen;
		reply_now(UIPC_HDR_SIZE + n);
		return;
	}
	reply_wire_fail();
}

static void
handle_kv_del(const unsigned char *b, unsigned blen)
{
	char key[LINKD_FIELD_MAX + 1u];
	int rc;

	if (rdstr(b, blen, 4u, key, LINKD_FIELD_MAX) < 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	rc = uproto_kv_del(key);
	if (rc == 0) {
		reply_status(LINKD_OK);
		return;
	}
	reply_wire_fail();
}

static void
handle_tool(const unsigned char *b, unsigned blen)
{
	char tool[LINKD_FIELD_MAX + 1u];
	char args[LINKD_DISPATCH_ARGS_MAX + 1u];
	char result[1024];
	char err[32];
	int tl, al, rc;

	tl = rdstr(b, blen, 4u, tool, LINKD_FIELD_MAX);
	if (tl < 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	al = rdstr(b, blen, 4u + (unsigned)tl + 1u, args,
		   LINKD_DISPATCH_ARGS_MAX);
	if (al < 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	rc = uproto_tool_dispatch(tool, args, result, sizeof(result),
				  err, sizeof(err));
	if (rc == 0) {
		reply_status_str(LINKD_OK, result);
		return;
	}
	if (rc == -2) {
		reply_status_str(LINKD_PEER_ERR, err);
		return;
	}
	/* -1: TRUNC, else wire failure. */
	{
		unsigned i;
		int is_trunc = 1;

		{
			const char *t = "TRUNC";
			for (i = 0u; t[i]; i++)
				if (err[i] != t[i])
					is_trunc = 0;
			if (err[i] != 0)
				is_trunc = 0;
		}
		if (is_trunc) {
			reply_status(LINKD_TRUNC);
			return;
		}
	}
	reply_wire_fail();
}

static void
handle_nethttp(const unsigned char *b, unsigned blen)
{
	char url[LINKD_FIELD_MAX + 1u];
	unsigned char body[1024];
	unsigned len = 0u, i;
	int rc;

	if (rdstr(b, blen, 4u, url, LINKD_FIELD_MAX) < 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	rc = uproto_nethttp_get(url, body, sizeof(body), &len);
	if (rc == 0 || rc == -2) {
		unsigned n;

		if (rc == -2) {
			reply_status(LINKD_TRUNC);
			return;
		}
		wr32(msg.body, 0u, LINKD_OK);
		wr32(msg.body, 4u, len);
		for (i = 0u; i < len; i++)
			msg.body[8u + i] = body[i];
		n = 8u + len;
		reply_now(UIPC_HDR_SIZE + n);
		return;
	}
	reply_wire_fail();
}

static void
handle_voice_start(const unsigned char *b, unsigned blen)
{
	unsigned rate, bits;
	int rc;

	if (rd32(b, blen, 4u, &rate) != 0 ||
	    rd32(b, blen, 8u, &bits) != 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	rc = uproto_voice_start(rate, bits);
	if (rc == 0) {
		reply_status(LINKD_OK);
		return;
	}
	reply_wire_fail();
}

static void
handle_voice_stop(void)
{
	int rc = uproto_voice_stop();

	if (rc >= 0) {
		wr32(msg.body, 0u, LINKD_OK);
		wr32(msg.body, 4u, (unsigned)rc);
		reply_now(UIPC_HDR_SIZE + 8u);
		return;
	}
	reply_wire_fail();
}

static void
handle_aud_frame(const unsigned char *b, unsigned blen)
{
	unsigned len, i;
	int rc;

	if (rd32(b, blen, 4u, &len) != 0 ||
	    len > LINK_MAX_PAYLOAD ||
	    8u + len > blen) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	/* uproto_aud_frame copies the payload into its
	 * own frame buffer; the request buffer is safe
	 * to hand over by pointer for the copy loop. */
	rc = uproto_aud_frame(b + 8u, len);
	if (rc == 0) {
		reply_status(LINKD_OK);
		return;
	}
	reply_wire_fail();
}

static void
handle_evt_wait(const unsigned char *b, unsigned blen)
{
	unsigned last, budget, seq, len;
	unsigned char pay[UPROTO_INB_EVT_MAX + 1u];
	unsigned n;

	if (rd32(b, blen, 4u, &last) != 0 ||
	    rd32(b, blen, 8u, &budget) != 0) {
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	if (evt_ring_next(last, &seq, pay, &len) == 0) {
		pay[len] = 0;
		wr32(msg.body, 0u, LINKD_OK);
		wr32(msg.body, 4u, seq);
		n = wrstr(msg.body, 8u, (const char *)pay);
		reply_now(UIPC_HDR_SIZE + n);
		return;
	}
	if (budget == 0u) {
		reply_status(LINKD_TIMEOUT);
		return;
	}
	if (nwaiters >= LINKD_WAITERS_MAX) {
		reply_status(LINKD_BUSY);
		return;
	}
	/* Park: store the send-once reply name, answer
	 * later from evt_fanout. No reply now. */
	wait_once[nwaiters] = msg.reply;
	wait_last[nwaiters] = last;
	nwaiters++;
}

static void
handle_status(void)
{
	unsigned ok, perr, tout, dead, crc, over, malf;
	unsigned resets, retries, denied, fsent, ffail;
	unsigned st, reason, n;

	uproto_link_counters(&ok, &perr, &tout, &dead, &crc,
			     &over, &malf, &resets, &retries,
			     &denied, &fsent, &ffail);
	st = (unsigned)uproto_link_state();
	reason = uproto_link_reason();
	n = 0u;
	wr32(msg.body, n, LINKD_OK); n += 4u;
	wr32(msg.body, n, st); n += 4u;
	wr32(msg.body, n, reason); n += 4u;
	wr32(msg.body, n, ok); n += 4u;
	wr32(msg.body, n, perr); n += 4u;
	wr32(msg.body, n, tout); n += 4u;
	wr32(msg.body, n, dead); n += 4u;
	wr32(msg.body, n, crc); n += 4u;
	wr32(msg.body, n, over); n += 4u;
	wr32(msg.body, n, malf); n += 4u;
	wr32(msg.body, n, resets); n += 4u;
	wr32(msg.body, n, retries); n += 4u;
	wr32(msg.body, n, denied); n += 4u;
	wr32(msg.body, n, fsent); n += 4u;
	wr32(msg.body, n, ffail); n += 4u;
	wr32(msg.body, n, uproto_inb_dropped()); n += 4u;
	wr32(msg.body, n, evt_dropped); n += 4u;
	wr32(msg.body, n, evt_seq); n += 4u;
	wr32(msg.body, n, nwaiters); n += 4u;
	wr32(msg.body, n, 5u); n += 4u;	/* qlimit: default */
	reply_now(UIPC_HDR_SIZE + n);
}

/* Dispatch one IPC request. Replies exactly once,
 * except EVT_WAIT parks (replied later by fanout). */
static void
handle_request(unsigned blen)
{
	unsigned op;
	const unsigned char *b = msg.body;

	if (rd32(b, blen, 0u, &op) != 0) {
		msg.id = 0;
		reply_status_str(LINKD_PEER_ERR, "ARG");
		return;
	}
	switch (op) {
	case LINKD_OP_CMD:
		handle_cmd(b, blen);
		break;
	case LINKD_OP_KV_SET:
		handle_kv_set(b, blen);
		break;
	case LINKD_OP_KV_GET:
		handle_kv_get(b, blen);
		break;
	case LINKD_OP_KV_DEL:
		handle_kv_del(b, blen);
		break;
	case LINKD_OP_TOOL_DISPATCH:
		handle_tool(b, blen);
		break;
	case LINKD_OP_NETHTTP_GET:
		handle_nethttp(b, blen);
		break;
	case LINKD_OP_VOICE_START:
		handle_voice_start(b, blen);
		break;
	case LINKD_OP_VOICE_STOP:
		handle_voice_stop();
		break;
	case LINKD_OP_AUD_FRAME:
		handle_aud_frame(b, blen);
		break;
	case LINKD_OP_EVT_WAIT:
		handle_evt_wait(b, blen);
		break;
	case LINKD_OP_STATUS:
		handle_status();
		break;
	default:
		/* WS family + DISPATCH_RESULT + unknown:
		 * accepted, answered UNSUP, wire untouched. */
		reply_status(LINKD_UNSUP);
		break;
	}
}

/* One inbound unit from the link: EVT fans out,
 * DISPATCH forwards to `tools`. */
static void
handle_unit(struct uproto_inb *inb)
{
	if (inb->kind == UPROTO_INB_EVT) {
		evt_fanout(inb->payload, inb->len);
		return;
	}
	if (inb->kind == UPROTO_INB_DISPATCH) {
		forward_dispatch(inb->id, inb->tool, inb->payload,
				 inb->len);
		return;
	}
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;

	rc = uipc_port_alloc();
	if (rc < 0) {
		u_put("LINKD: FAIL alloc rc=");
		puti(rc);
		u_put("\n");
		sys_exit(1);
	}
	port = (unsigned)rc;
	rc = uipc_register("link", port);
	if (rc != 0) {
		u_put("LINKD: FAIL register link rc=");
		puti(rc);
		u_put("\n");
		sys_exit(1);
	}
	uproto_backend_select(&uproto_backend_bridge);
	u_put("LINKD: registered link\n");

	for (;;) {
		struct uproto_inb inb;
		unsigned drained = 0u;

		/* Drain inbound units first (queued units
		 * from during a command land here too),
		 * capped per turn so IPC service cannot
		 * starve behind a flooding peer. While
		 * DOWN, reopen attempts are throttled:
		 * the idle loop serves IPC only until
		 * the throttle window passes. */
		for (;;) {
			int r;

			if (drained >= LINKD_DRAIN_MAX)
				break;
			if (uproto_link_state() == UPROTO_LINK_DOWN &&
			    sys_uptime() - last_open_try <
			    LINKD_OPEN_THROTTLE_MS)
				break;
			last_open_try = sys_uptime();
			r = uproto_poll_inbound(&inb, 0u);
			if (r == 1) {
				handle_unit(&inb);
				drained++;
				continue;
			}
			break;
		}

		rc = uipc_recv(port, &msg, UIPC_MSG_MAX,
			       (int)LINKD_RECV_DEADLINE_MS);
		if (rc == UIPC_ETIME) {
			sys_yield();
			continue;
		}
		if (rc < 0) {
			u_put("LINKD: recv rc=");
			puti(rc);
			u_put("\n");
			sys_yield();
			continue;
		}
		handle_request((unsigned)rc - UIPC_HDR_SIZE);
		sys_yield();
	}
}
