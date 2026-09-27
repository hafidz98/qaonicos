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
#include "blk.h"   /* Fase 12d: storage virtio-blk (emulasi SPI NAND) */

/* RAM guest di QEMU: -m 64 (spek Luckfox Pico Mini: 64MB DDR2).
 * Lihat run-qemu.sh. */
#define GUEST_RAM_MB 64u

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

/* Cetak uint64 desimal (compiler memakai __aeabi_uldivmod). */
static unsigned u64app(uint8_t *d, unsigned off, uint64_t v)
{
    char tmp[20];
    int n = 0, k;

    if (v == 0u)
        tmp[n++] = '0';
    else {
        while (v > 0u) {
            tmp[n++] = (char)('0' + v % 10u);
            v /= 10u;
        }
    }
    for (k = n - 1; k >= 0; k--)
        d[off++] = (uint8_t)tmp[k];
    return off;
}

static unsigned hexapp(uint8_t *d, unsigned off, uint8_t v)
{
    static const char *hx = "0123456789abcdef";
    d[off++] = (uint8_t)hx[v >> 4];
    d[off++] = (uint8_t)hx[v & 15u];
    return off;
}

/* --- pengukuran real dari emulasi QEMU (Fase 12d) ---
 * Menggantikan modul simulasi LCG Fase 12c. Semua angka di bawah ini
 * HASIL UKUR dari hardware virtual:
 *  - CPU %: idle-thread accounting (tick idle vs total per window)
 *  - Memory: page allocator kernel (used_pages*4KB) vs RAM 64MB (QEMU -m 64)
 *  - Storage: sektor yang ditulis kernel vs kapasitas REAL dari config
 *    device virtio-blk (128MB)
 *  - Bandwidth: counter byte RX/TX di net.c per window waktu
 */

/* CPU%: 100*(1 - idle/total) per window >=500ms. */
static unsigned cpu_pct_update(void)
{
    static unsigned last_t, last_idle, pct;
    unsigned t = sched_ticks();
    unsigned it = sched_idle_ticks();
    unsigned dt = t - last_t;

    if (dt >= 500u) {
        unsigned di = it - last_idle;
        pct = (di >= dt) ? 0u : (100u * (dt - di) / dt);
        last_t = t;
        last_idle = it;
    }
    return pct;
}

/* Bandwidth: persepuluh Kbps per window >=500ms, dari counter byte. */
static void bw_update(uint32_t *dn, uint32_t *up)
{
    static uint32_t last_t;
    static uint64_t last_rx, last_tx;
    static uint32_t dn10, up10;   /* persepuluh Kbps */
    unsigned t = sched_ticks();
    unsigned dt = t - last_t;

    if (dt >= 500u) {
        uint64_t rx = net_rx_bytes_get();
        uint64_t tx = net_tx_bytes_get();
        /* Delta per window tak mungkin >4GB: aman cast ke 32-bit.
         * persepuluh Kbps = byte*8*10/dt_ms = byte*80/dt_ms. */
        uint32_t drx = (uint32_t)(rx - last_rx);
        uint32_t dtx = (uint32_t)(tx - last_tx);
        dn10 = (drx * 80u) / dt;
        up10 = (dtx * 80u) / dt;
        last_t = t;
        last_rx = rx;
        last_tx = tx;
    }
    *dn = dn10;
    *up = up10;
}

