/*
 * uartproto.c - link layer: framing + backend dispatch (C6L-1).
 *
 * Text commands: "CMD arg..\n" -> "OK payload.." / "ERR kode".
 * Multi-line responses (WIFI.SCAN, WIFI.LIST) are
 * count-prefixed: first line "OK <n>", then exactly n
 * lines; the reader consumes exactly that many.
 * KV values are length-prefixed (see uproto_kv_*).
 * Binary frames use the frozen v2 format (uartproto.h).
 *
 * The link layer never touches transport bytes except
 * through the selected backend (link.h). Responses are
 * streamed: the reader pulls backend chunks into the
 * caller's buffer, so no fixed staging cap bounds a
 * response -- only the caller's buffer does.
 *
 * Protocol discipline (see WIRE-FREEZE.md for v1.1 and
 * WIRE-FREEZE-V1.2.md for the C6L-3 additions): strict
 * request/response alternation, except for the two
 * inbound unit kinds: EVT frames and peer DISPATCH
 * lines may arrive at any time and are queued by the
 * reader (see uproto_poll_inbound), never dropped.
 * A peer that sends other unsolicited bytes, or a
 * malformed length that stalls the stream, is a
 * link-reset event: the caller must close + reopen
 * the backend (uproto_backend_select), which clears
 * the byte stream but keeps queued inbound units.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"

#define TX_SZ	160

/* --- Backend selection --- */

static struct uproto_backend *cur_be = &uproto_backend_mock;
static int be_opened = 0;

/* Byte-stream staging (declared here because
 * uproto_backend_select() clears it on reopen; the
 * reader itself follows below). */
#define	RD_CHUNK	32u
static unsigned char rd_buf[RD_CHUNK];
static unsigned rd_len = 0, rd_pos = 0;
/* Two-byte pushback: the unit parser peeks at the
 * first two bytes to tell a binary frame ("QA") from
 * a text line starting with 'Q'. */
static unsigned char pb_buf[2];
static unsigned pb_n = 0;

void
uproto_backend_select(struct uproto_backend *be)
{
	if (be_opened && cur_be->close)
		cur_be->close();
	cur_be = be ? be : &uproto_backend_mock;
	be_opened = 0;
	/* New transport, fresh byte stream: drop staged
	 * bytes and pushback (a previous failed exchange
	 * must not poison the next one). Queued inbound
	 * units are application-level and survive: a
	 * dispatch queued during a timed-out command is
	 * still answerable after the reopen. */
	rd_len = rd_pos = 0;
	pb_n = 0;
}

struct uproto_backend *
uproto_backend_current(void)
{
	return cur_be;
}

static int
be_ensure_open(void)
{
	if (be_opened)
		return 0;
	if (cur_be->open && cur_be->open() != 0)
		return -1;
	be_opened = 1;
	return 0;
}

/* --- Streaming byte reader over the backend --- */

/* One byte, or -1 on timeout/link death. Elapsed-based
 * deadline so sys_uptime() wraparound is harmless. */
static int
rd_byte(unsigned timeout_ms)
{
	unsigned start = sys_uptime();
	int n;

	for (;;) {
		if (rd_pos < rd_len)
			return rd_buf[rd_pos++];
		n = cur_be->read(rd_buf, sizeof(rd_buf));
		if (n < 0)
			return -1;
		if (n > 0) {
			rd_len = (unsigned)n;
			rd_pos = 0;
			continue;
		}
		if (sys_uptime() - start >= timeout_ms)
			return -1;
	}
}

/* Next byte, pushback first, then the backend. Same
 * -1 contract as rd_byte. */
static int
next_byte(unsigned timeout_ms)
{
	int c;
	unsigned i;

	if (pb_n > 0) {
		c = pb_buf[0];
		for (i = 1; i < pb_n; i++)
			pb_buf[i - 1u] = pb_buf[i];
		pb_n--;
		return c;
	}
	return rd_byte(timeout_ms);
}

static void
push_byte(unsigned char c)
{
	if (pb_n < 2u)
		pb_buf[pb_n++] = c;
}

/* Read exactly len bytes into buf. Returns 0, or -1 on
 * timeout/link death (partial bytes stay consumed: the
 * stream is desynchronised and the caller must reopen
 * the backend). */
static int
rd_exact(unsigned char *buf, unsigned len)
{
	unsigned o = 0;
	int c;

	while (o < len) {
		c = next_byte(UPROTO_CMD_TIMEOUT_MS);
		if (c < 0)
			return -1;
		buf[o++] = (unsigned char)c;
	}
	return 0;
}

