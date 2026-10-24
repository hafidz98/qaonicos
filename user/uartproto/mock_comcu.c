/*
 * mock_comcu.c - mock co-MCU backend for the link layer (C6L-1).
 *
 * HONEST: this is not hardware. The co-MCU is simulated
 * in-process so UI and link tests run end-to-end in QEMU.
 * Unlike the pre-C6L-1 mock, this backend is a WIRE-LEVEL
 * peer: the link layer hands it the exact bytes a real
 * peer would receive, and it parses them with the same
 * rules (line grammar, length-prefixed KV bodies, QA
 * frames). A bug in the link layer's wire encoding is
 * therefore visible here, not hidden by a function call.
 *
 * Backing store = struct nvs_state (user/cfg/cfg.h)
 * persisted write-through to two SD sectors: KV, saved
 * Wi-Fi networks, BLE state survive reboot. Wi-Fi
 * connection state itself is runtime-only.
 *
 * Commands (frozen v1.1, see
 * docs/rencana-komunikasi-c6/WIRE-FREEZE.md):
 *   WIFI.SCAN -> OK <n>\n<ssid>,<rssi>,<sec>\n...
 *   WIFI.CONNECT <ssid> -> OK NEEDPASS | OK CONNECTED
 *                          | ERR NOTFOUND
 *   WIFI.PASS <pwd>     -> OK CONNECTED | ERR AUTH
 *                          | ERR NOCONN
 *   WIFI.LIST           -> OK <n>\n<ssid>,<sec>\n...
 *   WIFI.FORGET <ssid>  -> OK
 *   WIFI.STATUS         -> OK CONNECTED <ssid> | OK DISCONNECTED
 *   WIFI.DISC           -> OK
 *   BLE.ON / BLE.OFF    -> OK
 *   BLE.NAME <nama>     -> OK
 *   BLE.STATUS          -> OK ON|OFF <paired_addr|NONE>
 *                          (the mock never pairs: NONE)
 *   KV.SET <k> <len>\n<body> -> OK
 *   KV.GET <k>          -> OK <len>\n<body>
 *   KV.DEL <k>          -> OK
 *   TIME.GET            -> OK <iso8601>
 *   FIDO.LIST           -> OK 0
 * Mock extensions (not in the frozen v1 list, kept for
 * the screens): WIFI.STATUS, WIFI.DISC.
 * C6L-3 (wire v1.2, see
 * docs/rencana-komunikasi-c6/WIRE-FREEZE-V1.2.md):
 * TOOL.DISPATCH (stub tools get_info/gpio_read/
 * fail_test; unknown -> ERR NOTOOL), peer-initiated
 * DISPATCH + DISPATCH.RESULT (results captured for
 * the test), TEST.SCRIPT/TEST.BADFRAME (test-only
 * scripted inbound units), NET.WS.OPEN/CLOSE + BIN
 * echo, EVT frames (peer -> guest only).
 * Still absent: NET.HTTP (host peer only, C6L-2),
 * FIDO.DEL/APPROVE/DENY (command family; FIDO.EVENT
 * rides the generic EVT mechanism since C6L-3).
 */
#include "uartproto/uartproto.h"
#include "uartproto/link.h"
#include "ulib/ulib.h"
#include "../cfg/cfg.h"

/* Detik Unix (UTC) -> tanggal sipil WIB. Algoritma days-from-civil
 * (Hinnant), valid untuk seluruh rentang unix positif. */
static void
unix_to_wib(unsigned t, int *Y, int *M, int *D, int *h, int *m, int *s)
{
	long days, z, era, doe, yoe, y, doy, mp, d, mo;
	long rem;

	rem = (long)(t + 7u * 3600u);	/* WIB = UTC+7 */
	days = rem / 86400L;
	rem -= days * 86400L;

	z = days + 719468L;
	era = z / 146097L;
	doe = z - era * 146097L;			/* [0, 146096] */
	yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L;
	y = yoe + era * 400L;
	doy = doe - (365L * yoe + yoe / 4L - yoe / 100L);
	mp = (5L * doy + 2L) / 153L;
	d = doy - (153L * mp + 2L) / 5L + 1L;
	mo = mp + (mp < 10L ? 3L : -9L);
	y += (mo <= 2L);

	*Y = (int)y; *M = (int)mo; *D = (int)d;
	*h = (int)(rem / 3600L);
	*m = (int)((rem % 3600L) / 60L);
	*s = (int)(rem % 60L);
}

static void
put2(char *p, int v)	/* dua digit nol-di-depan */
{
	p[0] = (char)('0' + v / 10);
	p[1] = (char)('0' + v % 10);
}

/* TIME.GET: jam dinding real (NTP) bila sudah sinkron;
 * bila belum, label jujur seperti sebelumnya. */
static const char *
time_now(char *buf)
{
	unsigned t = sys_time_get();
	int Y, M, D, h, m, s;
	int i;
	static const char mock[] = "MOCK-2026-09-29T09:40:00+07:00";

	if (t == 0u) {
		for (i = 0; mock[i]; i++)
			buf[i] = mock[i];
		buf[i] = 0;
		return buf;
	}
	unix_to_wib(t, &Y, &M, &D, &h, &m, &s);
	put2(buf + 0, Y / 100); put2(buf + 2, Y % 100);
	buf[4] = '-';
	put2(buf + 5, M); buf[7] = '-';
	put2(buf + 8, D); buf[10] = 'T';
	put2(buf + 11, h); buf[13] = ':';
	put2(buf + 14, m); buf[16] = ':';
	put2(buf + 17, s);
	buf[19] = '+'; buf[20] = '0'; buf[21] = '7'; buf[22] = ':';
	buf[23] = '0'; buf[24] = '0'; buf[25] = 0;
	return buf;
}

