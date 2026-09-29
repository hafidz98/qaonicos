/*
 * mock_comcu.c - SIMULATOR co-MCU ESP32-C3 untuk QEMU.
 *
 * JUJUR: ini bukan hardware. co-MCU fisik belum ada; semua respons
 * canned agar alur UI WiFi/BLE/LLM/Passkey dapat diuji end-to-end di
 * QEMU.  Saat ESP32-C3 tiba: ganti xchg() di uartproto.c dengan
 * tulis/baca UART PL011 data — antarmuka uproto_* tidak berubah.
 *
 * Backing store = struct nvs_state (user/cfg/cfg.h) yang persisten di
 * 2 sektor NVS kartu SD (write-through tiap mutasi): KV LLM, jaringan
 * WiFi tersimpan, dan state/nama BLE selamat dari reboot. State
 * koneksi WiFi saat ini (wifi_conn/ssid/pending) tetap runtime saja.
 *
 * Perintah yang didukung (RENCANA-app §4):
 *   WIFI.SCAN -> OK <n>\n<ssid>,<rssi>,<sec>\n...
 *   WIFI.CONNECT <ssid> -> OK NEEDPASS | OK CONNECTED | ERR NOTFOUND
 *   WIFI.PASS <pwd>     -> OK CONNECTED | ERR AUTH | ERR NOCONN
 *   WIFI.LIST           -> OK <n>\n<ssid>,<sec>\n...
 *   WIFI.FORGET <ssid>  -> OK
 *   WIFI.STATUS         -> OK CONNECTED <ssid> | OK DISCONNECTED
 *   WIFI.DISC           -> OK
 *   BLE.ON / BLE.OFF    -> OK
 *   BLE.NAME <nama>     -> OK
 *   BLE.STATUS          -> OK ON|OFF <nama>
 *   KV.SET <k> <v>      -> OK
 *   KV.GET <k>          -> OK <v> | ERR NOTFOUND
 *   KV.DEL <k>          -> OK
 *   TIME.GET            -> OK <iso8601> (jam dinding real via NTP;
 *                              label MOCK-... bila belum sinkron)
 *   FIDO.LIST           -> OK 0
 */
#include "uartproto/uartproto.h"
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

/* Tulis "OK ...\n" ke rx. Return 0. */
static int
ok1(const char *p1, char *rx, unsigned rsz)
{
	unsigned i = 0, j = 0;
	rx[i++] = 'O';
	rx[i++] = 'K';
	if (p1) {
		rx[i++] = ' ';
		while (p1[j] && i + 2 < rsz)
			rx[i++] = p1[j++];
	}
	rx[i++] = '\n';
	rx[i] = 0;
	return 0;
}

static int
err(const char *code, char *rx, unsigned rsz)
{
	unsigned i = 0, j = 0;
	rx[i++] = 'E';
	rx[i++] = 'R';
	rx[i++] = 'R';
	rx[i++] = ' ';
	while (code[j] && i + 2 < rsz)
		rx[i++] = code[j++];
	rx[i++] = '\n';
	rx[i] = 0;
	return 0;
}

/* Ambil argumen ke-n (0-based) dari tx setelah spasi. */
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

int
mock_comcu_handle(const char *tx, char *rx, unsigned rsz)
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
		scpy(rx, tmp, rsz);
		return 0;
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
					return ok1("CONNECTED", rx, rsz);
				}
				scpy(wifi_pending, a0, sizeof(wifi_pending));
				return ok1("NEEDPASS", rx, rsz);
			}
		}
		return err("NOTFOUND", rx, rsz);
	}
	if (sncmp(tx, "WIFI.PASS", 9) == 0 && (tx[9] == ' ' || tx[9] == 0)) {
		if (!wifi_pending[0])
			return err("NOCONN", rx, rsz);
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
			return ok1("CONNECTED", rx, rsz);
		}
		return err("AUTH", rx, rsz);
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
		scpy(rx, tmp, rsz);
		return 0;
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
		return ok1(0, rx, rsz);
	}
	if (scmp(tx, "WIFI.STATUS") == 0) {
		if (wifi_conn) {
			scpy(tmp, "CONNECTED ", sizeof(tmp));
			o = slen(tmp);
			for (i = 0; wifi_ssid[i] && o + 2 < sizeof(tmp); i++)
				tmp[o++] = wifi_ssid[i];
			tmp[o] = 0;
			return ok1(tmp, rx, rsz);
		}
		return ok1("DISCONNECTED", rx, rsz);
	}
	if (scmp(tx, "WIFI.DISC") == 0) {
		wifi_conn = 0;
		wifi_ssid[0] = 0;
		return ok1(0, rx, rsz);
	}

	/* --- BLE --- */
	if (scmp(tx, "BLE.ON") == 0) {
		nvs.ble_on = 1;
		cfg_save(&nvs);	/* write-through; error diabaikan */
		return ok1(0, rx, rsz);
	}
	if (scmp(tx, "BLE.OFF") == 0) {
		nvs.ble_on = 0;
		cfg_save(&nvs);	/* write-through; error diabaikan */
		return ok1(0, rx, rsz);
	}
	if (sncmp(tx, "BLE.NAME", 8) == 0) {
		argn(tx, 0, a0, sizeof(a0));
		if (a0[0]) {
			scpy(nvs.ble_name, a0, sizeof(nvs.ble_name));
			cfg_save(&nvs);	/* write-through; error diabaikan */
		}
		return ok1(0, rx, rsz);
	}
	if (scmp(tx, "BLE.STATUS") == 0) {
		scpy(tmp, nvs.ble_on ? "ON " : "OFF ", sizeof(tmp));
		o = slen(tmp);
		for (i = 0; nvs.ble_name[i] && o + 2 < sizeof(tmp); i++)
			tmp[o++] = nvs.ble_name[i];
		tmp[o] = 0;
		return ok1(tmp, rx, rsz);
	}

	/* --- KV --- */
	if (sncmp(tx, "KV.SET", 6) == 0) {
		int f;
		argn(tx, 0, a0, sizeof(a0));
		argn(tx, 1, a1, sizeof(a1));
		if (!a0[0])
			return err("ARG", rx, rsz);
		f = kv_find(a0);
		if (f < 0)
			f = kv_free();
		if (f < 0)
			return err("FULL", rx, rsz);
		scpy(nvs.kv[f].key, a0, sizeof(nvs.kv[f].key));
		scpy(nvs.kv[f].val, a1, sizeof(nvs.kv[f].val));
		kv_recount();
		cfg_save(&nvs);	/* write-through; error diabaikan */
		return ok1(0, rx, rsz);
	}
	if (sncmp(tx, "KV.GET", 6) == 0) {
		int f;
		argn(tx, 0, a0, sizeof(a0));
		f = kv_find(a0);
		if (f < 0)
			return err("NOTFOUND", rx, rsz);
		return ok1(nvs.kv[f].val, rx, rsz);
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
		return ok1(0, rx, rsz);
	}

	/* --- TIME --- */
	if (scmp(tx, "TIME.GET") == 0) {
		static char tbuf[32];
		return ok1(time_now(tbuf), rx, rsz);
	}

	/* --- FIDO --- */
	if (scmp(tx, "FIDO.LIST") == 0)
		return ok1("0", rx, rsz);

	return err("UNKNOWN", rx, rsz);
}