/* --- Units: one text line, or one binary frame ---
 *
 * The stream mixes both. A frame starts with the "QA"
 * magic; anything else at a unit boundary is a text
 * line. A line starting with 'Q' but not "QA" is
 * pushed back and read as text. CRC is checked over
 * header + payload (voice_crc16 is whole-buffer, so
 * the frame is reassembled in a static buffer first;
 * the parser is never re-entered, so static is safe).
 * Frame payloads are capped at LINK_MAX_PAYLOAD; a
 * larger declared length is a link-reset error.
 *
 * Returns 1 (unit: kind TEXT -> line[], kind FRAME ->
 * ftype + frame payload in frame_tmp[HDR..HDR+len)),
 * 0 (no byte at all within timeout_ms: pure silence),
 * -1 (mid-unit timeout, bad CRC, oversize length).
 * Silence vs error matters to the poll loop: silence
 * means "try again", error means "reopen". The frame
 * buffer is shared with send_bin_frame (never live
 * at the same time: consumers copy a parsed frame
 * out before any send happens), so there is exactly
 * one 1 KB frame buffer in the link layer, not two. */
#define	UNIT_TEXT	0u
#define	UNIT_FRAME	1u

static unsigned char frame_tmp[LINK_FRAME_HDR + LINK_MAX_PAYLOAD +
    LINK_FRAME_CRC];

static int
read_unit(unsigned timeout_ms, unsigned *kind_out, unsigned *ftype_out,
	  unsigned *len_out, char *line, unsigned linecap)
{
	int c0, c1, t, hi, lo;
	unsigned len, i, o;
	unsigned crc, expect;

	c0 = next_byte(timeout_ms);
	if (c0 < 0)
		return 0;
	if (c0 == LINK_FRAME_MAGIC0) {
		c1 = next_byte(timeout_ms);
		if (c1 < 0)
			return -1;
		if (c1 != LINK_FRAME_MAGIC1) {
			/* Text line starting with 'Q'. */
			push_byte((unsigned char)c0);
			push_byte((unsigned char)c1);
		} else {
			t = next_byte(timeout_ms);
			if (t < 0)
				return -1;
			hi = next_byte(timeout_ms);
			if (hi < 0)
				return -1;
			lo = next_byte(timeout_ms);
			if (lo < 0)
				return -1;
			len = ((unsigned)hi << 8) | (unsigned)lo;
			if (len > LINK_MAX_PAYLOAD)
				return -1;
			for (i = 0; i < len; i++) {
				c0 = next_byte(timeout_ms);
				if (c0 < 0)
					return -1;
				frame_tmp[LINK_FRAME_HDR + i] = (unsigned char)c0;
			}
			hi = next_byte(timeout_ms);
			if (hi < 0)
				return -1;
			lo = next_byte(timeout_ms);
			if (lo < 0)
				return -1;
			crc = ((unsigned)hi << 8) | (unsigned)lo;
			frame_tmp[0] = LINK_FRAME_MAGIC0;
			frame_tmp[1] = LINK_FRAME_MAGIC1;
			frame_tmp[2] = (unsigned char)t;
			frame_tmp[3] = (unsigned char)(len >> 8);
			frame_tmp[4] = (unsigned char)(len & 0xffu);
			expect = voice_crc16(frame_tmp,
			    LINK_FRAME_HDR + len);
			if (crc != expect)
				return -1;
			*kind_out = UNIT_FRAME;
			*ftype_out = (unsigned)t;
			*len_out = len;
			return 1;
		}
	} else {
		push_byte((unsigned char)c0);
	}
	/* Text line. */
	o = 0;
	for (;;) {
		c0 = next_byte(timeout_ms);
		if (c0 < 0)
			return -1;
		if (c0 == '\n') {
			line[o] = 0;
			*kind_out = UNIT_TEXT;
			*ftype_out = 0;
			*len_out = o;
			return 1;
		}
		if (c0 == '\r')
			continue;
		if (o + 1 >= linecap)
			return -1;
		line[o++] = (char)c0;
	}
}

/* --- Inbound queue (EVT + DISPATCH units) --- */

#define	INB_Q_N	4u

static struct uproto_inb inb_q[INB_Q_N];
static unsigned inb_q_len = 0;
static unsigned inb_dropped_n = 0;