#define SSID_SZ	32

/* Backing store tunggal: NVS di SD. */
static struct nvs_state	nvs;
static int		nvs_ready = 0;

/* State koneksi WiFi (runtime, tidak disimpan). */
static int	wifi_conn = 0;
static char	wifi_ssid[SSID_SZ] = "";
static char	wifi_pending[SSID_SZ] = "";	/* CONNECT yg tunggu PASS */

/* Jaringan "tetangga" hasil scan (canned). */
static const char *scan_ssid[3] = { "RUMAH-HAFIDZ", "KANTOR-5G", "WARUNGKOPI" };
static const char *scan_rssi[3] = { "-52", "-67", "-78" };
static const char *scan_sec[3] = { "WPA2", "WPA2", "OPEN" };

/* --- util string minimal (tanpa libc) --- */

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
sncmp(const char *a, const char *b, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return (int)(unsigned char)a[i] -
			       (int)(unsigned char)b[i];
		if (a[i] == 0)
			return 0;
	}
	return 0;
}

static void
scpy(char *d, const char *s, unsigned dsz)
{
	unsigned i = 0;
	while (s[i] && i + 1 < dsz) {
		d[i] = s[i];
		i++;
	}
	d[i] = 0;
}

/* --- Response byte queue (what read() drains) --- */

#define	RESP_Q_SZ	256u	/* largest mock response is ~85 B */
static unsigned char resp_q[RESP_Q_SZ];
static unsigned resp_len = 0, resp_pos = 0;

static void
qbytes(const unsigned char *p, unsigned n)
{
	unsigned i;
	for (i = 0; i < n && resp_len < RESP_Q_SZ; i++)
		resp_q[resp_len++] = p[i];
}

static void
qstr(const char *s)
{
	qbytes((const unsigned char *)s, slen(s));
}

/* "OK ..." + '\n'. p1 may be NULL (bare OK). */
static void
qok1(const char *p1)
{
	qstr("OK");
	if (p1) {
		qstr(" ");
		qstr(p1);
	}
	qstr("\n");
}

static void
qerr(const char *code)
{
	qstr("ERR ");
	qstr(code);
	qstr("\n");
}

static unsigned put_dec_raw(char *d, unsigned v);

/* "OK <len>\n" + exactly len body bytes (TOOL.DISPATCH
 * results, KV.GET-style length framing). */
static void
qok_body(const unsigned char *body, unsigned len)
{
	char num[12];
	unsigned i, n;

	n = put_dec_raw(num, len);
	qstr("OK ");
	for (i = 0; i < n; i++)
		qbytes((unsigned char *)&num[i], 1u);
	qstr("\n");
	qbytes(body, len);
}

static unsigned mock_crc16(const unsigned char *p, unsigned len);

/* Build one QA frame (type + payload) into out;
 * returns the frame length. CRC via mock_crc16. */
static unsigned
build_frame(unsigned char *out, unsigned type,
	    const unsigned char *payload, unsigned len)
{
	unsigned crc, i;

	out[0] = LINK_FRAME_MAGIC0;
	out[1] = LINK_FRAME_MAGIC1;
	out[2] = (unsigned char)type;
	out[3] = (unsigned char)((len >> 8) & 0xffu);
	out[4] = (unsigned char)(len & 0xffu);
	for (i = 0; i < len; i++)
		out[LINK_FRAME_HDR + i] = payload[i];
	crc = mock_crc16(out, LINK_FRAME_HDR + len);
	out[LINK_FRAME_HDR + len] = (unsigned char)((crc >> 8) & 0xffu);
	out[LINK_FRAME_HDR + len + 1u] = (unsigned char)(crc & 0xffu);
	return LINK_FRAME_HDR + len + LINK_FRAME_CRC;
}

/* Ambil argumen ke-n (0-based) dari line setelah spasi. */
static void
argn(const char *tx, int n, char *out, unsigned osz)
{
	unsigned i = 0, a = 0, o = 0;
	/* lewati perintah */
	while (tx[i] && tx[i] != ' ')
		i++;
	while (tx[i] == ' ')
		i++;
	while (a < (unsigned)n) {
		while (tx[i] && tx[i] != ' ')
			i++;
		while (tx[i] == ' ')
			i++;
		a++;
	}
	while (tx[i] && tx[i] != ' ' && tx[i] != '\n' && o + 1 < osz)
		out[o++] = tx[i++];
	out[o] = 0;
}

/* Muat NVS sekali (perintah pertama); gagal -> default seperti semula. */
static void
nvs_init(void)
{
	unsigned i;

	if (cfg_load(&nvs) == 0)
		return;
	for (i = 0; i < sizeof(nvs); i++)
		((unsigned char *)&nvs)[i] = 0;
	scpy(nvs.ble_name, "QABOT-01", sizeof(nvs.ble_name));
	/* ble_on=0, wifi_nsaved=0, kv_count=0 sudah nol. */
}

