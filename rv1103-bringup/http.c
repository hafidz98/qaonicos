/*
 * http.c - Server HTTP/1.0 minimal untuk system monitor, Fase 12/12b.
 *
 * Routes:
 *   GET /        -> dashboard HTML "QaonicOS System Monitor" (nilai live,
 *                   di-generate per request; auto-refresh 5 detik)
 *   GET /metrics -> text/plain: uptime, daftar thread, info memori
 *   lainnya     -> 404
 */
#include "http.h"
#include "sched.h"
#include "vm.h"
#include "lib.h"
#include "tcp.h"
#include "net.h"

/* --- util string --- */
static unsigned strapp(uint8_t *d, unsigned off, const char *s)
{
    while (*s)
        d[off++] = (uint8_t)*s++;
    return off;
}

static unsigned u32app(uint8_t *d, unsigned off, uint32_t v)
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
        d[off++] = (uint8_t)tmp[i];
    return off;
}

static const char *thread_state_name(unsigned st)
{
    if (st == THREAD_RUNNABLE)
        return "runnable";
    if (st == THREAD_BLOCKED)
        return "blocked";
    return "dead";
}

static const char *thread_state_cls(unsigned st)
{
    if (st == THREAD_RUNNABLE)
        return "run";
    if (st == THREAD_BLOCKED)
        return "blk";
    return "ded";
}

static unsigned hexapp(uint8_t *d, unsigned off, uint8_t v)
{
    static const char *hx = "0123456789abcdef";
    d[off++] = (uint8_t)hx[v >> 4];
    d[off++] = (uint8_t)hx[v & 15u];
    return off;
}

/* --- simulasi spek Luckfox Pico Mini (Fase 12c) ---
 * LCG sederhana + random walk. State persisten antar request sehingga
 * nilai BERUBAH tiap halaman di-refresh. Semua nilai di sini SIMULASI
 * dari spek asli (RV1103 Cortex-A7 @1.2GHz, 64MB DDR2, 128MB SPI NAND),
 * bukan pengukuran nyata. Tanpa rand() libc.
 */
static uint32_t sim_state = 0u;
static int sim_cpu = 23;   /* persen, walk 4..87 */
static int sim_mem = 31;   /* MB terpakai dari 64MB DDR2, walk 18..52 */
static int sim_stor = 68;  /* MB terpakai dari 128MB SPI NAND, walk 40..110 */
static int sim_up = 85;    /* 0.1 Mbps, walk 0.2..40.0 */
static int sim_dn = 142;   /* 0.1 Mbps, walk 0.2..40.0 */

static uint32_t sim_next(void)
{
    sim_state = sim_state * 1103515245u + 12345u;
    return (sim_state >> 16) & 0x7fffu;
}