static int
inb_push(unsigned kind, unsigned id, const char *tool,
	 const unsigned char *payload, unsigned len)
{
	struct uproto_inb *e;
	unsigned i;

	if (inb_q_len >= INB_Q_N) {
		inb_dropped_n++;
		return -1;
	}
	e = &inb_q[inb_q_len++];
	e->kind = kind;
	e->id = id;
	e->len = len;
	for (i = 0; tool && tool[i] &&
	    i < UPROTO_INB_TOOL_MAX; i++)
		e->tool[i] = tool[i];
	e->tool[i] = 0;
	for (i = 0; i < len && i < sizeof(e->payload); i++)
		e->payload[i] = payload[i];
	return 0;
}

/* Copy one queue entry out, field by field. Deliberately
 * NOT a struct assignment: user programs link without
 * __aeabi_memcpy4 (only the kernel's aeabi.c provides
 * it), and clang lowers a struct copy to that call.
 * All loops below are runtime-bounded (NUL terminator
 * / len), so they stay loops and never become a
 * memcpy call either. */
static void
inb_copy(struct uproto_inb *dst, const struct uproto_inb *src)
{
	unsigned j;

	dst->kind = src->kind;
	dst->id = src->id;
	dst->len = src->len;
	for (j = 0;; j++) {
		dst->tool[j] = src->tool[j];
		if (src->tool[j] == 0)
			break;
	}
	for (j = 0; j < src->len; j++)
		dst->payload[j] = src->payload[j];
}

static int
inb_pop(struct uproto_inb *out)
{
	unsigned i;

	if (inb_q_len == 0)
		return 0;
	inb_copy(out, &inb_q[0]);
	for (i = 1; i < inb_q_len; i++)
		inb_copy(&inb_q[i - 1u], &inb_q[i]);
	inb_q_len--;
	return 1;
}

void
uproto_inb_flush(void)
{
	inb_q_len = 0;
}

unsigned
uproto_inb_dropped(void)
{
	return inb_dropped_n;
}

/* Parse "DISPATCH <id> <tool> <len>" (line without '\n').
 * Returns 0 with id/tool/len filled, or -1. */
static int
parse_dispatch_line(const char *line, unsigned *id_out, char *tool,
		    unsigned tcap, unsigned *len_out)
{
	unsigned i = 0, o = 0, id = 0, len = 0;
	int digits = 0;

	if (line[0] != 'D' || line[1] != 'I' || line[2] != 'S' ||
	    line[3] != 'P' || line[4] != 'A' || line[5] != 'T' ||
	    line[6] != 'C' || line[7] != 'H' || line[8] != ' ')
		return -1;
	i = 9;
	while (line[i] >= '0' && line[i] <= '9') {
		id = id * 10u + (unsigned)(line[i] - '0');
		digits = 1;
		i++;
	}
	if (!digits || line[i] != ' ')
		return -1;
	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && o + 1 < tcap)
		tool[o++] = line[i++];
	if (o == 0 || line[i] != ' ')
		return -1;
	tool[o] = 0;
	while (line[i] == ' ')
		i++;
	digits = 0;
	while (line[i] >= '0' && line[i] <= '9') {
		len = len * 10u + (unsigned)(line[i] - '0');
		digits = 1;
		i++;
	}
	if (!digits || line[i] != 0)
		return -1;
	*id_out = id;
	*len_out = len;
	return 0;
}

/* Read the next TEXT line for a command exchange,
 * queueing any EVT/DISPATCH units met on the way and
 * continuing past them. A BIN/AUD frame during a text
 * wait is a link-reset error (-1). A DISPATCH line is
 * followed by exactly <len> body bytes, consumed
 * here (over-cap bodies are a link-reset error).
 * Returns the line length, or -1. */
static int
next_resp_line(char *line, unsigned linecap)
{
	unsigned kind, ftype, ulen, id, blen;
	char tool[UPROTO_INB_TOOL_MAX + 1];
	unsigned char args[UPROTO_INB_DISPATCH_ARGS_MAX];

	for (;;) {
		int r = read_unit(UPROTO_CMD_TIMEOUT_MS, &kind, &ftype,
		    &ulen, line, linecap);
		if (r <= 0)
			return -1;
		if (kind == UNIT_FRAME) {
			if (ftype == LINK_FRAME_TYPE_EVT) {
				if (ulen > UPROTO_INB_EVT_MAX)
					return -1;
				/* Full queue drops + counts;
				 * the command read continues. */
				inb_push(UPROTO_INB_EVT, 0, 0,
				    frame_tmp + LINK_FRAME_HDR, ulen);
				continue;
			}
			return -1;
		}
		/* TEXT line: a DISPATCH line queues, anything
		 * else is the caller's line. The prefix is
		 * the full word + space, so a scan SSID
		 * merely starting with "DISP" is untouched
		 * (only a line shaped exactly like a dispatch
		 * header is swallowed, by grammar). */
		if (line[0] == 'D' && line[1] == 'I' &&
		    line[2] == 'S' && line[3] == 'P' &&
		    line[4] == 'A' && line[5] == 'T' &&
		    line[6] == 'C' && line[7] == 'H' &&
		    line[8] == ' ') {
			if (parse_dispatch_line(line, &id, tool,
				sizeof(tool), &blen) != 0)
				return -1;
			if (blen > UPROTO_INB_DISPATCH_ARGS_MAX)
				return -1;
			if (rd_exact(args, blen) != 0)
				return -1;
			inb_push(UPROTO_INB_DISPATCH, id, tool,
			    args, blen);
			continue;
		}
		return (int)ulen;
	}
}