/* Hitung ulang kv_count dari slot terisi (dipanggil sebelum save). */
static void
kv_recount(void)
{
	unsigned char n = 0;
	int i;

	for (i = 0; i < NVS_KV_MAX; i++)
		if (nvs.kv[i].key[0])
			n++;
	nvs.kv_count = n;
}

static int
kv_find(const char *k)
{
	int i;
	for (i = 0; i < NVS_KV_MAX; i++)
		if (nvs.kv[i].key[0] && scmp(nvs.kv[i].key, k) == 0)
			return i;
	return -1;
}

static int
kv_free(void)
{
	int i;
	for (i = 0; i < NVS_KV_MAX; i++)
		if (!nvs.kv[i].key[0])
			return i;
	return -1;
}

/* --- Data plane state (declared before dispatch) --- */
static unsigned voice_frames;
static int voice_active;

/* --- C6L-3 state: WS session + scripted dispatch capture --- */
static int ws_open;
static unsigned ws_drop_n;

/* DISPATCH.RESULT receipts, captured for the guest
 * test (TEST.SCRIPT runs): up to 4, in arrival order.
 * Test-only inspection API at the end of this file. */
#define	SCRIPT_RES_N	4u
static unsigned script_res_id[SCRIPT_RES_N];
static char script_res_json[SCRIPT_RES_N][128];
static unsigned script_res_n;

/* Decimal output into d (no NUL); returns digits. */
static unsigned
put_dec_raw(char *d, unsigned v)
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

/* Find "pin":<digits> in a JSON body (not NUL-
 * terminated). Returns the pin, or -1. */
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

/* --- Command dispatch (one complete line, plus KV body) --- */

