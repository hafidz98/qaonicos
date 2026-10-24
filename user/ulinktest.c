/*
 * user/ulinktest.c -- C6L-1: link layer + wire freeze test.
 *
 * One-shot (runs at boot from uprogs[], after uvoice):
 * every frozen v1.1 command against the mock backend,
 * including error paths, plus the v2 AUD slice over the
 * frozen QA frame format, plus a backend-swap proof.
 * Prints one "ULINK: <check> ok" marker per check and a
 * final "ULINK: PASS" (or "ULINK: FAIL").
 *
 * Self-cleaning: all mutations use test-only keys /
 * networks and are undone at start AND end, so the
 * NVS the UI screens read is left exactly as found
 * (test SSID forgotten, test keys deleted, BLE name
 * restored to QABOT-01, BLE off, Wi-Fi disconnected).
 * A crashed run is repaired by the next run's start
 * cleanup, which deletes the same fixed key list.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"
#include "uartproto/link.h"

static int fails = 0;

static void
ok(const char *marker)
{
	u_put("ULINK: ");
	u_put(marker);
	u_put(" ok\n");
}

static void
bad(const char *marker)
{
	fails++;
	u_put("ULINK: ");
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

static unsigned
slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
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

/* --- Cleanup: fixed test-key list, idempotent --- */

static void
cleanup(void)
{
	char resp[64];
	int i;
	static const char *keys[] = {
		"ulink_a", "ulink_sp", "ulink_e", "ulink_t",
		"ulink_f0", "ulink_f1", "ulink_f2", "ulink_f3",
		"ulink_f4", "ulink_f5", "ulink_f6", "ulink_f7",
		"ulink_stub",
	};

	uproto_cmd("WIFI.DISC", resp, sizeof(resp));
	uproto_cmd1("WIFI.FORGET", "RUMAH-HAFIDZ", resp, sizeof(resp));
	for (i = 0; i < 13; i++)
		uproto_kv_del(keys[i]);
	uproto_cmd("BLE.OFF", resp, sizeof(resp));
	uproto_cmd1("BLE.NAME", "QABOT-01", resp, sizeof(resp));
}

/* --- Stub backend: the seam proof ---
 *
 * Records the last wire bytes and answers from canned
 * responses. The link layer must produce identical
 * results through it as through the mock for the
 * probed commands, and the recorded KV.SET wire must
 * be exactly the frozen length-prefixed form. */
static unsigned char stub_wire[256];
static unsigned stub_wire_len = 0;
static unsigned char stub_resp[128];
static unsigned stub_resp_len = 0, stub_resp_pos = 0;

static int
stub_open(void)
{
	stub_wire_len = 0;
	stub_resp_len = stub_resp_pos = 0;
	return 0;
}

static int
stub_close(void)
{
	stub_wire_len = 0;
	stub_resp_len = stub_resp_pos = 0;
	return 0;
}

static void
stub_answer(const char *s)
{
	unsigned i;
	stub_resp_len = stub_resp_pos = 0;
	for (i = 0; s[i] && stub_resp_len < sizeof(stub_resp); i++)
		stub_resp[stub_resp_len++] = (unsigned char)s[i];
}

static int
stub_write(const unsigned char *buf, unsigned len)
{
	unsigned i;
	stub_wire_len = 0;
	for (i = 0; i < len && stub_wire_len < sizeof(stub_wire); i++)
		stub_wire[stub_wire_len++] = buf[i];
	/* Canned response keyed on the request's first line. */
	if (len >= 9 && buf[0] == 'W' && buf[1] == 'I' &&
	    buf[2] == 'F' && buf[3] == 'I' && buf[4] == '.' &&
	    buf[5] == 'S' && buf[6] == 'C' && buf[7] == 'A' &&
	    buf[8] == 'N') {
		stub_answer("OK 1\nSTUB-SSID,-50,WPA2\n");
	} else if (len >= 11 && buf[0] == 'W' && buf[5] == 'S' &&
	    buf[6] == 'T' && buf[7] == 'A') {
		/* WIFI.STATUS */
		stub_answer("OK DISCONNECTED\n");
	} else if (len >= 6 && buf[0] == 'K' && buf[1] == 'V' &&
	    buf[2] == '.' && buf[3] == 'G') {
		/* KV.GET */
		stub_answer("OK 3\nxyz");
	} else if (len >= 6 && buf[0] == 'K' && buf[1] == 'V') {
		/* KV.SET */
		stub_answer("OK\n");
	} else if (len >= 8 && buf[0] == 'T' && buf[1] == 'I') {
		/* TIME.GET */
		stub_answer("OK 2026-10-24T00:00:00+07:00\n");
	} else {
		stub_answer("OK\n");
	}
	return 0;
}

