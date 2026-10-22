/*
 * mach3/kernel/arm/http.c -- Server HTTP/1.0 minimal untuk system
 * monitor (Fase D).
 *
 * Port dari archive/kernel-scratch/kernel/src/http.c (Fase 12/12b).
 * Routes:
 *   GET /        -> dashboard HTML "QaonicOS System Monitor" (nilai live,
 *                   di-generate per request; auto-refresh 5 detik)
 *   GET /metrics -> text/plain: uptime, memori, storage, network
 *   lainnya     -> 404
 *
 * Statistik diadaptasi ke API Mach 3:
 *  - Uptime: arm_timer_ticks() (100 Hz)
 *  - Memory: vm_page_free_count vs mem_size (real)
 *  - Storage: blk_total_sectors() / blk_nsectors_dev(1) (real)
 *  - Network: net_rx_bytes_get()/net_tx_bytes_get() (real)
 *  - TCP: tcp_*_segs()/tcp_conns() (real)
 */

/* Kernel API yang dipakai. */
extern unsigned int	arm_timer_ticks(void);
extern int		vm_page_free_count;
extern unsigned long	mem_size;	/* vm_offset_t */
extern unsigned int	blk_total_sectors(void);
extern unsigned int	blk_nsectors_dev(unsigned int dev);
extern const unsigned char *net_mac(void);
extern unsigned long long net_rx_bytes_get(void);
extern unsigned long long net_tx_bytes_get(void);
extern unsigned int	tcp_rx_segs(void);
extern unsigned int	tcp_tx_segs(void);
extern unsigned int	tcp_conns(void);
extern unsigned int	netstack_rx_frames(void);
extern unsigned int	netstack_arp_hits(void);
extern unsigned int	netstack_ping_replies(void);

/* RAM guest di QEMU: -m 64 (spek Luckfox Pico Mini: 64MB DDR2). */
#define	GUEST_RAM_MB	64u

/* --- util string --- */
static unsigned
strapp(unsigned char *d, unsigned off, const char *s)
{
	while (*s)
		d[off++] = (unsigned char)*s++;
	return off;
}

static unsigned
u32app(unsigned char *d, unsigned off, unsigned int v)
{
	char tmp[12];
	int n = 0, i;

	if (v == 0u)
		tmp[n++] = '0';
	else {
		while (v > 0u) {
			tmp[n++] = (char)('0' + v % 10u);
			v /= 10u;
		}
	}
	for (i = n - 1; i >= 0; i--)
		d[off++] = (unsigned char)tmp[i];
	return off;
}

static unsigned
u64app(unsigned char *d, unsigned off, unsigned long long v)
{
	char tmp[20];
	int n = 0, k;

	if (v == 0u)
		tmp[n++] = '0';
	else {
		while (v > 0u) {
			tmp[n++] = (char)('0' + (unsigned)(v % 10u));
			v /= 10u;
		}
	}
	for (k = n - 1; k >= 0; k--)
		d[off++] = (unsigned char)tmp[k];
	return off;
}

static unsigned
hexapp(unsigned char *d, unsigned off, unsigned char v)
{
	static const char *hx = "0123456789abcdef";
	d[off++] = (unsigned char)hx[v >> 4];
	d[off++] = (unsigned char)hx[v & 15u];
	return off;
}

/* Bandwidth: persepuluh Kbps per window >=500ms, dari counter byte. */
static void
bw_update(unsigned int *dn, unsigned int *up)
{
	static unsigned int last_t;
	static unsigned long long last_rx, last_tx;
	static unsigned int dn10, up10;
	unsigned int t = arm_timer_ticks() * 10u; /* -> ms */
	unsigned int dt = t - last_t;

	if (dt >= 500u) {
		unsigned long long rx = net_rx_bytes_get();
		unsigned long long tx = net_tx_bytes_get();
		unsigned int drx = (unsigned int)(rx - last_rx);
		unsigned int dtx = (unsigned int)(tx - last_tx);
		dn10 = (drx * 80u) / dt;
		up10 = (dtx * 80u) / dt;
		last_t = t;
		last_rx = rx;
		last_tx = tx;
	}
	*dn = dn10;
	*up = up10;
}