static void
dispatch(const char *tx, const unsigned char *body, unsigned bodylen)
{
	char a0[64], a1[64], tmp[128];
	unsigned i, o;

	if (!nvs_ready) {
		nvs_init();
		nvs_ready = 1;
	}

	/* --- WIFI --- */
	if (scmp(tx, "WIFI.SCAN") == 0) {
		o = 0;
		tmp[o++] = 'O';
		tmp[o++] = 'K';
		tmp[o++] = ' ';
		tmp[o++] = '3';
		tmp[o++] = '\n';
		for (i = 0; i < 3; i++) {
			const char *p;
			for (p = scan_ssid[i]; *p && o + 2 < sizeof(tmp); p++)
				tmp[o++] = *p;
			tmp[o++] = ',';
			for (p = scan_rssi[i]; *p && o + 2 < sizeof(tmp); p++)
				tmp[o++] = *p;
			tmp[o++] = ',';
			for (p = scan_sec[i]; *p && o + 2 < sizeof(tmp); p++)
				tmp[o++] = *p;
			tmp[o++] = '\n';
		}
		tmp[o] = 0;
		qstr(tmp);
		return;
	}
	if (sncmp(tx, "WIFI.CONNECT", 12) == 0 &&
	    (tx[12] == ' ' || tx[12] == 0)) {
		argn(tx, 0, a0, sizeof(a0));
		for (i = 0; i < 3; i++) {
			if (scmp(a0, scan_ssid[i]) == 0) {
				if (scmp(scan_sec[i], "OPEN") == 0) {
					wifi_conn = 1;
					scpy(wifi_ssid, a0, sizeof(wifi_ssid));
					wifi_pending[0] = 0;
					qok1("CONNECTED");
					return;
				}
				scpy(wifi_pending, a0, sizeof(wifi_pending));
				qok1("NEEDPASS");
				return;
			}
		}
		qerr("NOTFOUND");
		return;
	}
	if (sncmp(tx, "WIFI.PASS", 9) == 0 && (tx[9] == ' ' || tx[9] == 0)) {
		if (!wifi_pending[0]) {
			qerr("NOCONN");
			return;
		}
		argn(tx, 0, a0, sizeof(a0));
		if (slen(a0) >= 8) {
			wifi_conn = 1;
			scpy(wifi_ssid, wifi_pending, sizeof(wifi_ssid));
			wifi_pending[0] = 0;
			/* simpan (persisten) */
			if (nvs.wifi_nsaved < 2) {
				int dup = 0;
				for (i = 0; i < nvs.wifi_nsaved; i++)
					if (scmp(nvs.wifi_saved[i],
						 wifi_ssid) == 0)
						dup = 1;
				if (!dup) {
					scpy(nvs.wifi_saved[nvs.wifi_nsaved++],
					     wifi_ssid, SSID_SZ);
					cfg_save(&nvs); /* write-through */
				}
			}
			qok1("CONNECTED");
			return;
		}
		qerr("AUTH");
		return;
	}
	if (scmp(tx, "WIFI.LIST") == 0) {
		o = 0;
		tmp[o++] = 'O';
		tmp[o++] = 'K';
		tmp[o++] = ' ';
		tmp[o++] = (char)('0' + nvs.wifi_nsaved);
		tmp[o++] = '\n';
		for (i = 0; i < nvs.wifi_nsaved; i++) {
			const char *p;
			for (p = nvs.wifi_saved[i];
			     *p && o + 2 < sizeof(tmp); p++)
				tmp[o++] = *p;
			tmp[o++] = ',';
			tmp[o++] = 'W';
			tmp[o++] = 'P';
			tmp[o++] = 'A';
			tmp[o++] = '2';
			tmp[o++] = '\n';
		}
		tmp[o] = 0;
		qstr(tmp);
		return;
	}
	if (sncmp(tx, "WIFI.FORGET", 11) == 0) {
		argn(tx, 0, a0, sizeof(a0));
		for (i = 0; i < nvs.wifi_nsaved; i++) {
			if (scmp(nvs.wifi_saved[i], a0) == 0) {
				unsigned j;
				for (j = i; j + 1 < nvs.wifi_nsaved; j++)
					scpy(nvs.wifi_saved[j],
					     nvs.wifi_saved[j + 1], SSID_SZ);
				nvs.wifi_nsaved--;
				break;
			}
		}
		cfg_save(&nvs);	/* write-through; error diabaikan */
		qok1(0);
		return;
	}
	if (scmp(tx, "WIFI.STATUS") == 0) {
		if (wifi_conn) {
			scpy(tmp, "CONNECTED ", sizeof(tmp));
			o = slen(tmp);
			for (i = 0; wifi_ssid[i] && o + 2 < sizeof(tmp); i++)
				tmp[o++] = wifi_ssid[i];
			tmp[o] = 0;
			qok1(tmp);
			return;
		}
		qok1("DISCONNECTED");
		return;
	}
	if (scmp(tx, "WIFI.DISC") == 0) {
		wifi_conn = 0;
		wifi_pending[0] = 0;
		wifi_ssid[0] = 0;
		qok1(0);
		return;
	}

	/* --- BLE --- */
	if (scmp(tx, "BLE.ON") == 0) {
		nvs.ble_on = 1;
		cfg_save(&nvs);	/* write-through; error diabaikan */
		qok1(0);
		return;
	}
	if (scmp(tx, "BLE.OFF") == 0) {
		nvs.ble_on = 0;
		cfg_save(&nvs);	/* write-through; error diabaikan */
		qok1(0);
		return;
	}
	if (sncmp(tx, "BLE.NAME", 8) == 0) {
		argn(tx, 0, a0, sizeof(a0));
		if (a0[0]) {
			scpy(nvs.ble_name, a0, sizeof(nvs.ble_name));
			cfg_save(&nvs);	/* write-through; error diabaikan */
		}
		qok1(0);
		return;
	}
	if (scmp(tx, "BLE.STATUS") == 0) {
		/* Frozen v1.1: paired address, never the
		 * advertise name. This mock never pairs. */
		qok1(nvs.ble_on ? "ON NONE" : "OFF NONE");
		return;
	}

	/* --- KV (length-prefixed) --- */
	if (sncmp(tx, "KV.SET", 6) == 0) {
		int f;
		/* Body arrived with the line (parser's job);
		 * a line-only KV.SET is malformed. */
		if (body == 0) {
			qerr("ARG");
			return;
		}
		argn(tx, 0, a0, sizeof(a0));
		if (!a0[0] || bodylen > UPROTO_KV_VAL_MAX) {
			qerr("ARG");
			return;
		}
		f = kv_find(a0);
		if (f < 0)
			f = kv_free();
		if (f < 0) {
			qerr("FULL");
			return;
		}
		scpy(nvs.kv[f].key, a0, sizeof(nvs.kv[f].key));
		/* Value bytes are a C string in this store
		 * (no NULs); copy at most the slot size. */
		{
			unsigned n = (bodylen < sizeof(nvs.kv[f].val) - 1u) ?
			    bodylen : sizeof(nvs.kv[f].val) - 1u;
			for (i = 0; i < n; i++)
				nvs.kv[f].val[i] = (char)body[i];
			nvs.kv[f].val[n] = 0;
		}
		kv_recount();
		cfg_save(&nvs);	/* write-through; error diabaikan */
		qok1(0);
		return;
	}
	if (sncmp(tx, "KV.GET", 6) == 0) {
		int f;
		char num[12];
		unsigned vlen, j;
		argn(tx, 0, a0, sizeof(a0));
		f = kv_find(a0);
		if (f < 0) {
			qerr("NOTFOUND");
			return;
		}
		vlen = slen(nvs.kv[f].val);
		/* "OK <len>\n" + exactly len value bytes. */
		j = 0;
		do {
			num[j++] = (char)('0' + vlen % 10u);
			vlen /= 10u;
		} while (vlen && j < sizeof(num));
		qstr("OK ");
		for (i = 0; i < j; i++)
			{
				char c = num[j - 1u - i];
				qbytes((unsigned char *)&c, 1u);
			}
		qstr("\n");
		qbytes((unsigned char *)nvs.kv[f].val,
		       slen(nvs.kv[f].val));
		(void)a1;
		return;
	}
	if (sncmp(tx, "KV.DEL", 6) == 0) {
		int f;
		argn(tx, 0, a0, sizeof(a0));
		f = kv_find(a0);
		if (f >= 0) {
			nvs.kv[f].key[0] = 0;
			nvs.kv[f].val[0] = 0;
		}
		kv_recount();
		cfg_save(&nvs);	/* write-through; error diabaikan */
		qok1(0);
		return;
	}

	/* --- TIME --- */
	if (scmp(tx, "TIME.GET") == 0) {
		static char tbuf[32];
		qok1(time_now(tbuf));
		return;
	}

	/* --- FIDO --- */
	if (scmp(tx, "FIDO.LIST") == 0) {
		qok1("0");
		return;
	}

	/* --- C6L-3: TOOL.DISPATCH (guest -> peer) ---
	 * tx = "TOOL.DISPATCH <tool> <len>"; body = args
	 * JSON. Stub tools only: get_info (peer identity),
	 * gpio_read (deterministic pin table: value =
	 * pin & 1), fail_test (always a tool failure).
	 * Anything else is ERR NOTOOL. */
	if (sncmp(tx, "TOOL.DISPATCH", 13) == 0) {
		static const char info[] =
		    "{\"ok\":true,\"result\":{\"device\":"
		    "\"mock-comcu\",\"link\":\"v1.2\"}}";
		static const char failj[] =
		    "{\"ok\":false,\"error\":\"test_failure\"}";

		if (body == 0) {
			qerr("ARG");
			return;
		}
		argn(tx, 0, a0, sizeof(a0));
		if (!a0[0]) {
			qerr("ARG");
			return;
		}
		if (scmp(a0, "get_info") == 0) {
			qok_body((const unsigned char *)info,
			    slen(info));
			return;
		}
		if (scmp(a0, "gpio_read") == 0) {
			int pin = json_pin(body, bodylen);
			char jb[64];
			unsigned o = 0, n;

			if (pin < 0) {
				qerr("ARG");
				return;
			}
			{
				static const char p1[] =
				    "{\"ok\":true,\"result\":"
				    "{\"pin\":";
				static const char p2[] =
				    ",\"value\":";
				static const char p3[] = "}}";
				unsigned k;

				for (k = 0; p1[k]; k++)
					jb[o++] = p1[k];
				n = put_dec_raw(jb + o, (unsigned)pin);
				o += n;
				for (k = 0; p2[k]; k++)
					jb[o++] = p2[k];
				n = put_dec_raw(jb + o,
				    (unsigned)(pin & 1));
				o += n;
				for (k = 0; p3[k]; k++)
					jb[o++] = p3[k];
			}
			qok_body((unsigned char *)jb, o);
			return;
		}
		if (scmp(a0, "fail_test") == 0) {
			qok_body((const unsigned char *)failj,
			    slen(failj));
			return;
		}
		qerr("NOTOOL");
		return;
	}

	/* --- C6L-3: DISPATCH.RESULT (guest answers a
	 * peer-initiated dispatch) ---
	 * tx = "DISPATCH.RESULT <id> <len>"; body =
	 * result JSON. Receipts are captured for the
	 * guest test (see mock_comcu_script_result). */
	if (sncmp(tx, "DISPATCH.RESULT", 15) == 0) {
		unsigned idv = 0, k;

		if (body == 0) {
			qerr("ARG");
			return;
		}
		argn(tx, 0, a0, sizeof(a0));
		for (k = 0; a0[k] >= '0' && a0[k] <= '9'; k++)
			idv = idv * 10u + (unsigned)(a0[k] - '0');
		if (script_res_n < SCRIPT_RES_N) {
			unsigned n = (bodylen < 127u) ? bodylen : 127u;

			script_res_id[script_res_n] = idv;
			for (k = 0; k < n; k++)
				script_res_json[script_res_n][k] =
				    (char)body[k];
			script_res_json[script_res_n][n] = 0;
			script_res_n++;
		}
		qok1(0);
		return;
	}

	/* --- C6L-3: test-only scripted inbound units ---
	 * TEST.SCRIPT c6l3 -> "OK ARMED" + (in order) a
	 * DISPATCH get_info, a DISPATCH gpio_read, a
	 * DISPATCH gpio_write, a DISPATCH self_destruct,
	 * and an EVT WIFI.DROPPED frame. The guest polls
	 * and answers them.
	 * TEST.BADFRAME -> "OK" + one EVT frame with a
	 * corrupt CRC (the guest must report a link
	 * error, reopen, and carry on). */
	if (scmp(tx, "TEST.SCRIPT c6l3") == 0) {
		unsigned char fr[LINK_FRAME_HDR + 12 +
		    LINK_FRAME_CRC];
		unsigned flen;

		script_res_n = 0;
		qok1("ARMED");
		qstr("DISPATCH 1 get_info 2\n{}");
		qstr("DISPATCH 2 gpio_read 9\n{\"pin\":3}");
		qstr("DISPATCH 3 gpio_write 17\n"
		    "{\"pin\":3,\"val\":1}");
		qstr("DISPATCH 4 self_destruct 2\n{}");
		flen = build_frame(fr, LINK_FRAME_TYPE_EVT,
		    (const unsigned char *)"WIFI.DROPPED", 12u);
		qbytes(fr, flen);
		return;
	}
	if (scmp(tx, "TEST.BADFRAME") == 0) {
		unsigned char fr[LINK_FRAME_HDR + 12 +
		    LINK_FRAME_CRC];
		unsigned flen;

		qok1(0);
		flen = build_frame(fr, LINK_FRAME_TYPE_EVT,
		    (const unsigned char *)"WIFI.DROPPED", 12u);
		/* Corrupt the CRC: the guest must fail the
		 * unit, not accept the event. */
		fr[flen - 1u] ^= 0xffu;
		qbytes(fr, flen);
		return;
	}

	/* --- C6L-3: NET.WS session (framing only) ---
	 * One session at a time (sid always 1). OPEN
	 * while open is ERR BUSY; CLOSE with no session
	 * or a wrong sid is ERR NOCONN. */
	if (sncmp(tx, "NET.WS.OPEN", 11) == 0) {
		if (ws_open) {
			qerr("BUSY");
			return;
		}
		ws_open = 1;
		qok1("1");
		return;
	}
	if (sncmp(tx, "NET.WS.CLOSE", 12) == 0) {
		argn(tx, 0, a0, sizeof(a0));
		if (!ws_open || scmp(a0, "1") != 0) {
			qerr("NOCONN");
			return;
		}
		ws_open = 0;
		qok1(0);
		return;
	}

	/* --- VOICE (protokol v2, D4) --- */
	if (scmp(tx, "VOICE.START 16000 16") == 0) {
		voice_frames = 0u;
		voice_active = 1;
		qok1("");
		return;
	}
	if (scmp(tx, "VOICE.STOP") == 0) {
		char nbuf[16];
		unsigned i = 0u, v = voice_frames;

		voice_active = 0;
		do {
			nbuf[i++] = (char)('0' + v % 10u);
			v /= 10u;
		} while (v && i + 1 < sizeof(nbuf));
		nbuf[i] = 0;
		{
			unsigned j;
			for (j = 0u; j < i / 2u; j++) {
				char t = nbuf[j];
				nbuf[j] = nbuf[i - 1u - j];
				nbuf[i - 1u - j] = t;
			}
		}
		qok1(nbuf);
		return;
	}

	qerr("UNKNOWN");
}