static int
stub_read(unsigned char *buf, unsigned len)
{
	unsigned avail, n, i;

	if (stub_resp_pos >= stub_resp_len)
		return 0;
	avail = stub_resp_len - stub_resp_pos;
	n = avail;
	if (n > len)
		n = len;
	if (n > 16u)
		n = 16u;
	for (i = 0; i < n; i++)
		buf[i] = stub_resp[stub_resp_pos++];
	return (int)n;
}

static int
stub_frame(const unsigned char *frame, unsigned flen)
{
	(void)frame;
	(void)flen;
	return 0;
}

static struct uproto_backend stub_backend = {
	"stub",
	stub_open,
	stub_write,
	stub_read,
	stub_frame,
	stub_close,
};

static int
wire_equals(const char *s)
{
	unsigned i, n = slen(s);
	if (stub_wire_len != n)
		return 0;
	for (i = 0; i < n; i++)
		if (stub_wire[i] != (unsigned char)s[i])
			return 0;
	return 1;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	char resp[512];
	char val[96];
	unsigned vlen = 0;
	int i, r;

	u_put("ulinktest: link layer + wire freeze (C6L-1)\n");
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
	r = uproto_cmd1("BLE.NAME", "ULINK-NAME", resp, sizeof(resp));
	r = uproto_cmd("BLE.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "ON NONE") == 0,
	    "ble status spec");
	r = uproto_cmd("BLE.OFF", resp, sizeof(resp));
	r = uproto_cmd("BLE.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "OFF NONE") == 0,
	    "ble on off name");
	/* BLE.ON/OFF/NAME all returned OK above (r==0 on the
	 * last STATUS is not the proof); re-prove the trio
	 * explicitly: */
	r = uproto_cmd("BLE.ON", resp, sizeof(resp));
	check(r == 0, "ble on off name");
	r = uproto_cmd("BLE.OFF", resp, sizeof(resp));
	check(r == 0, "ble on off name");

	/* --- KV: length-prefixed values --- */
	r = uproto_kv_set("ulink_a", "hello", 5);
	check(r == 0, "kv len-prefixed");
	r = uproto_kv_get("ulink_a", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 5 && scmp(val, "hello") == 0,
	    "kv len-prefixed");
	r = uproto_kv_set("ulink_sp", "hello world with spaces", 23);
	check(r == 0, "kv len-prefixed");
	r = uproto_kv_get("ulink_sp", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 23 &&
	    scmp(val, "hello world with spaces") == 0,
	    "kv len-prefixed");
	r = uproto_kv_set("ulink_e", "", 0);
	check(r == 0, "kv len-prefixed");
	r = uproto_kv_get("ulink_e", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 0 && val[0] == 0, "kv len-prefixed");
	r = uproto_kv_get("ulink_missing", val, sizeof(val), &vlen);
	check(r != 0, "kv errors");
	r = uproto_kv_del("ulink_a");
	r = uproto_kv_get("ulink_a", val, sizeof(val), &vlen);
	check(r != 0, "kv errors");
	/* Over-cap value: client guard, nothing stored. */
	{
		char big[70];
		for (i = 0; i < 69; i++)
			big[i] = 'x';
		big[69] = 0;
		r = uproto_kv_set("ulink_big", big, 69);
		check(r != 0, "kv errors");
		r = uproto_kv_get("ulink_big", val, sizeof(val), &vlen);
		check(r != 0, "kv errors");
	}
	/* FULL: fill remaining slots, then one more fails. */
	{
		int created = 0, got_full = 0;
		char kname[16];
		for (i = 0; i < 8; i++) {
			kname[0] = 'u'; kname[1] = 'l'; kname[2] = 'i';
			kname[3] = 'n'; kname[4] = 'k'; kname[5] = '_';
			kname[6] = 'f'; kname[7] = (char)('0' + i);
			kname[8] = 0;
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
			kname[3] = 'n'; kname[4] = 'k'; kname[5] = '_';
			kname[6] = 'f'; kname[7] = (char)('0' + i);
			kname[8] = 0;
			uproto_kv_del(kname);
		}
	}
	/* Truncation: small buffer keeps old behaviour. */
	r = uproto_kv_set("ulink_t", "0123456789abcdef", 16);
	check(r == 0, "kv trunc");
	r = uproto_kv_get("ulink_t", val, 8, &vlen);
	check(r == -2 && vlen == 16 && scmp(val, "0123456") == 0,
	    "kv trunc");
	/* Legacy inline shape: rejected at once, no desync. */
	r = uproto_cmd("KV.SET ulink_legacy hello", resp, sizeof(resp));
	check(r != 0, "kv legacy rejected");
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "kv legacy rejected");

	/* --- AUD over the frozen QA frame --- */
	{
		unsigned char pcm[512];
		int sent = 0, got;
		for (i = 0; i < 512; i++)
			pcm[i] = (unsigned char)(i & 0xff);
		r = uproto_voice_start(16000u, 16u);
		check(r == 0, "aud qa frame");
		for (i = 0; i < 8; i++) {
			if (uproto_aud_frame(pcm, 512) == 0)
				sent++;
		}
	/* Legacy "AUD" magic + a bad-CRC QA frame, sent
	 * while voice is ACTIVE so a rejection proves
	 * the frame itself was refused, not the state. */
	{
		unsigned char legacy[5 + 4 + 2];
		unsigned char bad[5 + 4 + 2];
		unsigned crc;
		legacy[0] = 'A'; legacy[1] = 'U'; legacy[2] = 'D';
		legacy[3] = 0; legacy[4] = 4;
		for (i = 0; i < 4; i++)
			legacy[5 + i] = (unsigned char)i;
		crc = voice_crc16(legacy, 9);
		legacy[9] = (unsigned char)((crc >> 8) & 0xff);
		legacy[10] = (unsigned char)(crc & 0xff);
		r = uproto_backend_current()->send_frame(legacy, 11);
		check(r != 0, "legacy aud rejected");
		bad[0] = 'Q'; bad[1] = 'A'; bad[2] = 0x01;
		bad[3] = 0; bad[4] = 4;
		for (i = 0; i < 4; i++)
			bad[5 + i] = (unsigned char)i;
		bad[9] = 0; bad[10] = 0;	/* bad CRC */
		r = uproto_backend_current()->send_frame(bad, 11);
		check(r != 0, "legacy aud rejected");
	}
		got = uproto_voice_stop();
		check(sent == 8 && got == 8, "aud qa frame");
	}

	/* --- Backend swap: same clients, stub backend --- */
	uproto_backend_select(&stub_backend);
	r = uproto_cmd("WIFI.SCAN", resp, sizeof(resp));
	check(r == 0 && resp[0] == '1' &&
	    contains(resp, "STUB-SSID"), "stub backend swapped");
	r = uproto_cmd("WIFI.STATUS", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "DISCONNECTED") == 0,
	    "stub backend swapped");
	r = uproto_kv_set("ulink_stub", "xyz", 3);
	check(r == 0 &&
	    wire_equals("KV.SET ulink_stub 3\nxyz"),
	    "stub backend swapped");
	r = uproto_kv_get("ulink_stub", val, sizeof(val), &vlen);
	check(r == 0 && vlen == 3 && scmp(val, "xyz") == 0,
	    "stub backend swapped");
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0 && starts(resp, "2026-10-24"),
	    "stub backend swapped");
	uproto_backend_select(0);	/* back to mock */
	r = uproto_cmd("WIFI.SCAN", resp, sizeof(resp));
	check(r == 0 && resp[0] == '3' &&
	    contains(resp, "RUMAH-HAFIDZ"), "stub backend swapped");

	/* --- TIME / FIDO / unknown --- */
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0 && (starts(resp, "20") || starts(resp, "MOCK-")),
	    "time fido unknown");
	r = uproto_cmd("FIDO.LIST", resp, sizeof(resp));
	check(r == 0 && scmp(resp, "0") == 0, "time fido unknown");
	r = uproto_cmd("NOPE.CMD", resp, sizeof(resp));
	check(r != 0, "time fido unknown");

	/* --- Malformed KV: numeric length with no body ---
	 * The mock waits for 5 body bytes; the link layer
	 * must time out (2 s) instead of hanging, and a
	 * backend reopen must resynchronise the stream. */
	r = uproto_cmd("KV.SET ulink_bad 5", resp, sizeof(resp));
	check(r != 0, "malformed kv resync");
	uproto_backend_select(0);	/* close + reopen mock */
	r = uproto_cmd("TIME.GET", resp, sizeof(resp));
	check(r == 0, "malformed kv resync");

	cleanup();

	if (fails == 0)
		u_put("ULINK: PASS\n");
	else
		u_put("ULINK: FAIL\n");
	sys_exit(fails);
}