/* Is this command's response count-prefixed multi-line? */
static int
is_multiline(const char *cmd)
{
	if (cmd[0] == 'W' && cmd[1] == 'I' && cmd[2] == 'F' &&
	    cmd[3] == 'I' && cmd[4] == '.') {
		const char *p = cmd + 5;
		if ((p[0] == 'S' && p[1] == 'C' && p[2] == 'A' &&
		     p[3] == 'N' && (p[4] == 0 || p[4] == ' ')) ||
		    (p[0] == 'L' && p[1] == 'I' && p[2] == 'S' &&
		     p[3] == 'T' && (p[4] == 0 || p[4] == ' ')))
			return 1;
	}
	return 0;
}

/* Parse "OK <n>" (n decimal) from a header line.
 * Returns n, or -1 if the line is not of that form. */
static int
parse_ok_count(const char *line)
{
	unsigned n = 0, i;

	if (line[0] != 'O' || line[1] != 'K' || line[2] != ' ')
		return -1;
	for (i = 3; line[i] >= '0' && line[i] <= '9'; i++)
		n = n * 10u + (unsigned)(line[i] - '0');
	if (i == 3 || line[i] != 0)
		return -1;
	return (int)n;
}

/* Read one text response for cmd into resp.
 * Returns 0 on OK (resp = payload after "OK ", lines
 * joined by '\n', NUL-terminated), -1 on ERR/timeout/
 * malformed response. Payload bytes past rsz-1 are
 * dropped but the result stays 0 (the caller's buffer
 * is the only cap -- see uartproto.h). */
static int
read_text_resp(const char *cmd, char *resp, unsigned rsz)
{
	char line[160];
	unsigned o = 0;
	int n, i;

	/* Unit-aware: EVT/DISPATCH met on the way are
	 * queued by next_resp_line, never lost. */
	if (next_resp_line(line, sizeof(line)) < 0)
		return -1;
	if (line[0] == 'E' && line[1] == 'R' && line[2] == 'R' &&
	    (line[3] == ' ' || line[3] == 0))
		return -1;
	if (!(line[0] == 'O' && line[1] == 'K' &&
	    (line[2] == ' ' || line[2] == 0)))
		return -1;
	/* Copy first-line payload (after "OK "). */
	i = (line[2] == ' ') ? 3 : 2;
	while (line[i] && o + 1 < rsz)
		resp[o++] = line[i++];
	if (is_multiline(cmd)) {
		n = parse_ok_count(line);
		if (n < 0)
			return -1;
		while (n-- > 0) {
			if (next_resp_line(line, sizeof(line)) < 0)
				return -1;
			if (o + 1 < rsz)
				resp[o++] = '\n';
			i = 0;
			while (line[i] && o + 1 < rsz)
				resp[o++] = line[i++];
		}
	}
	resp[o] = 0;
	return 0;
}

static void
cpycat(char *d, unsigned dsz, const char *a, const char *b, const char *c)
{
	unsigned i = 0, j;
	for (j = 0; a[j] && i + 1 < dsz; j++)
		d[i++] = a[j];
	if (b) {
		if (i + 1 < dsz)
			d[i++] = ' ';
		for (j = 0; b[j] && i + 1 < dsz; j++)
			d[i++] = b[j];
	}
	if (c) {
		if (i + 1 < dsz)
			d[i++] = ' ';
		for (j = 0; c[j] && i + 1 < dsz; j++)
			d[i++] = c[j];
	}
	d[i] = 0;
}

static unsigned
slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

/* Send one line (without '\n') and read its text
 * response. Returns 0/-1. */