/* --- Data plane: penerima frame AUD (D4) --- */
static unsigned
mock_crc16(const unsigned char *p, unsigned len)
{
	unsigned crc = 0xffffu;
	unsigned i, j;

	for (i = 0; i < len; i++) {
		crc ^= (unsigned)p[i] << 8;
		for (j = 0u; j < 8u; j++)
			crc = (crc & 0x8000u) ? (crc << 1) ^ 0x1021u :
			    crc << 1;
	}
	return crc & 0xffffu;
}

/* Frozen v2 frame: "QA" + type + len BE + payload + CRC BE.
 * AUD (0x01) only here; BIN (0x02, WS echo) is routed
 * in mock_send_frame, EVT (0x03) never comes from the
 * guest. The legacy 3-byte "AUD" magic is rejected. */
static int
aud_accept(const unsigned char *frame, unsigned flen)
{
	unsigned len, crc, expect;

	if (!voice_active)
		return -1;
	if (flen < LINK_FRAME_HDR + LINK_FRAME_CRC)
		return -1;
	if (frame[0] != LINK_FRAME_MAGIC0 ||
	    frame[1] != LINK_FRAME_MAGIC1)
		return -1;
	if (frame[2] != LINK_FRAME_TYPE_AUD)
		return -1;
	len = ((unsigned)frame[3] << 8) | frame[4];
	if (flen != LINK_FRAME_HDR + len + LINK_FRAME_CRC)
		return -1;
	crc = ((unsigned)frame[LINK_FRAME_HDR + len] << 8) |
	    frame[LINK_FRAME_HDR + len + 1u];
	expect = mock_crc16(frame, LINK_FRAME_HDR + len);
	if (crc != expect)
		return -1;
	voice_frames++;
	return 0;
}

