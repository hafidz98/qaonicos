/*
 * user/ulink3.c -- C6L-3: v2 completion test (TOOL.DISPATCH,
 * EVT, NET.WS framing; wire v1.2).
 *
 * Spawn-only test (run from the shell as
 * `run embed:ulink3`; NOT a boot one-shot, on purpose:
 * the bridge leg needs the scheduler active so yields
 * pump net_pump, per the C6L-2 finding). Two legs with
 * the same checks: the in-process mock backend, then
 * the bridge backend against the host Python peer
 * (scripts/mock-c6.py). The bridge leg prints
 * "ULINK3: SKIP (no peer)" and exits 0 when no peer
 * listens, so ordinary shell runs stay clean.
 *
 * Per leg, in order:
 *   - TEST.SCRIPT c6l3 arms scripted peer-initiated
 *     units; the guest polls them in order and
 *     answers each dispatch: get_info (ALLOW, real
 *     uptime output), gpio_read (ALLOW, pin-table
 *     output), gpio_write (CONFIRM -> needs_confirm),
 *     self_destruct (BLOCK -> refused); then an EVT
 *     WIFI.DROPPED is observed. The mock leg also
 *     reads back the peer's dispatch receipts.
 *   - TEST.BADFRAME sends one corrupt-CRC EVT frame:
 *     the poll must report a link error (not the
 *     event, not a hang); after a backend reopen a
 *     plain command must succeed again.
 *   - NET.WS: send before OPEN is dropped (mock:
 *     synchronous -1; bridge: no echo, since frames
 *     get no response); OPEN returns sid 1; a second
 *     OPEN is BUSY; a BIN message echoes verbatim; a
 *     bad-CRC BIN send is dropped the same way;
 *     CLOSE works; a second CLOSE is NOCONN.
 *   - Guest-originated TOOL.DISPATCH: get_info and
 *     gpio_read return structured results asserted
 *     field-by-field; fail_test returns a JSON tool
 *     failure; an unknown tool is ERR NOTOOL.
 *   - TIME.GET still works (stream in sync after all
 *     the asynchronous traffic).
 *
 * Policy mirror (qabotd user/qabot/tools.c +
 * policy.c): get_info/gpio_read are SAFE (ALLOW);
 * gpio_write/file_write are CONFIRM; anything else
 * (including self_destruct, which is not in the
 * registry at all) is BLOCK. The test consumer is a
 * stand-in for C6L-5's linkd + qabotd: linkd will
 * forward peer dispatches to qabotd's `tools` IPC
 * name, where the real gate decides. See DEVLOG-C6L3.
 *
 * Prints one "ULINK3: <leg> <check> ok" marker per
 * check and a final "ULINK3: PASS" (or FAIL); the
 * shell reports "exit <fails>".
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"

static int fails = 0;
static const char *leg = "mock";

static void
ok(const char *marker)
{
	u_put("ULINK3: ");
	u_put(leg);
	u_put(" ");
	u_put(marker);
	u_put(" ok\n");
}

static void
bad(const char *marker)
{
	fails++;
	u_put("ULINK3: ");
	u_put(leg);
	u_put(" ");
	u_put(marker);
	u_put(" FAILED\n");
}

static void
check(int cond, const char *marker)
{
	if (cond)
		ok(marker);
	else
		bad(marker);
}

static int
scmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int
contains(const char *hay, const char *needle)
{
	unsigned i, j;
	if (!*needle)
		return 1;
	for (i = 0; hay[i]; i++) {
		for (j = 0; needle[j] && hay[i + j] == needle[j]; j++)
			;
		if (!needle[j])
			return 1;
	}
	return 0;
}

static int
memeq(const unsigned char *a, const char *b, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++)
		if (a[i] != (unsigned char)b[i])
			return 0;
	return 1;
}

static unsigned
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
	return i;
}

/* Find "pin":<digits> in a JSON args buffer (length-
 * bounded, not NUL-terminated). Returns pin, or -1. */