static int sim_walk(int v, int lo, int hi, int step)
{
    int d = (int)(sim_next() % (uint32_t)(2 * step + 1)) - step;
    v += d;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

static void sim_tick(void)
{
    int k;
    if (sim_state == 0u) {
        sim_state = sched_ticks() | 0x9e3779b9u;
        if (sim_state == 0u)
            sim_state = 0x12345678u;
    }
    for (k = 0; k < 2; k++) {
        sim_cpu = sim_walk(sim_cpu, 4, 87, 6);
        sim_mem = sim_walk(sim_mem, 18, 52, 3);
        sim_stor = sim_walk(sim_stor, 40, 110, 4);
        sim_up = sim_walk(sim_up, 2, 400, 25);
        sim_dn = sim_walk(sim_dn, 2, 400, 30);
    }
}

/* Format persepuluh Mbps: 85 -> "8.5". */
static unsigned fix1app(uint8_t *d, unsigned off, int tenths)
{
    off = u32app(d, off, (uint32_t)(tenths / 10));
    off = strapp(d, off, ".");
    off = u32app(d, off, (uint32_t)(tenths % 10));
    return off;
}

/* --- body /metrics --- */
static unsigned build_metrics(uint8_t *b)
{
    unsigned off = 0, i, n;
    struct vm_stats vs;
    uint32_t ms = sched_ticks();

    off = strapp(b, off, "# QaonicOS system monitor\r\n");
    off = strapp(b, off, "uptime_ms ");
    off = u32app(b, off, ms);
    off = strapp(b, off, "\r\nuptime_s ");
    off = u32app(b, off, ms / 1000u);
    off = strapp(b, off, "\r\n");

    n = sched_thread_count();
    off = strapp(b, off, "threads ");
    off = u32app(b, off, n);
    off = strapp(b, off, "\r\n");
    for (i = 0; i < n; i++) {
        const struct sched_thread *t = sched_thread_at(i);
        if (!t)
            continue;
        off = strapp(b, off, "thread id=");
        off = u32app(b, off, (uint32_t)t->id);
        off = strapp(b, off, " state=");
        off = strapp(b, off, thread_state_name(t->state));
        off = strapp(b, off, " mode=");
        off = strapp(b, off, t->user_sp ? "user" : "kernel");
        off = strapp(b, off, "\r\n");
    }

    vm_get_stats(&vs);
    off = strapp(b, off, "mem_pages_used ");
    off = u32app(b, off, vs.pages_used);
    off = strapp(b, off, "/");
    off = u32app(b, off, vs.pages_total);
    off = strapp(b, off, "\r\nmem_l1_used ");
    off = u32app(b, off, vs.l1_used);
    off = strapp(b, off, "/");
    off = u32app(b, off, vs.l1_total);
    off = strapp(b, off, "\r\nmem_l2_used ");
    off = u32app(b, off, vs.l2_used);
    off = strapp(b, off, "/");
    off = u32app(b, off, vs.l2_total);
    off = strapp(b, off, "\r\n");
    off = strapp(b, off, "tcp_rx_segs ");
    off = u32app(b, off, tcp_rx_segs());
    off = strapp(b, off, "\r\ntcp_tx_segs ");
    off = u32app(b, off, tcp_tx_segs());
    off = strapp(b, off, "\r\ntcp_conns ");
    off = u32app(b, off, tcp_conns());
    off = strapp(b, off, "\r\n");
    return off;
}

/* --- body / : dashboard HTML system monitor (nilai live per request) ---
 * Dua bagian: "Simulasi" (spek Luckfox Pico Mini, nilai simulasi berlabel
 * jelas) dan "Data kernel (real)" (data nyata dari kernel).
 */
static unsigned build_dashboard(uint8_t *b)
{
    unsigned off = 0, i, n, nr = 0, nb = 0, nd = 0, pct;
    unsigned mempct, stopct;
    struct vm_stats vs;
    uint32_t ms = sched_ticks();
    const uint8_t *mac = net_mac();

    sim_tick(); /* majukan random walk simulasi tiap request */

    n = sched_thread_count();
    for (i = 0; i < n; i++) {
        const struct sched_thread *t = sched_thread_at(i);
        unsigned st = t ? t->state : 99u;
        if (st == THREAD_RUNNABLE)
            nr++;
        else if (st == THREAD_BLOCKED)
            nb++;
        else
            nd++;
    }
    vm_get_stats(&vs);
    pct = vs.pages_total ? (vs.pages_used * 100u / vs.pages_total) : 0;
    mempct = (unsigned)sim_mem * 100u / 64u;
    stopct = (unsigned)sim_stor * 100u / 128u;

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
        ".simtag{font-size:10px;color:#0b0e14;background:#d29922;"
        "border-radius:4px;padding:1px 6px;margin-left:6px}"
        "table{border-collapse:collapse;margin-top:8px}"
        "th,td{border:1px solid #263049;padding:3px 10px;text-align:left}"
        "th{color:#8fa3c0;font-weight:normal}"
        ".run{color:#3fb950}.blk{color:#d29922}.ded{color:#6e7681}"
        ".note{color:#8fa3c0;font-size:12px;max-width:680px}"
        "</style></head><body>"
        "<h1>QaonicOS System Monitor</h1>"
        "<h2>Simulasi - spek Luckfox Pico Mini</h2>"
        "<p class=\"note\">Nilai di bagian ini adalah SIMULASI dari spek asli "
        "Luckfox Pico Mini (RV1103 Cortex-A7 @1.2GHz, 64MB DDR2, 128MB SPI "
        "NAND Flash), bukan pengukuran nyata.</p>"
        "<div class=\"cards\">");

    /* Kartu CPU % (simulasi) */
    off = strapp(b, off,
        "<div class=\"card\"><b>CPU<span class=\"simtag\">simulasi</span></b>"
        "<span class=\"v\">");
    off = u32app(b, off, (uint32_t)sim_cpu);
    off = strapp(b, off, "%</span><div class=\"bar\"><i style=\"width:");
    off = u32app(b, off, (uint32_t)sim_cpu);
    off = strapp(b, off, "%\"></i></div>RV1103 @1.2GHz</div>");

    /* Kartu Memory % + MB (simulasi, 64MB DDR2) */
    off = strapp(b, off,
        "<div class=\"card\"><b>Memory<span class=\"simtag\">simulasi</span></b>"
        "<span class=\"v\">");
    off = u32app(b, off, (uint32_t)sim_mem);
    off = strapp(b, off, "/64 MB</span><div class=\"bar\"><i style=\"width:");
    off = u32app(b, off, mempct);
    off = strapp(b, off, "%\"></i></div>");
    off = u32app(b, off, mempct);
    off = strapp(b, off, "% dari 64MB DDR2</div>");

    /* Kartu Storage % + MB (simulasi, 128MB SPI NAND) */
    off = strapp(b, off,
        "<div class=\"card\"><b>Storage<span class=\"simtag\">simulasi</span></b>"
        "<span class=\"v\">");
    off = u32app(b, off, (uint32_t)sim_stor);
    off = strapp(b, off, "/128 MB</span><div class=\"bar\"><i style=\"width:");
    off = u32app(b, off, stopct);
    off = strapp(b, off, "%\"></i></div>");
    off = u32app(b, off, stopct);
    off = strapp(b, off, "% dari 128MB SPI NAND</div>");

    /* Kartu Bandwidth up/down (simulasi, link USB 2.0) */
    off = strapp(b, off,
        "<div class=\"card\"><b>Bandwidth<span class=\"simtag\">simulasi</span></b>"
        "<span class=\"v\">&darr; ");
    off = fix1app(b, off, sim_dn);
    off = strapp(b, off, " Mbps</span><br>&uarr; ");
    off = fix1app(b, off, sim_up);
    off = strapp(b, off, " Mbps<br>link USB 2.0</div>");

    /* --- Bagian data kernel (real) --- */
    off = strapp(b, off, "</div><h2>Data kernel (real)</h2><div class=\"cards\">");

    /* Kartu Uptime */
    off = strapp(b, off,
        "<div class=\"card\"><b>Uptime</b><span class=\"v\">");
    off = u32app(b, off, ms / 1000u);
    off = strapp(b, off, " dtk</span><br>");
    off = u32app(b, off, ms);
    off = strapp(b, off, " ms</div>");

    /* Kartu aktivitas thread (data kernel nyata) */
    off = strapp(b, off,
        "<div class=\"card\"><b>Aktivitas thread</b>"
        "<span class=\"v\">");
    off = u32app(b, off, nr);
    off = strapp(b, off, " runnable</span><br>");
    off = u32app(b, off, nb);
    off = strapp(b, off, " blocked, ");
    off = u32app(b, off, nd);
    off = strapp(b, off, " dead</div>");

    /* Kartu Memory kernel */
    off = strapp(b, off,
        "<div class=\"card\"><b>Memory kernel</b><span class=\"v\">");
    off = u32app(b, off, vs.pages_used);
    off = strapp(b, off, "/");
    off = u32app(b, off, vs.pages_total);
    off = strapp(b, off,
        " pages</span><div class=\"bar\"><i style=\"width:");
    off = u32app(b, off, pct);
    off = strapp(b, off, "%\"></i></div>L1 ");
    off = u32app(b, off, vs.l1_used);
    off = strapp(b, off, "/");
    off = u32app(b, off, vs.l1_total);
    off = strapp(b, off, ", L2 ");
    off = u32app(b, off, vs.l2_used);
    off = strapp(b, off, "/");
    off = u32app(b, off, vs.l2_total);
    off = strapp(b, off, "</div>");

    /* Kartu Network kernel */
    off = strapp(b, off,
        "<div class=\"card\"><b>Network (virtio-net)</b>"
        "<span class=\"v\">10.0.2.15</span><br>MAC ");
    for (i = 0; i < 6; i++) {
        off = hexapp(b, off, mac[i]);
        if (i < 5)
            off = strapp(b, off, ":");
    }
    off = strapp(b, off, "<br>rx ");
    off = u32app(b, off, tcp_rx_segs());
    off = strapp(b, off, " segs, tx ");
    off = u32app(b, off, tcp_tx_segs());
    off = strapp(b, off, " segs<br>");
    off = u32app(b, off, tcp_conns());
    off = strapp(b, off, " koneksi</div>");

    /* Tabel thread */
    off = strapp(b, off, "</div><h2>Threads (");
    off = u32app(b, off, n);
    off = strapp(b, off,
        ")</h2><table><tr><th>id</th><th>state</th><th>mode</th></tr>");
    for (i = 0; i < n; i++) {
        const struct sched_thread *t = sched_thread_at(i);
        if (!t)
            continue;
        off = strapp(b, off, "<tr><td>");
        off = u32app(b, off, (uint32_t)t->id);
        off = strapp(b, off, "</td><td class=\"");
        off = strapp(b, off, thread_state_cls(t->state));
        off = strapp(b, off, "\">");
        off = strapp(b, off, thread_state_name(t->state));
        off = strapp(b, off, "</td><td>");
        off = strapp(b, off, t->user_sp ? "user" : "kernel");
        off = strapp(b, off, "</td></tr>");
    }
    off = strapp(b, off,
        "</table><p class=\"note\">"
        "Bagian \"Simulasi\" memakai nilai acak realistis berbasis spek "
        "Luckfox Pico Mini dan selalu berlabel simulasi. Bagian "
        "\"Data kernel\" adalah data nyata dari kernel. "
        "Format teks: <a href=\"/metrics\">/metrics</a>. "
        "Halaman refresh otomatis tiap 5 detik."
        "</p></body></html>\r\n");
    return off;
}