/* Tampilkan laju (persepuluh Kbps): "N.N Kbps" atau "N.N Mbps". */
static unsigned rateapp(uint8_t *d, unsigned off, uint32_t tenth_kbps)
{
    if (tenth_kbps >= 10000u) {          /* >= 1000 Kbps -> Mbps */
        uint32_t tm = tenth_kbps / 1000u;
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

/* Persen satu desimal: 23 -> "2.3". Untuk persen kecil (memori). */
static unsigned pct1app(uint8_t *d, unsigned off, uint32_t tenth_pct)
{
    off = u32app(d, off, tenth_pct / 10u);
    off = strapp(d, off, ".");
    off = u32app(d, off, tenth_pct % 10u);
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
    /* Fase 12d: metrik real emulasi QEMU (key baru, format lama utuh). */
    off = strapp(b, off, "cpu_pct ");
    off = u32app(b, off, cpu_pct_update());
    off = strapp(b, off, "\r\nmem_used_kb ");
    off = u32app(b, off, vs.pages_used * 4u);
    off = strapp(b, off, "\r\nmem_total_mb ");
    off = u32app(b, off, GUEST_RAM_MB);
    off = strapp(b, off, "\r\nblk_total_sectors ");
    off = u32app(b, off, blk_total_sectors());
    off = strapp(b, off, "\r\nblk_used_sectors ");
    off = u32app(b, off, blk_used_sectors());
    off = strapp(b, off, "\r\nnet_rx_bytes ");
    off = u64app(b, off, net_rx_bytes_get());
    off = strapp(b, off, "\r\nnet_tx_bytes ");
    off = u64app(b, off, net_tx_bytes_get());
    off = strapp(b, off, "\r\n");
    return off;
}

/* --- body / : dashboard HTML system monitor (nilai live per request) ---
 * Dua bagian: "Emulasi QEMU" (HASIL UKUR dari hardware virtual yang
 * dikonfigurasi menyerupai Luckfox Pico Mini — Fase 12d, menggantikan
 * simulasi acak Fase 12c) dan "Data kernel (real)".
 */
static unsigned build_dashboard(uint8_t *b)
{
    unsigned off = 0, i, n, nr = 0, nb = 0, nd = 0, pct;
    struct vm_stats vs;
    uint32_t ms = sched_ticks();
    const uint8_t *mac = net_mac();
    unsigned cpu, dn10, up10;
    uint32_t mem_kb, mem_tenth_pct;
    uint32_t btotal, bused;

    cpu = cpu_pct_update();     /* real: idle-thread accounting */
    bw_update(&dn10, &up10);    /* real: counter byte RX/TX */

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

    /* Memory real: KB terpakai (page allocator) vs RAM 64MB. */
    mem_kb = vs.pages_used * 4u;    /* 4KB per halaman */
    mem_tenth_pct = mem_kb * 1000u / (GUEST_RAM_MB * 1024u);

    /* Storage real: sektor terpakai vs kapasitas device. */
    btotal = blk_total_sectors();
    bused = blk_used_sectors();

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
        "table{border-collapse:collapse;margin-top:8px}"
        "th,td{border:1px solid #263049;padding:3px 10px;text-align:left}"
        "th{color:#8fa3c0;font-weight:normal}"
        ".run{color:#3fb950}.blk{color:#d29922}.ded{color:#6e7681}"
        ".note{color:#8fa3c0;font-size:12px;max-width:680px}"
        "</style></head><body>"
        "<h1>QaonicOS System Monitor</h1>"
        "<h2>Emulasi QEMU - spek Luckfox Pico Mini</h2>"
        "<p class=\"note\">Semua angka di bagian ini adalah HASIL UKUR "
        "dari hardware virtual QEMU (Cortex-A7, RAM 64MB, storage 128MB "
        "virtio-blk) — bukan simulasi acak.</p>"
        "<div class=\"cards\">");

    /* Kartu CPU % (real: idle-thread accounting) */
    off = strapp(b, off,
        "<div class=\"card\"><b>CPU<span class=\"emutag\">emulasi</span></b>"
        "<span class=\"v\">");
    off = u32app(b, off, cpu);
    off = strapp(b, off, "%</span><div class=\"bar\"><i style=\"width:");
    off = u32app(b, off, cpu > 100u ? 100u : cpu);
    off = strapp(b, off, "%\"></i></div>idle-thread accounting</div>");

    /* Kartu Memory (real: page allocator vs 64MB) */
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

    /* Kartu Storage (real: sektor terpakai vs kapasitas device) */
    off = strapp(b, off,
        "<div class=\"card\"><b>Storage<span class=\"emutag\">emulasi</span></b>");
    if (btotal == 0u) {
        off = strapp(b, off,
            "<span class=\"v\">tak ada disk</span><br>virtio-blk tak "
            "terdeteksi</div>");
    } else {
        uint32_t tenth_mb = bused * 10u / 2048u;   /* sektor -> MB, 1 des */
        uint32_t tenth_pct = bused * 1000u / btotal;
        off = strapp(b, off, "<span class=\"v\">");
        off = pct1app(b, off, tenth_mb);
        off = strapp(b, off, "/");
        off = u32app(b, off, btotal / 2048u);
        off = strapp(b, off, " MB</span><div class=\"bar\"><i style=\"width:");
        off = u32app(b, off, tenth_pct / 10u);
        off = strapp(b, off, "%\"></i></div>");
        off = pct1app(b, off, tenth_pct);
        off = strapp(b, off, "% — virtio-blk (emulasi SPI NAND)</div>");
    }

    /* Kartu Bandwidth up/down (real: counter byte) */
    off = strapp(b, off,
        "<div class=\"card\"><b>Bandwidth<span class=\"emutag\">emulasi"
        "</span></b><span class=\"v\">&darr; ");
    off = rateapp(b, off, dn10);
    off = strapp(b, off, "</span><br>&uarr; ");
    off = rateapp(b, off, up10);
    off = strapp(b, off, "<br>virtio-net</div>");

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
        "Bagian \"Emulasi\" adalah hasil ukur nyata dari hardware virtual "
        "QEMU (bukan angka acak). Bagian "
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
        blk_log_request(sched_ticks(), 200u);  /* access log di disk */
        bodylen = build_dashboard(body);
        if (bodylen >= sizeof(body))
            bodylen = sizeof(body) - 1; /* pengaman: jangan baca lewat buffer */
        return respond(resp, 200, "OK", "text/html", body, bodylen);
    }
    if (pathlen == 8 && memcmp(path, "/metrics", 8) == 0) {
        blk_log_request(sched_ticks(), 200u);
        bodylen = build_metrics(body);
        return respond(resp, 200, "OK", "text/plain", body, bodylen);
    }
    {
        static const uint8_t nf[] = "Not Found\n";
        blk_log_request(sched_ticks(), 404u);
        return respond(resp, 404, "Not Found", "text/plain", nf,
                       sizeof(nf) - 1);
    }
}
