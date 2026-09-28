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

/* --- HTTP/1.0 request handler --- */
unsigned
http_handle(const unsigned char *req, unsigned reqlen, unsigned char *resp)
{
	unsigned off = 0;
	unsigned body_len;
	unsigned char body[8192];
	const char *ctype;
	unsigned int code;
	const char *codestr;

	/* Parse request line: "GET /path HTTP/1.x". */
	if (reqlen < 5u || req[0] != 'G' || req[1] != 'E' ||
	    req[2] != 'T' || req[3] != ' ') {
		code = 400u;
		codestr = "Bad Request";
		body_len = 0;
		ctype = "text/plain";
		goto respond;
	}

	if (req[4] == '/' &&
	    (reqlen == 5u || req[5] == ' ' || req[5] == '\r' ||
	     req[5] == '\n')) {
		/* GET / */
		code = 200u;
		codestr = "OK";
		ctype = "text/html";
		body_len = build_dashboard(body);
	} else if (reqlen >= 12u && req[4] == '/' && req[5] == 'm' &&
	           req[6] == 'e' && req[7] == 't' && req[8] == 'r' &&
	           req[9] == 'i' && req[10] == 'c' && req[11] == 's' &&
	           (reqlen == 12u || req[12] == ' ' || req[12] == '\r' ||
	            req[12] == '\n')) {
		/* GET /metrics */
		code = 200u;
		codestr = "OK";
		ctype = "text/plain";
		body_len = build_metrics(body);
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
	off = strapp(resp, off, "\r\nContent-Length: ");
	off = u32app(resp, off, body_len);
	off = strapp(resp, off, "\r\nConnection: close\r\n\r\n");
	{
		unsigned i;
		for (i = 0u; i < body_len; i++)
			resp[off++] = body[i];
	}
	return off;
}
