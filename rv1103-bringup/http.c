/*
 * http.c - Server HTTP/1.0 minimal untuk system monitor, Fase 12.
 *
 * Routes:
 *   GET /        -> halaman HTML sederhana (link ke /metrics)
 *   GET /metrics -> text/plain: uptime, daftar thread, info memori
 *   lainnya     -> 404
 */
#include "http.h"
#include "sched.h"
#include "vm.h"
#include "lib.h"
#include "tcp.h"

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

/* --- body / --- */
static unsigned build_index(uint8_t *b)
{
    unsigned off = 0;

    off = strapp(b, off,
        "<html><head><title>QaonicOS</title></head><body>"
        "<h1>QaonicOS</h1>"
        "<p>Nano OS berbasis Mach untuk Luckfox Pico.</p>"
        "<p><a href=\"/metrics\">System metrics</a></p>"
        "</body></html>\r\n");
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
    static uint8_t body[1100];
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
        bodylen = build_index(body);
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
