/*
 * user/ulinkbr.c -- C6L-2: bridge backend + host peer test.
 *
 * Spawn-only test (run from the shell as
 * `run embed:ulinkbr`; NOT a boot one-shot, on
 * purpose): a boot one-shot runs with the scheduler
 * inactive, where SYS_YIELD is a nop and net_pump
 * never runs, so the bridge could never complete a
 * TCP handshake there. Under the active scheduler
 * every yield pumps the network, which is also what
 * keeps the FND-3 watchdog fed during waits.
 *
 * The program selects the bridge backend (guest TCP
 * to the host Python peer, scripts/mock-c6.py) and
 * runs the frozen v1.1 command set against it, plus
 * AUD frames, a NET.HTTP hash check, and a peer-death
 * + reconnect sequence. Prints one "ULINKBR: <check>
 * ok" marker per check and a final "ULINKBR: PASS"
 * (or FAIL); the shell reports "exit <fails>".
 *
 * No peer listening: prints "ULINKBR: SKIP (no peer)"
 * and exits 0, so ordinary shell runs stay clean --
 * the bridge is only exercised when a peer is actually
 * present. All mutations use test-only keys/networks
 * and are undone at start AND end (the peer's NVS is
 * host-process memory, untouched by the guest's own
 * NVS; the mock backend is never selected here).
 *
 * Peer-death choreography (the harness drives it):
 * after the AUD + NET.HTTP phases the program prints
 * "ULINKBR: PHASE-A-DONE" and sleeps ~6 s. The harness
 * kills and restarts the peer inside that window. The
 * next command must then FAIL (dead connection), and
 * a reselect + retry must succeed against the new
 * peer process (whose NVS is fresh -- post-reconnect
 * checks use fresh keys only).
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"

static int fails = 0;

static void
ok(const char *marker)
{
	u_put("ULINKBR: ");
	u_put(marker);
	u_put(" ok\n");
}

static void
bad(const char *marker)
{
	fails++;
	u_put("ULINKBR: ");
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
starts(const char *s, const char *p)
{
	unsigned i;
	for (i = 0; p[i]; i++)
		if (s[i] != p[i])
			return 0;
	return 1;
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

static void
cleanup(void)
{
	char resp[64];
	int i;
	static const char *keys[] = {
		"ulinkbr_a", "ulinkbr_sp", "ulinkbr_e", "ulinkbr_t",
		"ulinkbr_f0", "ulinkbr_f1", "ulinkbr_f2", "ulinkbr_f3",
		"ulinkbr_f4", "ulinkbr_f5", "ulinkbr_f6", "ulinkbr_f7",
		"ulinkbr_wire", "ulinkbr_re",
	};

	uproto_cmd("WIFI.DISC", resp, sizeof(resp));
	uproto_cmd1("WIFI.FORGET", "RUMAH-HAFIDZ", resp, sizeof(resp));
	for (i = 0; i < 14; i++)
		uproto_kv_del(keys[i]);
	uproto_cmd("BLE.OFF", resp, sizeof(resp));
	uproto_cmd1("BLE.NAME", "QABOT-01", resp, sizeof(resp));
}

__attribute__((section(".text.start")))
void
_start(void)
{
	char resp[512];
	char val[96];
	unsigned vlen = 0;
	int i, r;

	u_put("ulinkbr: bridge backend + host peer (C6L-2)\n");
	uproto_backend_select(&uproto_backend_bridge);
	if (uproto_backend_current()->open() != 0) {
		u_put("ULINKBR: SKIP (no peer)\n");
		sys_exit(0);
	}
	cleanup();

	/* --- WIFI --- */
	r = uproto_cmd("WIFI.SCAN", resp, sizeof(resp));
	check(r == 0 && resp[0] == '3' &&
	    contains(resp, "RUMAH-HAFIDZ") &&
	    contains(resp, "KANTOR-5G") &&
	    contains(resp, "WARUNGKOPI"), "wifi scan");
	r = uproto_cmd1("WIFI.CONNECT", "RUMAH-HAFIDZ", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "NEEDPASS") == 0,
	    "wifi connect needpass");
	r = uproto_cmd1("WIFI.PASS", "short", resp, sizeof(resp));
	check(r != 0, "wifi err auth");
	r = uproto_cmd1("WIFI.PASS", "password123", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "CONNECTED") == 0,
	    "wifi pass connected");
	r = uproto_cmd1("WIFI.CONNECT", "WARUNGKOPI", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "CONNECTED") == 0,
	    "wifi open connected");
	r = uproto_cmd("WIFI.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "CONNECTED WARUNGKOPI") == 0,
	    "wifi open connected");
	r = uproto_cmd1("WIFI.CONNECT", "TIDAK-ADA", resp, sizeof(resp));
	check(r != 0, "wifi err notfound");
	r = uproto_cmd("WIFI.DISC", resp, sizeof(resp));
	r = uproto_cmd1("WIFI.PASS", "password123", resp, sizeof(resp));
	check(r != 0, "wifi err noconn");
	r = uproto_cmd("WIFI.LIST", resp, sizeof(resp));
	check(r == 0 && resp[0] == '1' &&
	    contains(resp, "RUMAH-HAFIDZ"), "wifi list forget");
	r = uproto_cmd1("WIFI.FORGET", "RUMAH-HAFIDZ", resp, sizeof(resp));
	r = uproto_cmd("WIFI.LIST", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "0") == 0, "wifi list forget");
	r = uproto_cmd("WIFI.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "DISCONNECTED") == 0,
	    "wifi status disc");

	/* --- BLE: frozen v1.1 STATUS is the paired peer --- */
	r = uproto_cmd("BLE.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "OFF NONE") == 0,
	    "ble status spec");
	r = uproto_cmd("BLE.ON", resp, sizeof(resp));
	r = uproto_cmd("BLE.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "ON NONE") == 0, "ble status spec");
	r = uproto_cmd1("BLE.NAME", "ULINKBR-NAME", resp, sizeof(resp));
	r = uproto_cmd("BLE.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "ON NONE") == 0,
	    "ble status spec");
	r = uproto_cmd("BLE.OFF", resp, sizeof(resp));
	r = uproto_cmd("BLE.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "OFF NONE") == 0,
	    "ble on off name");
	r = uproto_cmd("BLE.ON", resp, sizeof(resp));
	check(r == 0, "ble on off name");
	r = uproto_cmd("BLE.OFF", resp, sizeof(resp));
	check(r == 0, "ble on off name");

	/* --- KV: length-prefixed values --- */
	r = uproto_kv_set("ulinkbr_a", "hello", 5);
	check(r == 0, "kv len-prefixed");
	r = uproto_kv_get("ulinkbr_a", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 5 && scmp(val, "hello") == 0,
	    "kv len-prefixed");
	r = uproto_kv_set("ulinkbr_sp", "hello world with spaces", 23);
	check(r == 0, "kv len-prefixed");
	r = uproto_kv_get("ulinkbr_sp", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 23 &&
	    scmp(val, "hello world with spaces") == 0,
	    "kv len-prefixed");
	r = uproto_kv_set("ulinkbr_e", "", 0);
	check(r == 0, "kv len-prefixed");
	r = uproto_kv_get("ulinkbr_e", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 0 && val[0] == 0, "kv len-prefixed");
	r = uproto_kv_get("ulinkbr_missing", val, sizeof(val), &vlen);
	check(r != 0, "kv errors");
	r = uproto_kv_del("ulinkbr_a");
	r = uproto_kv_get("ulinkbr_a", val, sizeof(val), &vlen);
	check(r != 0, "kv errors");
	{
		char big[70];
		for (i = 0; i < 69; i++)
			big[i] = 'x';
		big[69] = 0;
		r = uproto_kv_set("ulinkbr_big", big, 69);
		check(r != 0, "kv errors");
		r = uproto_kv_get("ulinkbr_big", val, sizeof(val), &vlen);
		check(r != 0, "kv errors");
	}
	{
		int created = 0, got_full = 0;
		char kname[16];
		for (i = 0; i < 8; i++) {
			kname[0] = 'u'; kname[1] = 'l'; kname[2] = 'i';
			kname[3] = 'n'; kname[4] = 'k'; kname[5] = 'b';
			kname[6] = 'r'; kname[7] = '_'; kname[8] = 'f';
			kname[9] = (char)('0' + i); kname[10] = 0;
			if (uproto_kv_set(kname, "v", 1) == 0)
				created++;
			else {
				got_full = 1;
				break;
			}
		}
		check(got_full, "kv errors");
		for (i = 0; i < created; i++) {
			kname[0] = 'u'; kname[1] = 'l'; kname[2] = 'i';
			kname[3] = 'n'; kname[4] = 'k'; kname[5] = 'b';
			kname[6] = 'r'; kname[7] = '_'; kname[8] = 'f';
			kname[9] = (char)('0' + i); kname[10] = 0;
			uproto_kv_del(kname);
		}
	}
	r = uproto_kv_set("ulinkbr_t", "0123456789abcdef", 16);
	check(r == 0, "kv trunc");
	r = uproto_kv_get("ulinkbr_t", val, 8, &vlen);
	check(r == -2 && vlen == 16 && scmp(val, "0123456") == 0,
	    "kv trunc");
	r = uproto_cmd("KV.SET ulinkbr_legacy hello", resp, sizeof(resp));
	check(r != 0, "kv legacy rejected");
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "kv legacy rejected");

	/* --- Wire bytes: the harness asserts the peer
	 * transcript holds exactly this request. --- */
	r = uproto_kv_set("ulinkbr_wire", "xyz", 3);
	check(r == 0, "kv len-prefixed");

	/* --- AUD over the bridge --- */
	{
		unsigned char pcm[512];
		unsigned char legacy[5 + 4 + 2];
		unsigned char bad[5 + 4 + 2];
		unsigned crc;
		int sent = 0, got;

		for (i = 0; i < 512; i++)
			pcm[i] = (unsigned char)(i & 0xff);
		r = uproto_voice_start(16000u, 16u);
		check(r == 0, "aud frames");
		for (i = 0; i < 8; i++) {
			if (uproto_aud_frame(pcm, 512) == 0)
				sent++;
		}
		/* Rejected frames get no response over the
		 * bridge (frames are fire-and-forget); the
		 * proof is the STOP count, not the send
		 * return value. */
		legacy[0] = 'A'; legacy[1] = 'U'; legacy[2] = 'D';
		legacy[3] = 0; legacy[4] = 4;
		for (i = 0; i < 4; i++)
			legacy[5 + i] = (unsigned char)i;
		crc = voice_crc16(legacy, 9);
		legacy[9] = (unsigned char)((crc >> 8) & 0xff);
		legacy[10] = (unsigned char)(crc & 0xff);
		uproto_backend_current()->send_frame(legacy, 11);
		bad[0] = 'Q'; bad[1] = 'A'; bad[2] = 0x01;
		bad[3] = 0; bad[4] = 4;
		for (i = 0; i < 4; i++)
			bad[5 + i] = (unsigned char)i;
		bad[9] = 0; bad[10] = 0;	/* bad CRC */
		uproto_backend_current()->send_frame(bad, 11);
		got = uproto_voice_stop();
		check(sent == 8 && got == 8, "aud frames");
	}

	/* --- NET.HTTP: hash the deterministic body --- */
	{
		static unsigned char body[2048];
		unsigned blen = 0;
		unsigned crc_body, crc_pat, j;

		r = uproto_nethttp_get("http://x/test", body, sizeof(body),
		    &blen);
		crc_body = voice_crc16(body, blen);
		crc_pat = 0xffffu;
		{
			/* CRC over the locally generated pattern
			 * (same bytes the peer serves). */
			unsigned crc = 0xffffu;
			unsigned k, b;
			for (k = 0; k < 1024; k++) {
				b = (k * 7u + 13u) & 0xffu;
				crc ^= b << 8;
				for (j = 0; j < 8; j++)
					crc = (crc & 0x8000u) ?
					    (crc << 1) ^ 0x1021u :
					    crc << 1;
			}
			crc_pat = crc & 0xffffu;
		}
		check(r == 0 && blen == 1024 && crc_body == crc_pat,
		    "nethttp hash");
	}

	/* --- Peer death + reconnect (harness-driven) --- */
	u_put("ULINKBR: PHASE-A-DONE\n");
	sleep_ms(6000u);
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r != 0, "peer death detected");
	{
		int recovered = 0;
		for (i = 0; i < 5 && !recovered; i++) {
			uproto_backend_select(&uproto_backend_bridge);
			r = uproto_cmd("TIME.GET", resp, sizeof(resp));
			if (r == 0)
				recovered = 1;
			else
				sleep_ms(1000u);
		}
		check(recovered, "peer reconnect");
		/* Fresh keys only: the restarted peer's NVS
		 * is empty (its own reboot semantics). */
		r = uproto_kv_set("ulinkbr_re", "alive", 5);
		check(r == 0, "peer reconnect");
		r = uproto_kv_get("ulinkbr_re", val, sizeof(val), &vlen);
		check(r == 0 && vlen == 5 && scmp(val, "alive") == 0,
		    "peer reconnect");
	}

	/* --- TIME / FIDO / unknown (post-reconnect) --- */
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0 && starts(resp, "20"), "time fido unknown");
	r = uproto_cmd("FIDO.LIST", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "0") == 0, "time fido unknown");
	r = uproto_cmd("NOPE.CMD", resp, sizeof(resp));
	check(r != 0, "time fido unknown");

	cleanup();
	uproto_backend_select(0);	/* close the bridge TCP */

	if (fails == 0)
		u_put("ULINKBR: PASS\n");
	else
		u_put("ULINKBR: FAIL\n");
	sys_exit(fails);
}