static int
do_cmd(const char *txline, char *resp, unsigned rsz)
{
	char wire[TX_SZ + 1];
	unsigned n;

	if (be_ensure_open() != 0)
		return -1;
	cpycat(wire, sizeof(wire), txline, 0, 0);
	n = slen(wire);
	if (n + 1 > sizeof(wire))
		return -1;
	wire[n++] = '\n';
	if (cur_be->write((unsigned char *)wire, n) != 0)
		return -1;
	/* No staged-byte drop here (C6L-3): trailing
	 * bytes in staging may be queued inbound units;
	 * the unit reader consumes them in order. A
	 * failed exchange recovers by backend reopen,
	 * which clears the staging. */
	return read_text_resp(txline, resp, rsz);
}

int
uproto_cmd(const char *cmd, char *resp, unsigned rsz)
{
	char tx[TX_SZ];
	cpycat(tx, sizeof(tx), cmd, 0, 0);
	return do_cmd(tx, resp, rsz);
}

int
uproto_cmd1(const char *cmd, const char *arg, char *resp,
	    unsigned rsz)
{
	char tx[TX_SZ];
	cpycat(tx, sizeof(tx), cmd, arg, 0);
	return do_cmd(tx, resp, rsz);
}

int
uproto_cmd2(const char *cmd, const char *a1, const char *a2,
	    char *resp, unsigned rsz)
{
	char tx[TX_SZ];
	cpycat(tx, sizeof(tx), cmd, a1, a2);
	return do_cmd(tx, resp, rsz);
}

/* --- KV: length-prefixed values --- */