/* --- Backend: byte-stream parser ---
 *
 * write() appends to the input accumulator; the parser
 * consumes complete requests: a line, plus for KV.SET /
 * TOOL.DISPATCH / DISPATCH.RESULT exactly <len>
 * following body bytes. Malformed input policy: a
 * body line whose length is not numeric (the legacy
 * inline shape) is rejected at once with ERR ARG and
 * no body wait; a numeric length above the sane bound
 * (4096) drops the whole accumulator (the link layer
 * then times out and must reopen); a body longer than
 * the store cap is consumed and then rejected with
 * ERR ARG, keeping the stream in sync. Dispatch
 * bodies over 192 B are rejected at once with ERR
 * ARG (the guest guards the same cap, so no body
 * follows in practice). Overflow of the accumulator
 * is a link error (write returns -1); the caller
 * must reopen. */
#define	IN_ACC_SZ	256u	/* largest single write is ~240 B */
#define	BODY_MAX	4096u
#define	READ_CHUNK	32u
#define	BODY_DISPATCH_MAX	192u

static unsigned char in_acc[IN_ACC_SZ];
static unsigned in_len = 0;
static int need_body = -1;	/* >= 0: body bytes still wanted */
static int body_kind = 0;	/* 0 KV.SET, 1 TOOL.DISPATCH,
				 * 2 DISPATCH.RESULT */
static char body_key[64];	/* KV key, or dispatch tool name */
static unsigned body_id = 0;	/* DISPATCH.RESULT id */
static unsigned body_want = 0;

/* Strict parse of "KV.SET <key> <len>". Returns 0 with
 * key/len filled, or -1 (not a KV.SET line, or
 * malformed). */
static int
parse_kv_set(const char *line, char *key, unsigned kcap,
	     unsigned *len_out)
{
	unsigned i = 0, o = 0, len = 0;
	int digits = 0;

	if (sncmp(line, "KV.SET", 6) != 0)
		return -1;
	i = 6;
	if (line[i] != ' ')
		return -1;
	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && o + 1 < kcap)
		key[o++] = line[i++];
	if (o == 0)
		return -1;
	key[o] = 0;
	if (line[i] != ' ')
		return -1;
	while (line[i] == ' ')
		i++;
	while (line[i] >= '0' && line[i] <= '9') {
		len = len * 10u + (unsigned)(line[i] - '0');
		if (len > BODY_MAX + 1u)
			return -1;
		digits = 1;
		i++;
	}
	if (!digits || line[i] != 0)
		return -1;
	*len_out = len;
	return 0;
}

