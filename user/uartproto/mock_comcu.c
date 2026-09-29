/*
 * mock_comcu.c - SIMULATOR co-MCU ESP32-C3 untuk QEMU.
 *
 * JUJUR: ini bukan hardware. co-MCU fisik belum ada; semua respons
 * canned agar alur UI WiFi/BLE/LLM/Passkey dapat diuji end-to-end di
 * QEMU.  Saat ESP32-C3 tiba: ganti xchg() di uartproto.c dengan
 * tulis/baca UART PL011 data — antarmuka uproto_* tidak berubah.
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
 *   TIME.GET            -> OK <iso8601> (basis: waktu build mock)
 *   FIDO.LIST           -> OK 0
 */
#include "uartproto/uartproto.h"

#define SSID_SZ	32
#define KV_N	8
#define KV_SZ	48

static int	ble_on = 0;
static char	ble_name[24] = "QABOT-01";

static int	wifi_conn = 0;
static char	wifi_ssid[SSID_SZ] = "";
static char	wifi_pending[SSID_SZ] = "";	/* CONNECT yg tunggu PASS */

/* Jaringan "tetangga" hasil scan (canned). */
static const char *scan_ssid[3] = { "RUMAH-HAFIDZ", "KANTOR-5G", "WARUNGKOPI" };
static const char *scan_rssi[3] = { "-52", "-67", "-78" };
static const char *scan_sec[3] = { "WPA2", "WPA2", "OPEN" };

/* Jaringan tersimpan. */
static char	saved_ssid[2][SSID_SZ];
static int	nsaved = 0;

/* KV store mock (pengganti NVS ESP32). */
static char	kv_k[KV_N][KV_SZ];
static char	kv_v[KV_N][KV_SZ];

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

static int
kv_find(const char *k)
{
	int i;
	for (i = 0; i < KV_N; i++)
		if (kv_k[i][0] && scmp(kv_k[i], k) == 0)
			return i;
	return -1;
}

static int
kv_free(void)
{
	int i;
	for (i = 0; i < KV_N; i++)
		if (!kv_k[i][0])
			return i;
	return -1;
}

int
mock_comcu_handle(const char *tx, char *rx, unsigned rsz)
{
	char a0[64], a1[64], tmp[128];
	unsigned i, o;

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
			/* simpan */
			if (nsaved < 2) {
				int dup = 0;
				for (i = 0; i < (unsigned)nsaved; i++)
					if (scmp(saved_ssid[i], wifi_ssid) == 0)
						dup = 1;
				if (!dup)
					scpy(saved_ssid[nsaved++], wifi_ssid,
					     SSID_SZ);
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
		tmp[o++] = (char)('0' + nsaved);
		tmp[o++] = '\n';
		for (i = 0; i < (unsigned)nsaved; i++) {
			const char *p;
			for (p = saved_ssid[i]; *p && o + 2 < sizeof(tmp); p++)
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
		for (i = 0; i < (unsigned)nsaved; i++) {
			if (scmp(saved_ssid[i], a0) == 0) {
				unsigned j;
				for (j = i; j + 1 < (unsigned)nsaved; j++)
					scpy(saved_ssid[j], saved_ssid[j + 1],
					     SSID_SZ);
				nsaved--;
				break;
			}
		}
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
		ble_on = 1;
		return ok1(0, rx, rsz);
	}
	if (scmp(tx, "BLE.OFF") == 0) {
		ble_on = 0;
		return ok1(0, rx, rsz);
	}
	if (sncmp(tx, "BLE.NAME", 8) == 0) {
		argn(tx, 0, a0, sizeof(a0));
		if (a0[0])
			scpy(ble_name, a0, sizeof(ble_name));
		return ok1(0, rx, rsz);
	}
	if (scmp(tx, "BLE.STATUS") == 0) {
		scpy(tmp, ble_on ? "ON " : "OFF ", sizeof(tmp));
		o = slen(tmp);
		for (i = 0; ble_name[i] && o + 2 < sizeof(tmp); i++)
			tmp[o++] = ble_name[i];
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
		scpy(kv_k[f], a0, KV_SZ);
		scpy(kv_v[f], a1, KV_SZ);
		return ok1(0, rx, rsz);
	}
	if (sncmp(tx, "KV.GET", 6) == 0) {
		int f;
		argn(tx, 0, a0, sizeof(a0));
		f = kv_find(a0);
		if (f < 0)
			return err("NOTFOUND", rx, rsz);
		return ok1(kv_v[f], rx, rsz);
	}
	if (sncmp(tx, "KV.DEL", 6) == 0) {
		int f;
		argn(tx, 0, a0, sizeof(a0));
		f = kv_find(a0);
		if (f >= 0) {
			kv_k[f][0] = 0;
			kv_v[f][0] = 0;
		}
		return ok1(0, rx, rsz);
	}

	/* --- TIME --- */
	if (scmp(tx, "TIME.GET") == 0)
		/* Mock: jam "real" belum ada (butuh NTP/RTC); label jujur. */
		return ok1("MOCK-2026-09-29T09:40:00+07:00", rx, rsz);

	/* --- FIDO --- */
	if (scmp(tx, "FIDO.LIST") == 0)
		return ok1("0", rx, rsz);

	return err("UNKNOWN", rx, rsz);
}