static void
put_dec(char *d, unsigned v)
{
	char tmp[12];
	unsigned i = 0, j;

	do {
		tmp[i++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && i < sizeof(tmp));
	for (j = 0; j < i; j++)
		d[j] = tmp[i - 1u - j];
	d[i] = 0;
}

int
uproto_kv_set(const char *key, const char *val, unsigned vlen)
{
	char wire[TX_SZ + 1 + UPROTO_KV_VAL_MAX];
	char line[TX_SZ], num[12], resp[16];
	unsigned n, i;

	if (!key || !key[0] || vlen > UPROTO_KV_VAL_MAX)
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	put_dec(num, vlen);
	cpycat(line, sizeof(line), "KV.SET", key, num);
	n = slen(line);
	if (n + 1 + vlen > sizeof(wire))
		return -1;
	for (i = 0; i < n; i++)
		wire[i] = line[i];
	wire[n++] = '\n';
	for (i = 0; i < vlen; i++)
		wire[n++] = val[i];
	if (cur_be->write((unsigned char *)wire, n) != 0)
		return -1;
	/* Unit-aware single-line OK/ERR response (queued
	 * inbound units on the way are kept). */
	if (next_resp_line(resp, sizeof(resp)) < 0)
		return -1;
	if (resp[0] == 'O' && resp[1] == 'K' &&
	    (resp[2] == ' ' || resp[2] == 0))
		return 0;
	return -1;
}

int
uproto_kv_get(const char *key, char *val, unsigned vsz,
	      unsigned *vlen_out)
{
	char line[160], hdr[160];
	unsigned len = 0, take;
	int n;

	if (!key || !key[0] || !val || vsz == 0)
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	cpycat(line, sizeof(line), "KV.GET", key, 0);
	{
		unsigned n = slen(line);
		if (n + 1 > sizeof(line))
			return -1;
		line[n++] = '\n';
		if (cur_be->write((unsigned char *)line, n) != 0)
			return -1;
	}
	if (next_resp_line(hdr, sizeof(hdr)) < 0)
		return -1;
	if (hdr[0] == 'E' && hdr[1] == 'R' && hdr[2] == 'R')
		return -1;
	n = parse_ok_count(hdr);
	if (n < 0)
		return -1;
	len = (unsigned)n;
	if (vlen_out)
		*vlen_out = len;
	/* Read exactly len value bytes; truncate into the
	 * caller's buffer but always consume the full body
	 * so the stream stays synchronised. */
	take = (len < vsz) ? len : vsz - 1u;
	if (rd_exact((unsigned char *)val, take) != 0)
		return -1;
	if (len > take) {
		unsigned char sink[32];
		unsigned left = len - take;
		while (left > 0) {
			unsigned chunk = (left < sizeof(sink)) ?
			    left : sizeof(sink);
			if (rd_exact(sink, chunk) != 0)
				return -1;
			left -= chunk;
		}
		val[take] = 0;
		return -2;
	}
	val[take] = 0;
	return 0;
}

int
uproto_kv_del(const char *key)
{
	char resp[16];
	return uproto_cmd1("KV.DEL", key, resp, sizeof(resp));
}

/* --- NET.HTTP: length-prefixed binary body (C6L-2) ---
 *
 * Wire (defined by the C6L-2 peer, scripts/mock-c6.py;
 * NOT part of frozen v1.1):  NET.HTTP GET <url>\n ->
 * "OK <len>\n" + exactly <len> body bytes. The body
 * framing reuses the KV.GET shape (length IS the
 * terminator), so the stream stays synchronised.
 * Returns 0 when the whole body fit, -2 when truncated
 * into body (still NUL-safe for text bodies, *len_out
 * = the peer's full length), -1 on error. */
int
uproto_nethttp_get(const char *url, unsigned char *body, unsigned cap,
		   unsigned *len_out)
{
	char line[160], hdr[160];
	unsigned len = 0u, take;
	int n;

	if (!url || !url[0] || !body || cap == 0)
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	cpycat(line, sizeof(line), "NET.HTTP GET", url, 0);
	{
		unsigned n = slen(line);
		if (n + 1 > sizeof(line))
			return -1;
		line[n++] = '\n';
		if (cur_be->write((unsigned char *)line, n) != 0)
			return -1;
	}
	if (next_resp_line(hdr, sizeof(hdr)) < 0)
		return -1;
	if (hdr[0] == 'E' && hdr[1] == 'R' && hdr[2] == 'R')
		return -1;
	n = parse_ok_count(hdr);
	if (n < 0)
		return -1;
	len = (unsigned)n;
	if (len_out)
		*len_out = len;
	take = (len < cap) ? len : cap;
	if (rd_exact(body, take) != 0)
		return -1;
	if (len > take) {
		unsigned char sink[32];
		unsigned left = len - take;
		while (left > 0) {
			unsigned chunk = (left < sizeof(sink)) ?
			    left : sizeof(sink);
			if (rd_exact(sink, chunk) != 0)
				return -1;
			left -= chunk;
		}
		return -2;
	}
	return 0;
}

/* --- Data plane v2: VOICE.START/STOP + AUD frames --- */

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

int
uproto_voice_start(unsigned rate, unsigned bits)
{
	char resp[64];
	char a1[16], a2[16];

	put_dec(a1, rate);
	put_dec(a2, bits);
	if (uproto_cmd2("VOICE.START", a1, a2, resp, sizeof(resp)) != 0)
		return -1;
	return 0;
}

int
uproto_voice_stop(void)
{
	char resp[64];
	unsigned n = 0u, i;

	if (uproto_cmd("VOICE.STOP", resp, sizeof(resp)) != 0)
		return -1;
	for (i = 0u; resp[i] >= '0' && resp[i] <= '9'; i++)
		n = n * 10u + (unsigned)(resp[i] - '0');
	return (int)n;
}

/* --- C6L-3: inbound polling, TOOL.DISPATCH, NET.WS --- */

/* Build + send one binary frame of the given type.
 * Shared by AUD (C6L-1) and WS data (C6L-3). Uses
 * the parser's frame_tmp (see read_unit): a send
 * never overlaps a live parsed frame, because every
 * consumer copies its frame out before the next
 * send happens. */
static int
send_bin_frame(unsigned type, const unsigned char *payload, unsigned len)
{
	unsigned crc, flen, i;

	if (len > LINK_MAX_PAYLOAD)
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	frame_tmp[0] = LINK_FRAME_MAGIC0;
	frame_tmp[1] = LINK_FRAME_MAGIC1;
	frame_tmp[2] = (unsigned char)type;
	frame_tmp[3] = (unsigned char)((len >> 8) & 0xffu);
	frame_tmp[4] = (unsigned char)(len & 0xffu);
	for (i = 0u; i < len; i++)
		frame_tmp[LINK_FRAME_HDR + i] = payload[i];
	crc = voice_crc16(frame_tmp, LINK_FRAME_HDR + len);
	frame_tmp[LINK_FRAME_HDR + len] =
	    (unsigned char)((crc >> 8) & 0xffu);
	frame_tmp[LINK_FRAME_HDR + len + 1u] =
	    (unsigned char)(crc & 0xffu);
	flen = LINK_FRAME_HDR + len + LINK_FRAME_CRC;
	return cur_be->send_frame(frame_tmp, flen);
}

int
uproto_aud_frame(const unsigned char *pcm, unsigned len)
{
	if (len == 0u || len > LINK_MAX_PAYLOAD)
		return -1;
	return send_bin_frame(LINK_FRAME_TYPE_AUD, pcm, len);
}

/* One poll attempt: a single unit with a short
 * per-byte budget. Returns 1 (inb filled), 0 (pure
 * silence: no byte at all), -1 (error). */
static int
poll_one(struct uproto_inb *inb)
{
	unsigned kind, ftype, ulen, id, blen, i;
	char line[160];
	char tool[UPROTO_INB_TOOL_MAX + 1];
	unsigned char args[UPROTO_INB_DISPATCH_ARGS_MAX];
	int r;

	r = read_unit(50u, &kind, &ftype, &ulen, line, sizeof(line));
	if (r <= 0)
		return r;
	if (kind == UNIT_FRAME) {
		if (ftype != LINK_FRAME_TYPE_EVT)
			return -1;
		if (ulen > UPROTO_INB_EVT_MAX)
			return -1;
		inb->kind = UPROTO_INB_EVT;
		inb->id = 0;
		inb->tool[0] = 0;
		inb->len = ulen;
		for (i = 0; i < ulen; i++)
			inb->payload[i] = frame_tmp[LINK_FRAME_HDR + i];
		return 1;
	}
	/* TEXT: only a DISPATCH line is a unit here. */
	if (parse_dispatch_line(line, &id, tool, sizeof(tool), &blen) != 0)
		return -1;
	if (blen > UPROTO_INB_DISPATCH_ARGS_MAX)
		return -1;
	if (rd_exact(args, blen) != 0)
		return -1;
	inb->kind = UPROTO_INB_DISPATCH;
	inb->id = id;
	for (i = 0; tool[i]; i++)
		inb->tool[i] = tool[i];
	inb->tool[i] = 0;
	for (i = 0; i < blen; i++)
		inb->payload[i] = args[i];
	inb->len = blen;
	return 1;
}

int
uproto_poll_inbound(struct uproto_inb *inb, unsigned budget_ms)
{
	unsigned t0;

	if (!inb)
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	t0 = sys_uptime();
	for (;;) {
		int r;

		if (inb_pop(inb))
			return 1;
		r = poll_one(inb);
		if (r > 0)
			return 1;
		if (r < 0)
			return -1;
		if (sys_uptime() - t0 >= budget_ms)
			return 0;
		sys_yield();
	}
}

/* Outbound dispatch bodies are capped so one
 * line+body write always fits the peers' input
 * accumulators (the in-process mock's is 256 B):
 * line (<= ~40) + '\n' + body (<= 192) < 240. */
#define	DISPATCH_BODY_MAX	192u

/* Send one line + '\n' + body bytes in a single
 * write (the peers parse line + length-prefixed
 * body exactly like KV.SET). Returns 0/-1. */
static int
write_line_body(const char *line, const unsigned char *body,
		unsigned blen)
{
	static unsigned char wire[64 + 1 + DISPATCH_BODY_MAX];
	unsigned n, i;

	if (blen > DISPATCH_BODY_MAX)
		return -1;
	n = slen(line);
	if (n + 1 + blen > sizeof(wire))
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	for (i = 0; i < n; i++)
		wire[i] = (unsigned char)line[i];
	wire[n++] = '\n';
	for (i = 0; i < blen; i++)
		wire[n++] = body[i];
	return cur_be->write(wire, n);
}

static void
set_err(char *err, unsigned esz, const char *code)
{
	unsigned i;

	if (!err || esz == 0)
		return;
	for (i = 0; code[i] && i + 1 < esz; i++)
		err[i] = code[i];
	err[i] = 0;
}

int
uproto_tool_dispatch(const char *tool, const char *args_json,
		     char *result, unsigned rsz,
		     char *err, unsigned esz)
{
	char line[96], num[12], hdr[160];
	unsigned alen, len, take, i;
	int n;

	if (!tool || !tool[0] || !args_json || !result || rsz == 0)
		return -1;
	alen = slen(args_json);
	if (alen > DISPATCH_BODY_MAX)
		return -1;
	put_dec(num, alen);
	cpycat(line, sizeof(line), "TOOL.DISPATCH", tool, num);
	if (write_line_body(line, (const unsigned char *)args_json,
		alen) != 0) {
		set_err(err, esz, "WIRE");
		return -1;
	}
	if (next_resp_line(hdr, sizeof(hdr)) < 0) {
		set_err(err, esz, "WIRE");
		return -1;
	}
	if (hdr[0] == 'E' && hdr[1] == 'R' && hdr[2] == 'R' &&
	    hdr[3] == ' ') {
		/* Peer ERR: copy the code (e.g. NOTOOL). */
		i = 4;
		n = 0;
		while (hdr[i] && hdr[i] != ' ' && (unsigned)n + 1 < esz) {
			if (err)
				err[n] = hdr[i];
			n++;
			i++;
		}
		if (err && esz)
			err[n] = 0;
		return -2;
	}
	n = parse_ok_count(hdr);
	if (n < 0) {
		set_err(err, esz, "WIRE");
		return -1;
	}
	len = (unsigned)n;
	take = (len < rsz) ? len : rsz - 1u;
	if (rd_exact((unsigned char *)result, take) != 0) {
		set_err(err, esz, "WIRE");
		return -1;
	}
	if (len > take) {
		/* Consume the rest so the stream stays in
		 * sync, but never deliver a partial JSON. */
		unsigned char sink[32];
		unsigned left = len - take;

		while (left > 0) {
			unsigned chunk = (left < sizeof(sink)) ?
			    left : sizeof(sink);
			if (rd_exact(sink, chunk) != 0) {
				set_err(err, esz, "WIRE");
				return -1;
			}
			left -= chunk;
		}
		result[take] = 0;
		set_err(err, esz, "TRUNC");
		return -1;
	}
	result[take] = 0;
	return 0;
}

int
uproto_dispatch_result(unsigned id, const char *result_json)
{
	char line[96], num[12], idbuf[12], resp[64];
	unsigned rlen;

	if (!result_json)
		return -1;
	rlen = slen(result_json);
	if (rlen > DISPATCH_BODY_MAX)
		return -1;
	put_dec(idbuf, id);
	put_dec(num, rlen);
	cpycat(line, sizeof(line), "DISPATCH.RESULT", idbuf, num);
	if (write_line_body(line, (const unsigned char *)result_json,
		rlen) != 0)
		return -1;
	if (next_resp_line(resp, sizeof(resp)) < 0)
		return -1;
	if (resp[0] == 'O' && resp[1] == 'K' &&
	    (resp[2] == ' ' || resp[2] == 0))
		return 0;
	return -1;
}

/* --- NET.WS (C6L-3, framing only) --- */

int
uproto_ws_open(const char *url, unsigned *sid_out)
{
	char resp[64];
	unsigned sid = 0, i;

	if (!url || !url[0])
		return -1;
	if (uproto_cmd1("NET.WS.OPEN", url, resp, sizeof(resp)) != 0)
		return -1;
	if (!resp[0])
		return -1;
	for (i = 0; resp[i] >= '0' && resp[i] <= '9'; i++)
		sid = sid * 10u + (unsigned)(resp[i] - '0');
	if (i == 0 || resp[i] != 0)
		return -1;
	if (sid_out)
		*sid_out = sid;
	return 0;
}

int
uproto_ws_send(const unsigned char *msg, unsigned len)
{
	if (!msg && len > 0)
		return -1;
	return send_bin_frame(LINK_FRAME_TYPE_BIN, msg, len);
}

int
uproto_ws_recv(unsigned char *buf, unsigned cap, unsigned *len_out)
{
	unsigned kind, ftype, ulen, id, blen, i;
	char line[160];
	char tool[UPROTO_INB_TOOL_MAX + 1];
	unsigned char args[UPROTO_INB_DISPATCH_ARGS_MAX];

	if (!buf || cap == 0)
		return -1;
	for (;;) {
		int r = read_unit(UPROTO_CMD_TIMEOUT_MS, &kind, &ftype,
		    &ulen, line, sizeof(line));
		if (r <= 0)
			return -1;
		if (kind == UNIT_FRAME) {
			if (ftype == LINK_FRAME_TYPE_BIN) {
				unsigned take = (ulen < cap) ? ulen : cap;

				for (i = 0; i < take; i++)
					buf[i] = frame_tmp[LINK_FRAME_HDR + i];
				if (len_out)
					*len_out = ulen;
				return (ulen > take) ? -2 : 0;
			}
			if (ftype == LINK_FRAME_TYPE_EVT) {
				if (ulen > UPROTO_INB_EVT_MAX)
					return -1;
				inb_push(UPROTO_INB_EVT, 0, 0,
				    frame_tmp + LINK_FRAME_HDR, ulen);
				continue;
			}
			return -1;
		}
		/* TEXT during a WS wait: a DISPATCH line
		 * queues (its body is consumed here);
		 * anything else is a link-reset error. */
		if (parse_dispatch_line(line, &id, tool, sizeof(tool),
			&blen) != 0)
			return -1;
		if (blen > UPROTO_INB_DISPATCH_ARGS_MAX)
			return -1;
		if (rd_exact(args, blen) != 0)
			return -1;
		inb_push(UPROTO_INB_DISPATCH, id, tool, args, blen);
	}
}

int
uproto_ws_close(unsigned sid)
{
	char num[12], resp[64];

	put_dec(num, sid);
	return uproto_cmd1("NET.WS.CLOSE", num, resp, sizeof(resp));
}
