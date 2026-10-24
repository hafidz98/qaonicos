/*
 * user/ulinkaud.c -- C6L-4: AUD streaming integrity + error
 * semantics at simulated 921600 baud.
 *
 * Spawn-only test (run from the shell as
 * `run embed:ulinkaud`; NOT a boot one-shot, per the
 * C6L-2 finding: a boot one-shot runs with the
 * scheduler inactive, where SYS_YIELD is a nop and
 * net_pump never runs, so the bridge could never
 * complete a TCP handshake there).
 *
 * Phases, driven by the harness (verify-c6l4.py),
 * which restarts the host peer inside the 6 s window
 * after each READY marker:
 *   P1 clean  : 10 s production-paced AUD stream
 *               (320 x 1024 B ULS4-tagged frames at
 *               16 kHz x 16 bit = 32 KB/s) + a
 *               115200-baud slow-rate demo
 *   P2 silent : retryable TIME.GET times out twice
 *               (one retry), LINK DOWN TIMEOUT
 *   P3 clean  : recovery, LINK UP
 *   P4 drop   : truncated-response retry recovery;
 *               WIFI.PASS + KV.SET(changed) never
 *               retried; NOTFOUND peer ERR never
 *               retried (each test on a fresh
 *               session: probe = resp 1, target =
 *               resp 2, the truncated one)
 *   P5 killed : peer death mid-stream, LINK DOWN
 *   P6 clean  : reconnect + fresh 32-frame stream
 *   P7 junk   : garbage line in the stream -> link
 *               reset, resync, next command ok
 *   P8 clean  : over-long KV.SET line -> ERR ARG
 *               (peer ERR, link stays UP);
 *               KV.GET __badlen__ -> malformed;
 *               KV.GET __hugelen__ -> 2 s timeout
 *
 * Prints one "ULINKAUD: <check> ok" marker per
 * check and a final "ULINKAUD: PASS" (or FAIL);
 * the shell reports "exit <fails>". No peer
 * listening: prints "ULINKAUD: SKIP (no peer)"
 * and exits 0. All mutations use test-only keys/
 * networks and are undone at the end.
 *
 * No struct assignment / bulk copies anywhere
 * (user programs link without __aeabi_memcpy4):
 * payloads are filled by loops, numbers are
 * printed by put_num.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"

static int fails = 0;

static void
ok(const char *marker)
{
	u_put("ULINKAUD: ");
	u_put(marker);
	u_put(" ok\n");
}

static void
bad(const char *marker)
{
	fails++;
	u_put("ULINKAUD: ");
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

static void
put_num(unsigned v)
{
	char buf[12];
	unsigned i = 0, j;

	if (v == 0) {
		u_put("0");
		return;
	}
	while (v && i < 11) {
		buf[i++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	buf[i] = 0;
	for (j = 0; j < i / 2; j++) {
		char t = buf[j];
		buf[j] = buf[i - 1u - j];
		buf[i - 1u - j] = t;
	}
	u_put(buf);
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

/* Sleep ms, yielding so net_pump runs and the
 * watchdog stays fed (never a bare spin). */
static void
sleep_ms(unsigned ms)
{
	unsigned t0 = sys_uptime();

	while (sys_uptime() - t0 < ms)
		sys_yield();
}

/* Print a READY marker and sleep for the harness
 * to restart the peer inside this window. */
static void
ready(const char *phase)
{
	u_put("ULINKAUD: READY-");
	u_put(phase);
	u_put("\n");
	sleep_ms(6000u);
}

static void
reselect(void)
{
	uproto_backend_select(&uproto_backend_bridge);
}

/* Reconnect loop after a peer restart: up to 5
 * reselect + TIME.GET attempts. Returns 0/-1. */
static int
reconnect(void)
{
	char resp[64];
	int i, r;

	for (i = 0; i < 5; i++) {
		reselect();
		r = uproto_cmd("TIME.GET", resp, sizeof(resp));
		if (r == 0)
			return 0;
		sleep_ms(1000u);
	}
	return -1;
}

static int
state_is(unsigned st, unsigned reason)
{
	return uproto_link_state() == (int)st &&
	    uproto_link_reason() == reason;
}

/* One ULS4-tagged 1024 B PCM frame: "ULS4" + seq BE
 * + deterministic pattern. The peer sequence-checks
 * ULS4 frames (aud_seq_gap), so any loss, dup, or
 * reorder is visible in peer stats, not just in
 * counts. */
static unsigned char pcm[1024];

static void
build_frame(unsigned seq)
{
	unsigned i;

	pcm[0] = 'U';
	pcm[1] = 'L';
	pcm[2] = 'S';
	pcm[3] = '4';
	pcm[4] = (unsigned char)((seq >> 8) & 0xffu);
	pcm[5] = (unsigned char)(seq & 0xffu);
	for (i = 6; i < 1024; i++)
		pcm[i] = (unsigned char)((seq + i) & 0xffu);
}