static unsigned
rateapp(unsigned char *d, unsigned off, unsigned int tenth_kbps)
{
	if (tenth_kbps >= 10000u) {
		unsigned int tm = tenth_kbps / 1000u;
		off = u32app(d, off, tm / 10u);
		off = strapp(d, off, ".");
		off = u32app(d, off, tm % 10u);
		off = strapp(d, off, " Mbps");
	} else {
		off = u32app(d, off, tenth_kbps / 10u);
		off = strapp(d, off, ".");
		off = u32app(d, off, tenth_kbps % 10u);
		off = strapp(d, off, " Kbps");
	}
	return off;
}

static unsigned
pct1app(unsigned char *d, unsigned off, unsigned int tenth_pct)
{
	off = u32app(d, off, tenth_pct / 10u);
	off = strapp(d, off, ".");
	off = u32app(d, off, tenth_pct % 10u);
	return off;
}

/* --- body /metrics --- */
static unsigned
build_metrics(unsigned char *b)
{
	unsigned off = 0;
	unsigned int ms = arm_timer_ticks() * 10u;
	unsigned int total_pages = (unsigned int)(mem_size / 4096u);
	unsigned int free_pages = (unsigned int)vm_page_free_count;
	unsigned int used_pages =
	    total_pages > free_pages ? total_pages - free_pages : 0u;

	off = strapp(b, off, "# QaonicOS system monitor (Mach 3)\r\n");
	off = strapp(b, off, "uptime_ms ");
	off = u32app(b, off, ms);
	off = strapp(b, off, "\r\nuptime_s ");
	off = u32app(b, off, ms / 1000u);
	off = strapp(b, off, "\r\n");
	off = strapp(b, off, "mem_pages_used ");
	off = u32app(b, off, used_pages);
	off = strapp(b, off, "/");
	off = u32app(b, off, total_pages);
	off = strapp(b, off, "\r\nmem_used_kb ");
	off = u32app(b, off, used_pages * 4u);
	off = strapp(b, off, "\r\nmem_total_mb ");
	off = u32app(b, off, GUEST_RAM_MB);
	off = strapp(b, off, "\r\n");
	off = strapp(b, off, "blk_total_sectors ");
	off = u32app(b, off, blk_total_sectors());
	off = strapp(b, off, "\r\nblk_sd_sectors ");
	off = u32app(b, off, blk_nsectors_dev(1));
	off = strapp(b, off, "\r\n");
	off = strapp(b, off, "net_rx_bytes ");
	off = u64app(b, off, net_rx_bytes_get());
	off = strapp(b, off, "\r\nnet_tx_bytes ");
	off = u64app(b, off, net_tx_bytes_get());
	off = strapp(b, off, "\r\n");
	off = strapp(b, off, "tcp_rx_segs ");
	off = u32app(b, off, tcp_rx_segs());
	off = strapp(b, off, "\r\ntcp_tx_segs ");
	off = u32app(b, off, tcp_tx_segs());
	off = strapp(b, off, "\r\ntcp_conns ");
	off = u32app(b, off, tcp_conns());
	off = strapp(b, off, "\r\n");
	off = strapp(b, off, "net_rx_frames ");
	off = u32app(b, off, netstack_rx_frames());
	off = strapp(b, off, "\r\nnet_arp_hits ");
	off = u32app(b, off, netstack_arp_hits());
	off = strapp(b, off, "\r\nnet_ping_replies ");
	off = u32app(b, off, netstack_ping_replies());
	off = strapp(b, off, "\r\n");
	return off;
}