static void
parser_reset(void)
{
	in_len = 0;
	need_body = -1;
	body_kind = 0;
	body_id = 0;
	body_want = 0;
	resp_len = resp_pos = 0;
}

/* Strict parse of "CMD <token> <len>" (TOOL.DISPATCH
 * tool names). Returns 0 with token/len filled, or
 * -1. Mirrors parse_kv_set's grammar. */
static int
parse_cmd_tok_len(const char *line, const char *cmd, char *tok,
		  unsigned tcap, unsigned *len_out)
{
	unsigned i = 0, o = 0, len = 0;
	int digits = 0;

	while (cmd[i] && line[i] == cmd[i])
		i++;
	if (cmd[i] != 0 || line[i] != ' ')
		return -1;
	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && o + 1 < tcap)
		tok[o++] = line[i++];
	if (o == 0 || line[i] != ' ')
		return -1;
	tok[o] = 0;
	while (line[i] == ' ')
		i++;
	while (line[i] >= '0' && line[i] <= '9') {
		len = len * 10u + (unsigned)(line[i] - '0');
		if (len > BODY_MAX + 1u)
			return -1;
		digits = 1;
		i++;
	}
	if (!digits || line[i] != 0)
		return -1;
	*len_out = len;
	return 0;
}

/* Strict parse of "DISPATCH.RESULT <id> <len>".
 * Returns 0 with id/len filled, or -1. */
static int
parse_cmd_id_len(const char *line, unsigned *id_out,
		 unsigned *len_out)
{
	unsigned i = 15, id = 0, len = 0;
	int digits = 0;

	if (sncmp(line, "DISPATCH.RESULT", 15) != 0 ||
	    line[15] != ' ')
		return -1;
	while (line[i] == ' ')
		i++;
	while (line[i] >= '0' && line[i] <= '9') {
		id = id * 10u + (unsigned)(line[i] - '0');
		digits = 1;
		i++;
	}
	if (!digits || line[i] != ' ')
		return -1;
	while (line[i] == ' ')
		i++;
	digits = 0;
	while (line[i] >= '0' && line[i] <= '9') {
		len = len * 10u + (unsigned)(line[i] - '0');
		if (len > BODY_MAX + 1u)
			return -1;
		digits = 1;
		i++;
	}
	if (!digits || line[i] != 0)
		return -1;
	*id_out = id;
	*len_out = len;
	return 0;
}

static int
mock_open(void)
{
	parser_reset();
	return 0;
}

static int
mock_close(void)
{
	parser_reset();
	return 0;
}