static int
json_pin(const unsigned char *body, unsigned bodylen)
{
	unsigned i, pin = 0;
	int digits = 0;

	for (i = 0; i + 5 < bodylen; i++) {
		if (body[i] == '"' && body[i + 1u] == 'p' &&
		    body[i + 2u] == 'i' && body[i + 3u] == 'n' &&
		    body[i + 4u] == '"') {
			i += 5u;
			while (i < bodylen && (body[i] == ' ' ||
			    body[i] == ':'))
				i++;
			while (i < bodylen && body[i] >= '0' &&
			    body[i] <= '9') {
				pin = pin * 10u +
				    (unsigned)(body[i] - '0');
				digits = 1;
				i++;
			}
			return digits ? (int)pin : -1;
		}
	}
	return -1;
}

/* Build the get_info result: real guest data (uptime). */
static void
build_get_info(char *out, unsigned cap)
{
	static const char p1[] = "{\"ok\":true,\"result\":"
	    "{\"uptime_ms\":";
	static const char p2[] = "}}";
	unsigned o = 0, i, n;

	for (i = 0; p1[i] && o + 1 < cap; i++)
		out[o++] = p1[i];
	n = put_dec(out + o, sys_uptime());
	o += n;
	for (i = 0; p2[i] && o + 1 < cap; i++)
		out[o++] = p2[i];
	out[o] = 0;
}

/* Build the gpio_read result for pin (pin table:
 * value = pin & 1, same table as the peers). */
static void
build_gpio_read(char *out, unsigned cap, int pin)
{
	static const char p1[] = "{\"ok\":true,\"result\":"
	    "{\"pin\":";
	static const char p2[] = ",\"value\":";
	static const char p3[] = "}}";
	unsigned o = 0, i, n;

	for (i = 0; p1[i] && o + 1 < cap; i++)
		out[o++] = p1[i];
	n = put_dec(out + o, (unsigned)pin);
	o += n;
	for (i = 0; p2[i] && o + 1 < cap; i++)
		out[o++] = p2[i];
	n = put_dec(out + o, (unsigned)(pin & 1));
	o += n;
	for (i = 0; p3[i] && o + 1 < cap; i++)
		out[o++] = p3[i];
	out[o] = 0;
}

/* --- Script phase: poll + answer the scripted units ---
 * Expected order: DISPATCH get_info, DISPATCH
 * gpio_read, DISPATCH gpio_write, DISPATCH
 * self_destruct, EVT WIFI.DROPPED. check_receipts
 * (mock leg only) reads the peer's captured receipts
 * back through the test-only mock API. */
