/*
 * ulink5.c - C6L-5 spawn-only test client through
 * linkd (the stub, linkcli) -- NOT through the direct
 * layer. NOT in uprogs[]: test-only, "embed:ulink5".
 *
 * Legs (marker style ULINK5:, final ULINK5: PASS /
 * FAIL, shell reports exit <fails>):
 *   link up (STATUS) -> TIME.GET -> WIFI.SCAN
 *   (multi-line reply; contains RUMAH-HAFIDZ)
 *   -> KV set/get/del round-trip on its own key
 *   "link5a" -> guest TOOL.DISPATCH: get_info
 *   (mock-c6), gpio_read pin 3 (value 1),
 *   fail_test (test_failure), unknown tool
 *   (-2 NOTOOL) -> NET.HTTP GET (1024 B body,
 *   deterministic pattern) -> voice stream: 32
 *   x 1024 B frames at ~31 ms through the stub,
 *   zero frame failures, STOP == 32
 *   -> transport-death handshake: prints
 *   "ready kill" (the script kills the peer),
 *   settles 1.5 s, then TIME.GET must fail AND
 *   link state must read DOWN; prints "ready
 *   restart" (the script restarts the peer),
 *   then TIME.GET must succeed again and state
 *   must read UP.
 *
 * Demultiplexing is proven jointly with ulink5d
 * (the marker-gated daemon, running concurrently):
 * each client round-trips its OWN key value, and
 * the script asserts ulink5d's ok markers appear
 * during this program's run with no FAILED.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"

static int	fails;
static char	buf[2048];
static char	val[128];
static char	err[64];

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

static void
check(const char *leg, int ok)
{
	u_put("ULINK5: ");
	u_put(leg);
	if (ok) {
		u_put(" ok\n");
	} else {
		u_put(" FAILED\n");
		fails++;
	}
}

static int
contains(const char *s, const char *sub)
{
	unsigned i = 0u, j;

	if (sub[0] == 0)
		return 1;
	while (s[i]) {
		j = 0u;
		while (sub[j] && s[i + j] == sub[j])
			j++;
		if (sub[j] == 0)
			return 1;
		i++;
	}
	return 0;
}

static int
streq(const char *a, const char *b)
{
	unsigned i = 0u;

	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

static void
sleep_ms(unsigned ms)
{
	unsigned t0 = sys_uptime();

	while (sys_uptime() - t0 < ms)
		sys_yield();
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;

	u_put("ULINK5: start\n");

	check("link up", uproto_link_state() == 0);
	check("time", uproto_cmd("TIME.GET", buf, sizeof(buf)) == 0);
	rc = uproto_cmd("WIFI.SCAN", buf, sizeof(buf));
	check("scan", rc == 0 && contains(buf, "RUMAH-HAFIDZ"));

	rc = uproto_kv_set("link5a", "A1", 2u);
	check("kv set", rc == 0);
	rc = uproto_kv_get("link5a", val, sizeof(val), 0);
	check("kv get", rc == 0 && streq(val, "A1"));
	rc = uproto_kv_del("link5a");
	check("kv del", rc == 0);
	rc = uproto_kv_get("link5a", val, sizeof(val), 0);
	check("kv gone", rc < 0);

	rc = uproto_tool_dispatch("get_info", "{}", buf, sizeof(buf),
				  err, sizeof(err));
	check("tool info", rc == 0 && contains(buf, "mock-c6"));
	rc = uproto_tool_dispatch("gpio_read", "{\"pin\":3}", buf,
				  sizeof(buf), err, sizeof(err));
	check("tool gpio", rc == 0 && contains(buf, "\"value\":1"));
	rc = uproto_tool_dispatch("fail_test", "{}", buf, sizeof(buf),
				  err, sizeof(err));
	check("tool fail", rc == 0 && contains(buf, "test_failure"));
	rc = uproto_tool_dispatch("nosuchtool", "{}", buf, sizeof(buf),
				  err, sizeof(err));
	check("tool notool", rc == -2 && streq(err, "NOTOOL"));

	{
		unsigned hlen = 0u;

		rc = uproto_nethttp_get("http://example.test/",
					(unsigned char *)buf,
					sizeof(buf), &hlen);
		check("nethttp",
		      rc == 0 && hlen == 1024u &&
		      (unsigned char)buf[0] == 13u &&
		      (unsigned char)buf[1023] == 6u);
	}

	/* Voice stream through the stub at
	 * production pace (~31 ms / 1024 B frame). */
	{
		unsigned char frame[1024];
		int fstart, fstop, fbad = 0;
		unsigned t0, f;

		fstart = uproto_voice_start(16000, 16);
		check("voice start", fstart == 0);
		for (f = 0u; f < 32u; f++) {
			unsigned j;

			for (j = 0u; j < sizeof(frame); j++)
				frame[j] = (unsigned char)((f * 3u + j) &
							   0xFFu);
			if (uproto_aud_frame(frame,
					     sizeof(frame)) != 0)
				fbad++;
			t0 = sys_uptime();
			while (sys_uptime() - t0 < 31u)
				sys_yield();
		}
		fstop = uproto_voice_stop();
		u_put("ULINK5: voice fbad=");
		putu((unsigned)fbad);
		u_put(" fstop=");
		puti(fstop);
		u_put("\n");
		check("voice frames", fbad == 0);
		check("voice stop", fstop == 32);
	}

	/* Transport death, scripted by markers. The
	 * script kills the peer on "ready kill"; the
	 * kill may land just after an attempt, so
	 * retry until the failure is observed (up
	 * to ~4 s) rather than racing one attempt. */
	u_put("ULINK5: ready kill\n");
	{
		unsigned att;
		int dead = 0;

		for (att = 0u; att < 8u; att++) {
			rc = uproto_cmd("TIME.GET", buf, sizeof(buf));
			if (rc != 0 && uproto_link_state() != 0) {
				dead = 1;
				break;
			}
			sleep_ms(500u);
		}
		check("dead cmd fails", dead);
	}
	check("dead state down", uproto_link_state() != 0);
	u_put("ULINK5: dead ok\n");
	u_put("ULINK5: ready restart\n");
	{
		unsigned t0 = sys_uptime();
		int ok = 0;

		while (sys_uptime() - t0 < 30000u) {
			if (uproto_cmd("TIME.GET", buf,
				       sizeof(buf)) == 0) {
				ok = 1;
				break;
			}
			sys_yield();
		}
		check("recovered cmd", ok);
	}
	check("recovered state up", uproto_link_state() == 0);
	u_put("ULINK5: recovered ok\n");

	u_put("ULINK5: ");
	if (fails == 0) {
		u_put("PASS\n");
	} else {
		u_put("FAIL ");
		putu((unsigned)fails);
		u_put("\n");
	}
	sys_exit(fails);
}