/* Bangun respons lengkap. Mengembalikan panjang total. */
static unsigned respond(uint8_t *resp, int code, const char *reason,
                        const char *ctype, const uint8_t *body,
                        unsigned bodylen)
{
    unsigned off = 0, i;

    off = strapp(resp, off, "HTTP/1.0 ");
    off = u32app(resp, off, (uint32_t)code);
    off = strapp(resp, off, " ");
    off = strapp(resp, off, reason);
    off = strapp(resp, off, "\r\nContent-Type: ");
    off = strapp(resp, off, ctype);
    off = strapp(resp, off, "\r\nContent-Length: ");
    off = u32app(resp, off, bodylen);
    off = strapp(resp, off, "\r\nConnection: close\r\n\r\n");
    for (i = 0; i < bodylen; i++)
        resp[off++] = body[i];
    return off;
}

unsigned http_handle(const uint8_t *req, unsigned reqlen, uint8_t *resp)
{
    static uint8_t body[8192];
    unsigned bodylen, i;
    const uint8_t *path;
    unsigned pathlen = 0;

    if (reqlen < 5)
        return 0;
    /* Harus "GET ". */
    if (req[0] != 'G' || req[1] != 'E' || req[2] != 'T' || req[3] != ' ')
        return 0;
    /* Path = sampai spasi / CRLF berikutnya. */
    path = req + 4;
    for (i = 4; i < reqlen; i++) {
        if (req[i] == ' ' || req[i] == '\r' || req[i] == '\n')
            break;
        pathlen++;
    }
    if (pathlen == 0 || pathlen > 64)
        return 0;

    if (pathlen == 1 && path[0] == '/') {
        bodylen = build_dashboard(body);
        if (bodylen >= sizeof(body))
            bodylen = sizeof(body) - 1; /* pengaman: jangan baca lewat buffer */
        return respond(resp, 200, "OK", "text/html", body, bodylen);
    }
    if (pathlen == 8 && memcmp(path, "/metrics", 8) == 0) {
        bodylen = build_metrics(body);
        return respond(resp, 200, "OK", "text/plain", body, bodylen);
    }
    {
        static const uint8_t nf[] = "Not Found\n";
        return respond(resp, 404, "Not Found", "text/plain", nf,
                       sizeof(nf) - 1);
    }
}
