/*
 * ugpio.c - Utilitas GPIO userspace (Fase 14).
 *
 * Tanpa argv (shell paling akhir): "baris perintah" dibaca dari
 * /gpio.cmd yang ditulis init.c, satu perintah per baris:
 *   set <pin> <val>   -> SYS_GPIO_SET (val 0/1)
 *   get <pin>         -> SYS_GPIO_GET, hasilnya dicatat
 * Hasil tiap get ditulis ke /gpio.out sebagai "<pin>=<val>\n",
 * lalu sentinel /.ugpio_done. Pola koordinasi = ucat (Fase 10):
 * init menulis /gpio.cmd -> sentinel /.gpio_cmd_ready; ugpio menunggu
 * sentinel itu, mengeksekusi perintah, menulis hasil.
 *
 * Bare-metal, tanpa libc. Di-link di UGPIO_PROG_VA (0x10048000) via
 * ugpio.ld, di-embed sebagai blob -> ugpio_img.
 */
#include "ulib.h"

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
        p++;
    return p;
}

/* Parse desimal; *ok=0 bila tidak ada digit. */
static unsigned parse_u(const char **pp, const char *end, int *ok)
{
    const char *p = *pp;
    unsigned v = 0u;
    *ok = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned)(*p - '0');
        p++;
        *ok = 1;
    }
    *pp = p;
    return v;
}

static int starts(const char *p, const char *end, const char *w, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++)
        if (p + i >= end || p[i] != w[i])
            return 0;
    return 1;
}

/* Tulis desimal v ke buf; kembalikan jumlah karakter. */
static unsigned put_u(char *buf, unsigned v)
{
    char tmp[12];
    unsigned n = 0u, i;
    if (v == 0u) {
        buf[0] = '0';
        return 1u;
    }
    while (v > 0u) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (i = 0u; i < n; i++)
        buf[i] = tmp[n - 1u - i];
    return n;
}

__attribute__((section(".text.start")))
void _start(void)
{
    char cmd[512];
    char out[256];
    unsigned out_n = 0u;
    int fd, r, fails = 0;
    const char *p, *end, *line;

    if (!u_wait_file("/.gpio_cmd_ready")) {
        u_put("ugpio FAIL: /.gpio_cmd_ready timeout\n");
        u_exit();
    }

    fd = u_open("/gpio.cmd", O_RDONLY);
    if (fd < 0) {
        u_put("ugpio FAIL: open /gpio.cmd\n");
        u_exit();
    }
    r = u_read((unsigned)fd, cmd, sizeof(cmd) - 1u);
    u_close((unsigned)fd);
    if (r <= 0) {
        u_put("ugpio FAIL: baca /gpio.cmd\n");
        u_exit();
    }
    p = cmd;
    end = cmd + r;

    /* Eksekusi per baris. */
    while (p < end) {
        unsigned pin, val;
        int ok, gr;
        line = skip_ws(p, end);
        /* Maju p ke akhir baris untuk iterasi berikut. */
        p = line;
        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;  /* lewati '\n' */
        if (line >= end || *line == '\n')
            continue;

        if (starts(line, end, "set", 3u)) {
            const char *q = skip_ws(line + 3, end);
            pin = parse_u(&q, end, &ok);
            if (!ok) { fails++; continue; }
            q = skip_ws(q, end);
            val = parse_u(&q, end, &ok);
            if (!ok || val > 1u) { fails++; continue; }
            if (u_gpio_set(pin, val) != 0) {
                u_put("ugpio FAIL: gpio_set\n");
                fails++;
            }
        } else if (starts(line, end, "get", 3u)) {
            const char *q = skip_ws(line + 3, end);
            pin = parse_u(&q, end, &ok);
            if (!ok) { fails++; continue; }
            gr = u_gpio_get(pin);
            if (gr < 0) {
                u_put("ugpio FAIL: gpio_get\n");
                fails++;
                continue;
            }
            /* Catat "<pin>=<val>\n". */
            out_n += put_u(out + out_n, pin);
            out[out_n++] = '=';
            out_n += put_u(out + out_n, (unsigned)gr);
            out[out_n++] = '\n';
        } else {
            u_put("ugpio FAIL: perintah tak dikenal\n");
            fails++;
        }
    }

    if (fails != 0) {
        u_put("ugpio FAIL: eksekusi perintah\n");
        u_exit();
    }

    /* Tulis hasil. */
    fd = u_open("/gpio.out", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("ugpio FAIL: open /gpio.out\n");
        u_exit();
    }
    if (out_n > 0u) {
        r = u_write((unsigned)fd, out, out_n);
        if (r < 0 || (unsigned)r != out_n) {
            u_put("ugpio FAIL: write /gpio.out\n");
            u_close((unsigned)fd);
            u_exit();
        }
    }
    u_close((unsigned)fd);

    u_put("[ugpio] ok\n");
    if (!u_touch("/.ugpio_done"))
        u_put("ugpio WARN: sentinel /.ugpio_done gagal\n");

    u_exit();
    for (;;) { }
}