/* --- body / : dashboard HTML --- */
static unsigned
build_dashboard(unsigned char *b)
{
	unsigned off = 0;
	unsigned int ms = arm_timer_ticks() * 10u;
	const unsigned char *mac = net_mac();
	unsigned int total_pages = (unsigned int)(mem_size / 4096u);
	unsigned int free_pages = (unsigned int)vm_page_free_count;
	unsigned int used_pages =
	    total_pages > free_pages ? total_pages - free_pages : 0u;
	unsigned int mem_kb = used_pages * 4u;
	unsigned int mem_tenth_pct =
	    mem_kb * 1000u / (GUEST_RAM_MB * 1024u);
	unsigned int btotal = blk_total_sectors();
	unsigned int sdtotal = blk_nsectors_dev(1);
	unsigned int dn10, up10;

	bw_update(&dn10, &up10);

	off = strapp(b, off,
	    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	    "<meta http-equiv=\"refresh\" content=\"5\">"
	    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	    "<title>QaonicOS System Monitor</title><style>"
	    "body{background:#0b0e14;color:#c9d4e3;font-family:monospace;"
	    "margin:0;padding:16px}"
	    "h1{color:#6ea8fe;font-size:22px}h2{color:#8fa3c0;font-size:16px}"
	    "a{color:#6ea8fe}"
	    ".cards{display:flex;flex-wrap:wrap;gap:10px;margin:12px 0}"
	    ".card{background:#131926;border:1px solid #263049;border-radius:8px;"
	    "padding:10px 14px;min-width:170px}"
	    ".card b{display:block;color:#8fa3c0;font-size:12px;font-weight:normal}"
	    ".card .v{font-size:20px;color:#fff}"
	    ".bar{height:8px;background:#263049;border-radius:4px;margin-top:6px}"
	    ".bar i{display:block;height:8px;background:#3fb950;border-radius:4px}"
	    ".emutag{font-size:10px;color:#0b0e14;background:#39c5cf;"
	    "border-radius:4px;padding:1px 6px;margin-left:6px}"
	    "</style></head><body>"
	    "<h1>QaonicOS System Monitor"
	    "<span class=\"emutag\">Mach 3</span></h1>");

	off = strapp(b, off,
	    "<h2>Emulasi QEMU (hasil ukur)</h2>"
	    "<p>Semua angka adalah HASIL UKUR dari hardware virtual QEMU "
	    "(Cortex-A7, RAM 64MB) — bukan simulasi.</p>"
	    "<div class=\"cards\">");

	/* Memory (real: page allocator vs 64MB) */
	off = strapp(b, off,
	    "<div class=\"card\"><b>Memory<span class=\"emutag\">emulasi</span></b>"
	    "<span class=\"v\">");
	off = u32app(b, off, mem_kb);
	off = strapp(b, off, " KB / ");
	off = u32app(b, off, GUEST_RAM_MB);
	off = strapp(b, off, " MB</span><div class=\"bar\"><i style=\"width:");
	off = u32app(b, off, mem_tenth_pct / 10u);
	off = strapp(b, off, "%\"></i></div>");
	off = pct1app(b, off, mem_tenth_pct);
	off = strapp(b, off, "% dari 64MB DDR2 (QEMU -m 64)</div>");

	/* Storage (real: kapasitas device) */
	off = strapp(b, off,
	    "<div class=\"card\"><b>Storage<span class=\"emutag\">emulasi</span></b>"
	    "<span class=\"v\">");
	off = u32app(b, off, btotal / 2048u);
	off = strapp(b, off, " MB</span><br>internal (virtio-blk)<br>");
	if (sdtotal > 0u) {
		off = strapp(b, off, "<span class=\"v\">");
		off = u32app(b, off, sdtotal / 2048u);
		off = strapp(b, off, " MB</span><br>SD card (FAT32)</div>");
	} else {
		off = strapp(b, off, "SD: tak ada</div>");
	}

	/* Bandwidth (real: counter byte) */
	off = strapp(b, off,
	    "<div class=\"card\"><b>Bandwidth<span class=\"emutag\">emulasi"
	    "</span></b><span class=\"v\">&darr; ");
	off = rateapp(b, off, dn10);
	off = strapp(b, off, "</span><br>&uarr; ");
	off = rateapp(b, off, up10);
	off = strapp(b, off, "<br>virtio-net</div>");

	/* Uptime */
	off = strapp(b, off,
	    "<div class=\"card\"><b>Uptime</b><span class=\"v\">");
	off = u32app(b, off, ms / 1000u);
	off = strapp(b, off, " dtk</span><br>");
	off = u32app(b, off, ms);
	off = strapp(b, off, " ms</div>");

	/* Network */
	off = strapp(b, off,
	    "<div class=\"card\"><b>Network</b><span class=\"v\">");
	off = hexapp(b, off, mac[0]);
	off = strapp(b, off, ":");
	off = hexapp(b, off, mac[1]);
	off = strapp(b, off, ":");
	off = hexapp(b, off, mac[2]);
	off = strapp(b, off, ":");
	off = hexapp(b, off, mac[3]);
	off = strapp(b, off, ":");
	off = hexapp(b, off, mac[4]);
	off = strapp(b, off, ":");
	off = hexapp(b, off, mac[5]);
	off = strapp(b, off, "</span><br>10.0.2.15 (QEMU user-net)<br>TCP: ");
	off = u32app(b, off, tcp_conns());
	off = strapp(b, off, " conn, ");
	off = u32app(b, off, tcp_rx_segs());
	off = strapp(b, off, "/");
	off = u32app(b, off, tcp_tx_segs());
	off = strapp(b, off, " segs rx/tx</div>");

	off = strapp(b, off,
	    "</div><p><a href=\"/metrics\">/metrics</a> (text/plain)</p>"
	    "</body></html>");
	return off;
}

