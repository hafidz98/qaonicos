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
 * Protocol discipline (see WIRE-FREEZE.md): strict
 * request/response alternation. A peer that sends
 * unsolicited bytes, or a malformed length that stalls
 * the stream, is a link-reset event: the caller must
 * close + reopen the backend. No inbound (peer-
 * initiated) path exists yet; EVT/TOOL.DISPATCH arrive
 * in C6L-3.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"

#define TX_SZ	160

/* --- Backend selection --- */

static struct uproto_backend *cur_be = &uproto_backend_mock;
static int be_opened = 0;

void
uproto_backend_select(struct uproto_backend *be)
{
	if (be_opened && cur_be->close)
		cur_be->close();
	cur_be = be ? be : &uproto_backend_mock;
	be_opened = 0;
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

#define	RD_CHUNK	32u

static unsigned char rd_buf[RD_CHUNK];
static unsigned rd_len = 0, rd_pos = 0;

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

/* Read one line (without '\n') into buf (NUL-terminated).
 * Returns line length, or -1 on timeout/link death/
 * overflow (a line longer than cap-1). Bytes past the
 * newline in the final chunk are dropped: the protocol
 * is strict request/response, no pipelining. */
static int
rd_line(char *buf, unsigned cap)
{
	unsigned o = 0;
	int c;

	for (;;) {
		c = rd_byte(UPROTO_CMD_TIMEOUT_MS);
		if (c < 0)
			return -1;
		if (c == '\n') {
			buf[o] = 0;
			return (int)o;
		}
		if (c == '\r')
			continue;
		if (o + 1 >= cap)
			return -1;
		buf[o++] = (char)c;
	}
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
		c = rd_byte(UPROTO_CMD_TIMEOUT_MS);
		if (c < 0)
			return -1;
		buf[o++] = (unsigned char)c;
	}
	return 0;
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

	if (rd_line(line, sizeof(line)) < 0)
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
			if (rd_line(line, sizeof(line)) < 0)
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
	/* Fresh exchange: drop any stale staged bytes. */
	rd_len = rd_pos = 0;
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
	rd_len = rd_pos = 0;
	/* Single-line OK/ERR response. */
	if (rd_line(resp, sizeof(resp)) < 0)
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
	rd_len = rd_pos = 0;
	if (rd_line(hdr, sizeof(hdr)) < 0)
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

int
uproto_aud_frame(const unsigned char *pcm, unsigned len)
{
	static unsigned char frame[LINK_FRAME_HDR + LINK_MAX_PAYLOAD +
	    LINK_FRAME_CRC];
	unsigned crc, flen, i;

	if (len == 0u || len > LINK_MAX_PAYLOAD)
		return -1;
	if (be_ensure_open() != 0)
		return -1;
	frame[0] = LINK_FRAME_MAGIC0;
	frame[1] = LINK_FRAME_MAGIC1;
	frame[2] = LINK_FRAME_TYPE_AUD;
	frame[3] = (unsigned char)((len >> 8) & 0xffu);
	frame[4] = (unsigned char)(len & 0xffu);
	for (i = 0u; i < len; i++)
		frame[LINK_FRAME_HDR + i] = pcm[i];
	crc = voice_crc16(frame, LINK_FRAME_HDR + len);
	frame[LINK_FRAME_HDR + len] = (unsigned char)((crc >> 8) & 0xffu);
	frame[LINK_FRAME_HDR + len + 1u] = (unsigned char)(crc & 0xffu);
	flen = LINK_FRAME_HDR + len + LINK_FRAME_CRC;
	return cur_be->send_frame(frame, flen);
}