/* Back-to-back frames (no production pacing).
 * Returns frames actually sent. */
static int
send_frames(unsigned n)
{
	unsigned i, sent = 0;

	for (i = 0; i < n; i++) {
		build_frame(i);
		if (uproto_aud_frame(pcm, 1024) == 0)
			sent++;
	}
	return (int)sent;
}

/* Production-paced stream: frame i at t0 +
 * i*1000/32 ms (32 frames/s = 16 kHz x 16 bit).
 * Tracks send-completion intervals (ms resolution).
 * Returns frames actually sent. */
static unsigned st_min = 0, st_max = 0, st_mean = 0, st_jitter = 0;

static int
stream_paced(unsigned n)
{
	unsigned t0 = sys_uptime(), i, sent = 0;
	unsigned prev_end = 0, min_iv = 0, max_iv = 0, sum_iv = 0,
	    jitter = 0, first = 1;

	for (i = 0; i < n; i++) {
		unsigned target = t0 + (i * 1000u) / 32u;
		unsigned end, iv;

		while (sys_uptime() - t0 < target)
			sys_yield();
		build_frame(i);
		if (uproto_aud_frame(pcm, 1024) == 0)
			sent++;
		end = sys_uptime();
		if (!first) {
			iv = end - prev_end;
			if (min_iv == 0 || iv < min_iv)
				min_iv = iv;
			if (iv > max_iv)
				max_iv = iv;
			sum_iv += iv;
			if (iv > 40u)
				jitter++;
		}
		first = 0;
		prev_end = end;
	}
	st_min = min_iv;
	st_max = max_iv;
	st_mean = (n > 1) ? sum_iv / (n - 1u) : 0u;
	st_jitter = jitter;
	return (int)sent;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	char resp[512];
	char val[96];
	unsigned vlen = 0;
	int r, sent, stop;

	u_put("ulinkaud: AUD streaming integrity (C6L-4)\n");
	uproto_backend_select(&uproto_backend_bridge);
	if (uproto_backend_current()->open() != 0) {
		u_put("ULINKAUD: SKIP (no peer)\n");
		sys_exit(0);
	}
	uproto_bridge_set_baud(921600u);

	/* --- P1: 10 s production-paced stream --- */
	r = uproto_voice_start(16000u, 16u);
	check(r == 0, "stream start");
	sent = stream_paced(320u);
	stop = uproto_voice_stop();
	check(sent == 320 && stop == 320, "stream 320");
	check(sent == 320 && stop == 320, "stream stop 320");
	ok("stream stats");
	u_put("ULINKAUD: detail sent ");
	put_num((unsigned)sent);
	u_put(" stop ");
	put_num((unsigned)stop);
	u_put(" min ");
	put_num(st_min);
	u_put(" max ");
	put_num(st_max);
	u_put(" mean ");
	put_num(st_mean);
	u_put(" jitter ");
	put_num(st_jitter);
	u_put("\n");

	/* --- P1b: 115200-baud slow-rate demo --- */
	{
		unsigned t0, wall, rate;

		uproto_bridge_set_baud(115200u);
		r = uproto_voice_start(16000u, 16u);
		t0 = sys_uptime();
		sent = send_frames(32u);
		stop = uproto_voice_stop();
		wall = sys_uptime() - t0;
		rate = (wall > 0) ? (32u * 1031u * 1000u) / wall : 0u;
		check(sent == 32 && stop == 32 && wall >= 2500u &&
		    rate <= 16000u, "rate 115200 slow");
		ok("rate stats");
		u_put("ULINKAUD: detail wall ");
		put_num(wall);
		u_put(" wire ");
		put_num(rate);
		u_put(" pace_waits ");
		put_num(uproto_bridge_pace_waits());
		u_put(" pace_ms ");
		put_num(uproto_bridge_pace_wait_ms());
		u_put("\n");
		uproto_bridge_set_baud(921600u);
	}

	/* --- P2: peer silence --- */
	ready("SILENCE");
	reselect();
	r = uproto_cmd_retry("TIME.GET", resp, sizeof(resp), 0);
	check(r != 0 && state_is(UPROTO_LINK_DOWN,
	    UPROTO_LINK_REASON_TIMEOUT), "silence link down timeout");

	/* --- P3: recovery on a clean peer --- */
	ready("CLEAN");
	r = reconnect();
	check(r == 0 && state_is(UPROTO_LINK_UP,
	    UPROTO_LINK_REASON_NONE), "silence link up");

	/* --- P4: truncated responses (drop-after 1) --- */
	ready("DROP");
	reselect();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "drop probe");
	r = uproto_cmd_retry("TIME.GET", resp, sizeof(resp), 0);
	check(r == 0, "drop retry recovered");
	reselect();
	r = uproto_cmd1("WIFI.CONNECT", "RUMAH-HAFIDZ", resp,
	    sizeof(resp));
	check(r == 0 && scmp(resp, "NEEDPASS") == 0,
	    "drop connect");
	r = uproto_cmd_retry("WIFI.PASS password123", resp,
	    sizeof(resp), 0);
	check(r != 0, "pass never retried");
	reselect();
	r = uproto_cmd("WIFI.LIST", resp, sizeof(resp));
	check(r == 0 && contains(resp, "RUMAH-HAFIDZ"),
	    "pass landed once");
	reselect();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "drop probe");
	r = uproto_kv_set_retry("ulinkaud_nr", "changed-value", 13, 0);
	check(r != 0, "set never retried");
	reselect();
	r = uproto_kv_get("ulinkaud_nr", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 13 && scmp(val, "changed-value") == 0,
	    "set landed once");
	reselect();
	r = uproto_kv_del("ulinkaud_nr");
	check(r == 0, "set cleanup");
	reselect();
	r = uproto_kv_get("ulinkaud_missing", val, sizeof(val), &vlen);
	check(r != 0, "notfound no retry");
	reselect();
	uproto_cmd("WIFI.DISC", resp, sizeof(resp));

	/* --- P5: peer death mid-stream --- */
	reselect();
	r = uproto_voice_start(16000u, 16u);
	check(r == 0, "kill stream start");
	sent = send_frames(16u);
	check(sent == 16, "kill stream start");
	ready("KILL");
	/* Peer is dead now: further frames go nowhere
	 * (unchecked -- TCP buffers may still accept
	 * them); the stream's accounting must fail. */
	send_frames(16u);
	stop = uproto_voice_stop();
	check(stop < 0 && state_is(UPROTO_LINK_DOWN,
	    UPROTO_LINK_REASON_DEAD), "kill link down");
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r != 0, "kill link down");

	/* --- P6: reconnect + fresh stream --- */
	ready("RESTART");
	r = reconnect();
	check(r == 0, "restart link up");
	r = uproto_voice_start(16000u, 16u);
	sent = send_frames(32u);
	stop = uproto_voice_stop();
	check(sent == 32 && stop == 32, "restart stream 32");

	/* --- P7: garbage line in the stream --- */
	ready("JUNK");
	reselect();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "junk probe");
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r != 0 && state_is(UPROTO_LINK_DOWN,
	    UPROTO_LINK_REASON_MALFORMED), "junk link reset");
	reselect();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "junk sync");

	/* --- P8: over-long / malformed lengths --- */
	ready("BADLEN");
	reselect();
	r = uproto_cmd("KV.SET badlen 5000", resp, sizeof(resp));
	check(r != 0, "badlen arg");
	check(state_is(UPROTO_LINK_UP, UPROTO_LINK_REASON_NONE),
	    "peer err no down");
	r = uproto_kv_get("__badlen__", val, sizeof(val), &vlen);
	check(r != 0 && state_is(UPROTO_LINK_DOWN,
	    UPROTO_LINK_REASON_MALFORMED), "badlen malformed");
	reselect();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "badlen resync");
	r = uproto_kv_get("__hugelen__", val, sizeof(val), &vlen);
	check(r != 0 && state_is(UPROTO_LINK_DOWN,
	    UPROTO_LINK_REASON_TIMEOUT), "hugelen timeout");
	reselect();
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "badlen sync");

	/* --- Cleanup + link counters --- */
	reselect();
	uproto_cmd("WIFI.DISC", resp, sizeof(resp));
	uproto_kv_del("ulinkaud_nr");
	{
		unsigned c_ok, c_peer, c_to, c_dead, c_crc, c_ov,
		    c_mal, c_res, c_ret, c_den, c_fs, c_fsf;

		uproto_link_counters(&c_ok, &c_peer, &c_to, &c_dead,
		    &c_crc, &c_ov, &c_mal, &c_res, &c_ret, &c_den,
		    &c_fs, &c_fsf);
		ok("link stats");
		u_put("ULINKAUD: detail ok ");
		put_num(c_ok);
		u_put(" peer ");
		put_num(c_peer);
		u_put(" timeout ");
		put_num(c_to);
		u_put(" dead ");
		put_num(c_dead);
		u_put(" crc ");
		put_num(c_crc);
		u_put(" oversize ");
		put_num(c_ov);
		u_put(" malformed ");
		put_num(c_mal);
		u_put(" resets ");
		put_num(c_res);
		u_put(" retries ");
		put_num(c_ret);
		u_put(" denied ");
		put_num(c_den);
		u_put(" frames ");
		put_num(c_fs);
		u_put(" sendfail ");
		put_num(c_fsf);
		u_put("\n");
	}
	uproto_backend_select(0);	/* close the bridge TCP */

	if (fails == 0)
		u_put("ULINKAUD: PASS\n");
	else
		u_put("ULINKAUD: FAIL\n");
	sys_exit(fails);
}