static int
mock_write(const unsigned char *buf, unsigned len)
{
	unsigned i;

	if (in_len + len > IN_ACC_SZ)
		return -1;
	for (i = 0; i < len; i++)
		in_acc[in_len++] = buf[i];

	for (;;) {
		/* Body phase: consume exactly body_want bytes. */
		if (need_body >= 0) {
			unsigned have;
			if (in_len < body_want)
				return 0;	/* wait for more */
			have = body_want;
			{
				/* Stage up to the dispatch cap
				 * (192 B); KV bodies are still
				 * rejected above 64 by dispatch,
				 * never stored truncated. Guest
				 * stacks are one page: no 4 KB
				 * array. */
				unsigned char body[BODY_DISPATCH_MAX];
				unsigned take = (have < BODY_DISPATCH_MAX) ?
				    have : BODY_DISPATCH_MAX;
				unsigned k;
				for (k = 0; k < take; k++)
					body[k] = in_acc[k];
				/* Shift the accumulator. */
				for (k = 0; k + have < in_len; k++)
					in_acc[k] = in_acc[k + have];
				in_len -= have;
				need_body = -1;
				{
					char line[128];
					unsigned li = 0, n;
					const char *p;

					if (body_kind == 1)
						p = "TOOL.DISPATCH ";
					else if (body_kind == 2)
						p = "DISPATCH.RESULT ";
					else
						p = "KV.SET ";
					while (*p)
						line[li++] = *p++;
					if (body_kind == 2) {
						n = put_dec_raw(line + li,
						    body_id);
						li += n;
					} else {
						for (k = 0; body_key[k]; k++)
							line[li++] =
							    body_key[k];
					}
					line[li++] = ' ';
					n = put_dec_raw(line + li, have);
					li += n;
					line[li] = 0;
					if (body_kind == 0 &&
					    have > UPROTO_KV_VAL_MAX) {
						qerr("ARG");
					} else if (body_kind != 0 &&
					    have > BODY_DISPATCH_MAX) {
						qerr("ARG");
					} else {
						dispatch(line, body, have);
					}
				}
			}
			continue;
		}
		/* Line phase: find '\n'. */
		{
			unsigned nl, k;
			int found = -1;
			for (k = 0; k < in_len; k++) {
				if (in_acc[k] == '\n') {
					found = (int)k;
					break;
				}
			}
			if (found < 0)
				return 0;	/* wait for more */
			nl = (unsigned)found;
			{
				char line[192];
				unsigned li = 0;
				for (k = 0; k < nl && li + 1 < sizeof(line); k++) {
					if (in_acc[k] == '\r')
						continue;
					line[li++] = in_acc[k];
				}
				line[li] = 0;
				/* Shift past the newline. */
				for (k = 0; k + nl + 1u < in_len; k++)
					in_acc[k] = in_acc[k + nl + 1u];
				in_len -= nl + 1u;
				/* KV.SET with a body? */
				if (sncmp(line, "KV.SET", 6) == 0) {
					char key[64];
					unsigned blen;
					if (parse_kv_set(line, key,
							 sizeof(key),
							 &blen) != 0) {
						/* Legacy inline shape
						 * or garbage: reject at
						 * once, no body wait. */
						qerr("ARG");
						continue;
					}
					if (blen > BODY_MAX) {
						/* Unreachable via the
						 * parser bound, kept as
						 * defence: drop stream. */
						parser_reset();
						return -1;
					}
					scpy(body_key, key, sizeof(body_key));
					body_kind = 0;
					if (blen == 0) {
						dispatch(line,
							 (unsigned char *)"",
							 0u);
					} else {
						body_want = blen;
						need_body = 1;
					}
					continue;
				}
				/* TOOL.DISPATCH with a body? */
				if (sncmp(line, "TOOL.DISPATCH", 13) == 0) {
					char tool[32];
					unsigned blen;
					if (parse_cmd_tok_len(line,
						"TOOL.DISPATCH", tool,
						sizeof(tool), &blen) != 0) {
						qerr("ARG");
						continue;
					}
					if (blen > BODY_DISPATCH_MAX) {
						/* Guest guards the same
						 * cap: reject at once,
						 * no body wait. */
						qerr("ARG");
						continue;
					}
					scpy(body_key, tool,
					    sizeof(body_key));
					body_kind = 1;
					if (blen == 0) {
						dispatch(line,
							 (unsigned char *)"",
							 0u);
					} else {
						body_want = blen;
						need_body = 1;
					}
					continue;
				}
				/* DISPATCH.RESULT with a body? */
				if (sncmp(line, "DISPATCH.RESULT", 15) == 0) {
					unsigned idv, blen;
					if (parse_cmd_id_len(line, &idv,
						&blen) != 0) {
						qerr("ARG");
						continue;
					}
					if (blen > BODY_DISPATCH_MAX) {
						qerr("ARG");
						continue;
					}
					body_id = idv;
					body_kind = 2;
					if (blen == 0) {
						dispatch(line,
							 (unsigned char *)"",
							 0u);
					} else {
						body_want = blen;
						need_body = 1;
					}
					continue;
				}
				dispatch(line, 0, 0u);
			}
		}
	}
}

static int
mock_read(unsigned char *buf, unsigned len)
{
	unsigned avail, n, i;

	if (resp_pos >= resp_len)
		return 0;
	avail = resp_len - resp_pos;
	n = avail;
	if (n > len)
		n = len;
	if (n > READ_CHUNK)
		n = READ_CHUNK;
	for (i = 0; i < n; i++)
		buf[i] = resp_q[resp_pos++];
	if (resp_pos >= resp_len)
		resp_len = resp_pos = 0;
	return (int)n;
}

static int
mock_send_frame(const unsigned char *frame, unsigned flen)
{
	unsigned len, crc, expect;

	if (flen < LINK_FRAME_HDR + LINK_FRAME_CRC ||
	    frame[0] != LINK_FRAME_MAGIC0 ||
	    frame[1] != LINK_FRAME_MAGIC1)
		return -1;
	if (frame[2] == LINK_FRAME_TYPE_BIN) {
		/* WS data: validate, then echo verbatim
		 * while a session is open; with no session
		 * the frame is dropped (frames get no
		 * response) and the send reports -1. */
		len = ((unsigned)frame[3] << 8) | frame[4];
		if (flen != LINK_FRAME_HDR + len + LINK_FRAME_CRC)
			return -1;
		crc = ((unsigned)frame[LINK_FRAME_HDR + len] << 8) |
		    frame[LINK_FRAME_HDR + len + 1u];
		expect = mock_crc16(frame, LINK_FRAME_HDR + len);
		if (crc != expect)
			return -1;
		if (!ws_open) {
			ws_drop_n++;
			return -1;
		}
		/* Echo fits resp_q only for small messages;
		 * the WS test keeps them small. */
		qbytes(frame, flen);
		return 0;
	}
	return aud_accept(frame, flen);
}

/* --- Test-only inspection API (C6L-3) ---
 * The guest test reads back the DISPATCH.RESULT
 * receipts the mock captured, to prove the peer
 * actually received the guest's answers (the bridge
 * leg proves the same via the host transcript). */
unsigned
mock_comcu_script_result_count(void)
{
	return script_res_n;
}

int
mock_comcu_script_result(unsigned idx, unsigned *id_out, char *buf,
			  unsigned cap)
{
	unsigned i;

	if (idx >= script_res_n || !buf || cap == 0)
		return -1;
	if (id_out)
		*id_out = script_res_id[idx];
	for (i = 0; script_res_json[idx][i] && i + 1 < cap; i++)
		buf[i] = script_res_json[idx][i];
	buf[i] = 0;
	return 0;
}

struct uproto_backend uproto_backend_mock = {
	"mock",
	mock_open,
	mock_write,
	mock_read,
	mock_send_frame,
	mock_close,
};