static void
run_script_phase(int check_receipts)
{
	struct uproto_inb inb;
	char resp[64], res[128];
	int r, step = 0, ans[4] = { 0, 0, 0, 0 };
	unsigned t0;

	uproto_inb_flush();
	r = uproto_cmd1("TEST.SCRIPT", "c6l3", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "ARMED") == 0, "script armed");
	t0 = sys_uptime();
	while (step < 5 && sys_uptime() - t0 < 10000u) {
		r = uproto_poll_inbound(&inb, 500u);
		if (r == 0) {
			sys_yield();
			continue;
		}
		if (r < 0)
			break;
		if (step == 0 && inb.kind == UPROTO_INB_DISPATCH &&
		    scmp(inb.tool, "get_info") == 0) {
			build_get_info(res, sizeof(res));
			if (uproto_dispatch_result(inb.id, res) == 0)
				ans[0] = 1;
			step++;
		} else if (step == 1 &&
		    inb.kind == UPROTO_INB_DISPATCH &&
		    scmp(inb.tool, "gpio_read") == 0) {
			int pin = json_pin(inb.payload, inb.len);

			if (pin >= 0) {
				build_gpio_read(res, sizeof(res), pin);
				if (uproto_dispatch_result(inb.id,
					res) == 0)
					ans[1] = 1;
			}
			step++;
		} else if (step == 2 &&
		    inb.kind == UPROTO_INB_DISPATCH &&
		    scmp(inb.tool, "gpio_write") == 0) {
			if (uproto_dispatch_result(inb.id,
				"{\"ok\":false,\"error\":"
				"\"needs_confirm\"}") == 0)
				ans[2] = 1;
			step++;
		} else if (step == 3 &&
		    inb.kind == UPROTO_INB_DISPATCH &&
		    scmp(inb.tool, "self_destruct") == 0) {
			if (uproto_dispatch_result(inb.id,
				"{\"ok\":false,\"error\":\"refused\"}")
				== 0)
				ans[3] = 1;
			step++;
		} else if (step == 4 &&
		    inb.kind == UPROTO_INB_EVT && inb.len == 12 &&
		    memeq(inb.payload, "WIFI.DROPPED", 12)) {
			step++;
		} else {
			break;
		}
	}
	check(step >= 1 && ans[0], "dispatch get_info");
	check(step >= 2 && ans[1], "dispatch gpio_read");
	check(step >= 3 && ans[2], "dispatch gpio_write confirm");
	check(step >= 4 && ans[3], "dispatch self_destruct refused");
	check(step >= 5, "evt dropped");
	if (check_receipts) {
		char rb[160];
		unsigned id0, id1, id2, id3;
		int rc;

		rc = mock_comcu_script_result_count();
		check(rc == 4, "dispatch receipts");
		if (rc == 4) {
			mock_comcu_script_result(0, &id0, rb,
			    sizeof(rb));
			check(id0 == 1 && contains(rb, "uptime_ms"),
			    "dispatch receipts");
			mock_comcu_script_result(1, &id1, rb,
			    sizeof(rb));
			check(id1 == 2 && contains(rb, "\"pin\":3") &&
			    contains(rb, "\"value\":1"),
			    "dispatch receipts");
			mock_comcu_script_result(2, &id2, rb,
			    sizeof(rb));
			check(id2 == 3 &&
			    contains(rb, "needs_confirm"),
			    "dispatch receipts");
			mock_comcu_script_result(3, &id3, rb,
			    sizeof(rb));
			check(id3 == 4 && contains(rb, "refused"),
			    "dispatch receipts");
		}
	}
}

/* --- Bad-frame probe: corrupt EVT -> link error,
 * reopen, plain command works again. --- */
static void
run_badframe_phase(struct uproto_backend *be)
{
	struct uproto_inb inb;
	char resp[64];
	int r;

	r = uproto_cmd("TEST.BADFRAME", resp, sizeof(resp));
	check(r == 0, "bad evt reset");
	if (r != 0)
		return;
	r = uproto_poll_inbound(&inb, 3000u);
	check(r < 0, "bad evt reset");
	uproto_backend_select(be);
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "sync after reset");
}

/* --- NET.WS phase ---
 * sync_reject (mock leg only): the in-process mock
 * rejects a BIN send synchronously (-1) when no
 * session is open or the CRC is bad. Over the bridge
 * the peer's rejection is unknowable at send time
 * (frames get no response): the send succeeds and
 * the rejection surfaces as absence of echo (plus
 * the peer's ws_drop stats, asserted by the verify
 * script). The test asserts each backend's own
 * semantics; pretending the bridge rejects
 * synchronously would be a false assertion. */