/* --- ONB-1: web settings UI (login/settings/shell) --- */
#include "webui.h"
#include "credstore.h"

extern unsigned int	bootstate_gui_mode(void);
extern void	bootstate_set_gui_mode(unsigned int on);

static int
pseq(const char *a, const char *b, unsigned int n)
{
	unsigned int i;
	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static int
req_has(const unsigned char *req, unsigned reqlen, const char *needle)
{
	unsigned i, nl = 0;
	while (needle[nl])
		nl++;
	if (reqlen < nl)
		return 0;
	for (i = 0; i + nl <= reqlen; i++) {
		unsigned j;
		int ok = 1;
		for (j = 0; j < nl; j++)
			if (req[i + j] != (unsigned char)needle[j])
				ok = 0;
		if (ok)
			return (int)i;
	}
	return -1;
}

static const unsigned char *
body_of(const unsigned char *req, unsigned reqlen, unsigned *blen)
{
	unsigned i;
	for (i = 0; i + 3u < reqlen; i++)
		if (req[i] == '\r' && req[i + 1] == '\n' &&
		    req[i + 2] == '\r' && req[i + 3] == '\n') {
			*blen = reqlen - (i + 4u);
			return req + i + 4u;
		}
	*blen = 0;
	return req + reqlen;
}

static int
hexv(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* urlencoded form field -> decoded out. 1 found, 0 absent. */
static int
form_get(const unsigned char *body, unsigned blen, const char *key,
    char *out, unsigned max)
{
	unsigned i = 0, kl = 0, o = 0;
	while (key[kl])
		kl++;
	while (i < blen) {
		unsigned j = 0;
		while (i + j < blen && body[i + j] != '=' &&
		       body[i + j] != '&')
			j++;
		if (j == kl) {
			unsigned m;
			int ok = 1;
			for (m = 0; m < kl; m++)
				if (body[i + m] != (unsigned char)key[m])
					ok = 0;
			if (ok) {
				i += j + 1u;
				while (i < blen && body[i] != '&') {
					unsigned char c = body[i];
					if (c == '+') {
						c = ' ';
						i++;
					} else if (c == '%' &&
					    i + 2u < blen &&
					    hexv(body[i + 1u]) >= 0 &&
					    hexv(body[i + 2u]) >= 0) {
						c = (unsigned char)(
						    hexv(body[i + 1u]) * 16 +
						    hexv(body[i + 2u]));
						i += 3u;
					} else {
						i++;
					}
					if (o + 1u < max)
						out[o++] = (char)c;
				}
				out[o] = 0;
				return 1;
			}
		}
		while (i < blen && body[i] != '&')
			i++;
		if (i < blen)
			i++;
	}
	return 0;
}

static int
cookie_get(const unsigned char *req, unsigned reqlen, const char *name,
    char *out, unsigned max)
{
	int at = req_has(req, reqlen, name);
	unsigned i, o = 0;
	if (at < 0)
		return 0;
	i = (unsigned)at;
	while (name[o])
		i++, o++;
	o = 0;
	while (i < reqlen && req[i] != ';' && req[i] != '\r' &&
	       req[i] != '\n' && req[i] != ' ') {
		if (o + 1u < max)
			out[o++] = (char)req[i];
		i++;
	}
	out[o] = 0;
	return o > 0u;
}

static int
sess_ok(const unsigned char *req, unsigned reqlen)
{
	char tok[40];
	if (!cookie_get(req, reqlen, "qaon_sess=", tok, sizeof(tok)))
		return 0;
	return webui_session_check(tok, 32u);
}

static unsigned
escapp(unsigned char *d, unsigned off, const char *s)
{
	while (*s) {
		if (*s == '&')
			off = strapp(d, off, "&amp;");
		else if (*s == '<')
			off = strapp(d, off, "&lt;");
		else if (*s == '>')
			off = strapp(d, off, "&gt;");
		else if (*s == '"')
			off = strapp(d, off, "&quot;");
		else
			d[off++] = (unsigned char)*s;
		s++;
	}
	return off;
}

#define UI_STYLE \
	"<style>body{background:#0b0e14;color:#c9d4e3;" \
	"font-family:monospace;margin:0;padding:16px}" \
	"h1{color:#6ea8fe;font-size:20px}label{display:block;" \
	"margin:10px 0 4px;color:#8fa3c0}" \
	"input,select{background:#131926;color:#fff;border:1px solid #263049;" \
	"border-radius:6px;padding:8px;width:280px;max-width:90%}" \
	"input[type=checkbox]{width:auto}" \
	"button{background:#1f6feb;color:#fff;border:0;border-radius:6px;" \
	"padding:9px 18px;margin-top:14px;cursor:pointer}" \
	".err{color:#f47067}.ok{color:#3fb950}pre{background:#131926;" \
	"padding:10px;border-radius:6px;white-space:pre-wrap}</style>"

static unsigned
build_login(unsigned char *b, const char *err)
{
	unsigned off = 0;
	int has_pin = webui_pin_present();

	off = strapp(b, off,
	    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	    "<meta name=\"viewport\" content=\"width=device-width,"
	    "initial-scale=1\"><title>QaonicOS Login</title>"
	    UI_STYLE "</head><body>");
	off = strapp(b, off, has_pin ? "<h1>QaonicOS &mdash; Login</h1>"
	    : "<h1>QaonicOS &mdash; Buat PIN</h1>");
	if (err) {
		off = strapp(b, off, "<p class=\"err\">");
		off = strapp(b, off, err);
		off = strapp(b, off, "</p>");
	}
	if (!has_pin)
		off = strapp(b, off,
		    "<p>Device ini belum punya PIN. Buat PIN (4-16 "
		    "karakter) untuk mengunci Settings dan shell.</p>");
	off = strapp(b, off,
	    "<form method=\"POST\" action=\"/login\">"
	    "<label>PIN</label>"
	    "<input type=\"password\" name=\"pin\" inputmode=\"numeric\" "
	    "autocomplete=\"off\" autofocus>"
	    "<br><button type=\"submit\">");
	off = strapp(b, off, has_pin ? "Masuk" : "Buat PIN &amp; masuk");
	off = strapp(b, off, "</button></form></body></html>");
	return off;
}

static unsigned
build_settings(unsigned char *b, const char *msg)
{
	unsigned off = 0;
	char name[48], tz[24];
	int has_key, shell_on;
	unsigned int gui;

	name[0] = 0;
	tz[0] = 0;
	(void)webui_cfg_read("cfg-name", name, sizeof(name));
	(void)webui_cfg_read("cfg-tz", tz, sizeof(tz));
	has_key = credstore_present("apikey");	/* B5.5: no plaintext read */
	shell_on = webui_shell_enabled();
	gui = bootstate_gui_mode();

	off = strapp(b, off,
	    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	    "<meta name=\"viewport\" content=\"width=device-width,"
	    "initial-scale=1\"><title>QaonicOS Settings</title>"
	    UI_STYLE "</head><body><h1>QaonicOS Settings</h1>");
	if (msg) {
		off = strapp(b, off, "<p class=\"ok\">");
		off = strapp(b, off, msg);
		off = strapp(b, off, "</p>");
	}
	off = strapp(b, off,
	    "<form method=\"POST\" action=\"/settings\">"
	    "<label>Nama device</label><input name=\"name\" value=\"");
	off = escapp(b, off, name);
	off = strapp(b, off,
	    "\"><label>Zona waktu</label><input name=\"tz\" value=\"");
	off = escapp(b, off, tz);
	off = strapp(b, off, "\"><label>API key LLM ");
	off = strapp(b, off, has_key ? "(sudah terisi)" : "(kosong &mdash; bot offline)");
	off = strapp(b, off,
	    "</label><input type=\"password\" name=\"apikey\" "
	    "autocomplete=\"off\" placeholder=\"isi untuk mengganti\">"
	    "<label><input type=\"checkbox\" name=\"clear_apikey\"> "
	    "Hapus API key</label>"
	    "<label>Mode boot</label><select name=\"mode\">"
	    "<option value=\"dev\"");
	off = strapp(b, off, gui ? "" : " selected");
	off = strapp(b, off, ">dev (shell dulu)</option>"
	    "<option value=\"user\"");
	off = strapp(b, off, gui ? " selected" : "");
	off = strapp(b, off,
	    ">user (GUI langsung)</option></select>"
	    "<label><input type=\"checkbox\" name=\"shell_on\"");
	off = strapp(b, off, shell_on ? " checked" : "");
	off = strapp(b, off,
	    "> Web shell aktif (default mati; mati sendiri setelah "
	    "15 menit nganggur)</label>"
	    "<br><button type=\"submit\">Simpan</button></form>"
	    "<p><a href=\"/shell\">Web shell</a> &middot; "
	    "<a href=\"/\">Monitor</a></p></body></html>");
	return off;
}

static char	ws_last_out[4096];
static int	ws_pending;

static unsigned
build_shell(unsigned char *b, const char *msg)
{
	unsigned off = 0;
	char tmp[4096];

	if (ws_pending &&
	    webui_shell_result(tmp, sizeof(tmp)) >= 0) {
		unsigned i;
		for (i = 0; i < sizeof(ws_last_out); i++)
			ws_last_out[i] = tmp[i];
		ws_pending = 0;
	}
	off = strapp(b, off,
	    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	    "<meta name=\"viewport\" content=\"width=device-width,"
	    "initial-scale=1\"><title>QaonicOS Shell</title>");
	if (ws_pending)
		off = strapp(b, off,
		    "<meta http-equiv=\"refresh\" content=\"2\">");
	off = strapp(b, off, UI_STYLE
	    "</head><body><h1>QaonicOS Web Shell</h1>");
	if (msg) {
		off = strapp(b, off, "<p>");
		off = strapp(b, off, msg);
		off = strapp(b, off, "</p>");
	}
	off = strapp(b, off,
	    "<form method=\"POST\" action=\"/shell\">"
	    "<input name=\"cmd\" placeholder=\"perintah shell\" "
	    "autocomplete=\"off\" autofocus>"
	    "<br><button type=\"submit\">Jalankan</button></form>");
	if (ws_pending)
		off = strapp(b, off,
		    "<p>Perintah sedang dijalankan&hellip;</p>");
	else if (ws_last_out[0]) {
		off = strapp(b, off, "<pre>");
		off = escapp(b, off, ws_last_out);
		off = strapp(b, off, "</pre>");
	}
	off = strapp(b, off,
	    "<p><a href=\"/settings\">Settings</a></p></body></html>");
	return off;
}

static unsigned
build_note(unsigned char *b, const char *title, const char *text)
{
	unsigned off = 0;
	off = strapp(b, off,
	    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	    "<meta name=\"viewport\" content=\"width=device-width,"
	    "initial-scale=1\"><title>QaonicOS</title>"
	    UI_STYLE "</head><body><h1>");
	off = strapp(b, off, title);
	off = strapp(b, off, "</h1><p>");
	off = strapp(b, off, text);
	off = strapp(b, off, "</p></body></html>");
	return off;
}

/* --- HTTP/1.0 request handler --- */
unsigned
http_handle(const unsigned char *req, unsigned reqlen, unsigned char *resp)
{
	unsigned off = 0;
	unsigned body_len = 0;
	static unsigned char body[8192];
	const char *ctype = "text/html";
	unsigned int code = 200u;
	const char *codestr = "OK";
	char extra[192];
	char path[24];
	unsigned pi = 0, pstart;
	int is_post = 0;
	const unsigned char *fbody;
	unsigned flen;

	extra[0] = 0;
	fbody = body_of(req, reqlen, &flen);

	/* Method + path. */
	if (reqlen >= 4u && req[0] == 'G' && req[1] == 'E' &&
	    req[2] == 'T' && req[3] == ' ') {
		pstart = 4u;
	} else if (reqlen >= 5u && req[0] == 'P' && req[1] == 'O' &&
	    req[2] == 'S' && req[3] == 'T' && req[4] == ' ') {
		is_post = 1;
		pstart = 5u;
	} else {
		code = 400u;
		codestr = "Bad Request";
		ctype = "text/plain";
		goto respond;
	}
	while (pstart + pi < reqlen && req[pstart + pi] != ' ' &&
	       req[pstart + pi] != '?') {
		if (pi + 1u < sizeof(path))
			path[pi] = (char)req[pstart + pi];
		pi++;
	}
	path[pi < sizeof(path) ? pi : sizeof(path) - 1u] = 0;

#define PATH_IS(s) (pi == sizeof(s) - 1u && \
    pseq(path, s, sizeof(s) - 1u))

	if (!is_post && PATH_IS("/")) {
		body_len = build_dashboard(body);
	} else if (!is_post && PATH_IS("/metrics")) {
		ctype = "text/plain";
		body_len = build_metrics(body);
	} else if (!is_post && PATH_IS("/login")) {
		body_len = build_login(body, 0);
	} else if (is_post && PATH_IS("/login")) {
		char pin[24];
		int vr = 0;	/* B5.5: verify tri-state */
		if (form_get(fbody, flen, "pin", pin, sizeof(pin))) {
			unsigned pl = 0;
			while (pin[pl])
				pl++;
			if (!webui_pin_present())
				vr = webui_pin_set(pin, pl) == 0 &&
				    webui_pin_verify(pin, pl) == 1;
			else
				vr = webui_pin_verify(pin, pl);
		}
		if (vr == 1) {
			char tok[40];
			if (webui_session_login(tok) == 0) {
				code = 303u;
				codestr = "See Other";
				{
					unsigned e = 0;
					e = strapp((unsigned char *)extra,
					    e, "Location: /settings\r\n"
					    "Set-Cookie: qaon_sess=");
					e = strapp((unsigned char *)extra,
					    e, tok);
					e = strapp((unsigned char *)extra,
					    e, "; HttpOnly; Path=/\r\n");
					(void)e;
				}
				body_len = 0;
			} else {
				body_len = build_login(body,
				    "Gagal membuat sesi.");
			}
		} else if (vr == -1) {
			/* B5.5: locked out -- say for how long. */
			static char lmsg[96];
			unsigned r = webui_pin_lock_remaining_s();
			unsigned e = 0, d = 1, t = r;
			const char *p =
			    "Terlalu banyak percobaan. Terkunci ";
			while (*p)
				lmsg[e++] = *p++;
			while (t / 10u >= d && d <= 100u)
				d *= 10u;
			while (d) {
				lmsg[e++] = (char)('0' + (r / d) % 10u);
				d /= 10u;
			}
			p = " detik.";
			while (*p)
				lmsg[e++] = *p++;
			lmsg[e] = 0;
			body_len = build_login(body, lmsg);
		} else {
			body_len = build_login(body,
			    "PIN salah atau tidak valid (4-16 karakter).");
		}
	} else if (!is_post && PATH_IS("/settings")) {
		if (!sess_ok(req, reqlen)) {
			code = 303u;
			codestr = "See Other";
			(void)strapp((unsigned char *)extra, 0,
			    "Location: /login\r\n");
			body_len = 0;
		} else {
			body_len = build_settings(body, 0);
		}
	} else if (is_post && PATH_IS("/settings")) {
		if (!sess_ok(req, reqlen)) {
			code = 303u;
			codestr = "See Other";
			(void)strapp((unsigned char *)extra, 0,
			    "Location: /login\r\n");
			body_len = 0;
		} else {
			char v[64], dummy[8];
			static char kv[520];	/* B5.5: API keys can be long */
			if (form_get(fbody, flen, "name", v, 40u)) {
				unsigned n = 0;
				while (v[n])
					n++;
				(void)webui_cfg_write("cfg-name", v, n);
			}
			if (form_get(fbody, flen, "tz", v, 16u)) {
				unsigned n = 0;
				while (v[n])
					n++;
				(void)webui_cfg_write("cfg-tz", v, n);
			}
			if (form_get(fbody, flen, "clear_apikey",
			    dummy, sizeof(dummy))) {
				(void)credstore_clear("apikey");
			} else if (form_get(fbody, flen, "apikey",
			    kv, sizeof(kv)) && kv[0]) {
				unsigned n = 0;
				while (kv[n])
					n++;
				(void)credstore_set("apikey",
				    (const unsigned char *)kv, n);
			}
			if (form_get(fbody, flen, "mode", v, sizeof(v)))
				bootstate_set_gui_mode(
				    v[0] == 'u' ? 1u : 0u);
			webui_shell_set_enabled(form_get(fbody, flen,
			    "shell_on", dummy, sizeof(dummy)));
			body_len = build_settings(body, "Tersimpan.");
		}
	} else if (!is_post && PATH_IS("/shell")) {
		if (!sess_ok(req, reqlen)) {
			code = 303u;
			codestr = "See Other";
			(void)strapp((unsigned char *)extra, 0,
			    "Location: /login\r\n");
			body_len = 0;
		} else if (!webui_shell_enabled()) {
			body_len = build_note(body, "Web shell mati",
			    "Web shell nonaktif (default). Aktifkan dari "
			    "<a href=\"/settings\">Settings</a> setelah "
			    "login.");
		} else {
			body_len = build_shell(body, 0);
		}
	} else if (is_post && PATH_IS("/shell")) {
		if (!sess_ok(req, reqlen)) {
			code = 303u;
			codestr = "See Other";
			(void)strapp((unsigned char *)extra, 0,
			    "Location: /login\r\n");
			body_len = 0;
		} else if (!webui_shell_enabled()) {
			body_len = build_note(body, "Web shell mati",
			    "Web shell nonaktif. Aktifkan dari Settings.");
		} else {
			char cmd[176];
			if (form_get(fbody, flen, "cmd", cmd,
			    sizeof(cmd)) && cmd[0]) {
				unsigned n = 0;
				while (cmd[n])
					n++;
				if (webui_shell_submit(cmd, n) == 0) {
					ws_pending = 1;
					ws_last_out[0] = 0;
					body_len = build_shell(body, 0);
				} else {
					body_len = build_shell(body,
					    "Shell sibuk — coba lagi.");
				}
			} else {
				body_len = build_shell(body, 0);
			}
		}
	} else {
		code = 404u;
		codestr = "Not Found";
		ctype = "text/plain";
		body_len = 0;
	}

respond:
	off = strapp(resp, off, "HTTP/1.0 ");
	off = u32app(resp, off, code);
	off = strapp(resp, off, " ");
	off = strapp(resp, off, codestr);
	off = strapp(resp, off, "\r\nContent-Type: ");
	off = strapp(resp, off, ctype);
	off = strapp(resp, off, "\r\n");
	if (extra[0])
		off = strapp(resp, off, extra);
	off = strapp(resp, off, "Content-Length: ");
	off = u32app(resp, off, body_len);
	off = strapp(resp, off, "\r\nConnection: close\r\n\r\n");
	{
		unsigned i;
		for (i = 0u; i < body_len; i++)
			resp[off++] = body[i];
	}
	return off;
}