static void
run_ws_phase(int sync_reject)
{
	unsigned char msg[16], echo[32];
	unsigned sid = 0, elen = 0;
	unsigned char frame[64];
	unsigned crc, flen, i;
	int r;

	/* No session yet: a BIN send is dropped. */
	if (sync_reject) {
		r = uproto_ws_send((unsigned char *)"WS-HELLO", 8);
		check(r != 0, "ws preopen rejected");
	} else {
		r = uproto_ws_send((unsigned char *)"WS-HELLO", 8);
		check(r == 0, "ws preopen sent");
		if (r == 0) {
			/* No echo arrives: the peer dropped it. */
			r = uproto_ws_recv(echo, sizeof(echo), &elen);
			check(r != 0, "ws preopen dropped");
		}
	}
	r = uproto_ws_open("ws://test/echo", &sid);
	check(r == 0 && sid == 1, "ws open");
	r = uproto_ws_open("ws://test/echo", &sid);
	check(r != 0, "ws busy");
	for (i = 0; i < 8; i++)
		msg[i] = (unsigned char)"WS-HELLO"[i];
	r = uproto_ws_send(msg, 8);
	check(r == 0, "ws echo");
	if (r == 0) {
		r = uproto_ws_recv(echo, sizeof(echo), &elen);
		check(r == 0 && elen == 8 &&
		    memeq(echo, "WS-HELLO", 8), "ws echo");
	}
	/* Bad-CRC BIN: build a valid frame, corrupt it,
	 * send via the backend directly. */
	frame[0] = LINK_FRAME_MAGIC0;
	frame[1] = LINK_FRAME_MAGIC1;
	frame[2] = LINK_FRAME_TYPE_BIN;
	frame[3] = 0;
	frame[4] = 4;
	for (i = 0; i < 4; i++)
		frame[LINK_FRAME_HDR + i] = (unsigned char)i;
	crc = voice_crc16(frame, LINK_FRAME_HDR + 4);
	frame[LINK_FRAME_HDR + 4] = (unsigned char)((crc >> 8) & 0xffu);
	frame[LINK_FRAME_HDR + 5] = (unsigned char)(crc & 0xffu);
	frame[LINK_FRAME_HDR + 5] ^= 0xffu;
	flen = LINK_FRAME_HDR + 4 + LINK_FRAME_CRC;
	r = uproto_backend_current()->send_frame(frame, flen);
	if (sync_reject)
		check(r != 0, "ws bad crc rejected");
	else
		/* Transport accepted it; the peer drops
		 * it (ws_drop stats, asserted by the
		 * verify script). */
		check(r == 0, "ws bad crc dropped");
	r = uproto_ws_close(1);
	check(r == 0, "ws close");
	r = uproto_ws_close(1);
	check(r != 0, "ws noconn");
}

/* --- Guest-originated TOOL.DISPATCH phase --- */
static void
run_tool_phase(void)
{
	char result[256], err[32];
	int r;

	r = uproto_tool_dispatch("get_info", "{}", result, sizeof(result),
	    err, sizeof(err));
	check(r == 0 && contains(result, "\"ok\":true") &&
	    contains(result, "\"device\""), "tool get_info");
	r = uproto_tool_dispatch("gpio_read", "{\"pin\":3}", result,
	    sizeof(result), err, sizeof(err));
	check(r == 0 && contains(result, "\"pin\":3") &&
	    contains(result, "\"value\":1"), "tool gpio_read");
	r = uproto_tool_dispatch("gpio_read", "{\"pin\":4}", result,
	    sizeof(result), err, sizeof(err));
	check(r == 0 && contains(result, "\"pin\":4") &&
	    contains(result, "\"value\":0"), "tool gpio_read");
	r = uproto_tool_dispatch("fail_test", "{}", result, sizeof(result),
	    err, sizeof(err));
	check(r == 0 && contains(result, "\"ok\":false") &&
	    contains(result, "test_failure"), "tool fail");
	r = uproto_tool_dispatch("nope_tool", "{}", result, sizeof(result),
	    err, sizeof(err));
	check(r == -2 && scmp(err, "NOTOOL") == 0, "tool notool");
}

static void
run_leg(struct uproto_backend *be, int check_receipts,
	int sync_reject)
{
	char resp[64];
	int r;

	run_script_phase(check_receipts);
	run_badframe_phase(be);
	run_ws_phase(sync_reject);
	run_tool_phase();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "sync time");
}

__attribute__((section(".text.start")))
void
_start(void)
{
	u_put("ulink3: v2 completion (TOOL.DISPATCH/EVT/NET.WS, C6L-3)\n");

	/* --- Leg 1: in-process mock (always present) --- */
	leg = "mock";
	uproto_backend_select(0);
	run_leg(0, 1, 1);

	/* --- Leg 2: bridge + host peer --- */
	leg = "bridge";
	uproto_backend_select(&uproto_backend_bridge);
	if (uproto_backend_current()->open() != 0) {
		u_put("ULINK3: SKIP (no peer)\n");
		sys_exit(0);
	}
	run_leg(&uproto_backend_bridge, 0, 0);
	uproto_backend_select(0);	/* close the bridge TCP */

	check(uproto_inb_dropped() == 0, "no drops");
	leg = "final";
	if (fails == 0)
		u_put("ULINK3: PASS\n");
	else
		u_put("ULINK3: FAIL\n");
	sys_exit(fails);
}
