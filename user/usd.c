/*
 * usd.c - Utilitas SD card userspace (Fase 15).
 *
 * Tanpa argv (shell paling akhir): "baris perintah" dibaca dari
 * /sd.cmd yang ditulis init, satu perintah per baris:
 *   w <sector>   -> tulis pola deterministik ke sektor itu
 *   r <sector>   -> baca sektor, verifikasi pola byte-exact
 * Hasil tiap perintah ditulis ke /sd.out sebagai
 * "<op> <sector>=ok\n", lalu sentinel /.usd_done.
 * Pola koordinasi = ucat/ugpio: init menulis /sd.cmd -> sentinel
 * /.sd_cmd_ready; usd menunggu sentinel itu, mengeksekusi perintah,
 * menulis hasil.
 *
 * Sektor 0 TIDAK dipakai (superblock "QAONSD01"); init memakai
 * sektor 10, 100, 1000.
 *
 * Bare-metal, tanpa libc. Di-link di USD_PROG_VA (0x10050000) via
 * usd.ld, di-embed sebagai blob -> usd_img.
 */
#include "ulib.h"

#define SD_SECTOR_SZ 512u

static unsigned char secbuf[SD_SECTOR_SZ];

/* Pola deterministik dari nomor sektor (sama untuk tulis & baca). */
static void fill_pattern(unsigned sector, unsigned char *buf)
{
    unsigned i;
    for (i = 0u; i < SD_SECTOR_SZ; i++)
        buf[i] = (unsigned char)(sector + i * 13u + (i >> 8));
}

static int check_pattern(unsigned sector, const unsigned char *buf)
{
    unsigned i;
    for (i = 0u; i < SD_SECTOR_SZ; i++)
        if (buf[i] != (unsigned char)(sector + i * 13u + (i >> 8)))
            return 0;
    return 1;
}

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

    if (!u_wait_file("/.sd_cmd_ready")) {
        u_put("usd FAIL: /.sd_cmd_ready timeout\n");
        u_exit();
    }

    fd = u_open("/sd.cmd", O_RDONLY);
    if (fd < 0) {
        u_put("usd FAIL: open /sd.cmd\n");
        u_exit();
    }
    r = u_read((unsigned)fd, cmd, sizeof(cmd) - 1u);
    u_close((unsigned)fd);
    if (r <= 0) {
        u_put("usd FAIL: baca /sd.cmd\n");
        u_exit();
    }
    p = cmd;
    end = cmd + r;

    /* Eksekusi per baris. */
    while (p < end) {
        unsigned sector;
        int ok;
        line = skip_ws(p, end);
        /* Maju p ke akhir baris untuk iterasi berikut. */
        p = line;
        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;  /* lewati '\n' */
        if (line >= end || *line == '\n')
            continue;

        if (*line == 'w' || *line == 'r') {
            char op = *line;
            const char *q = skip_ws(line + 1, end);
            sector = parse_u(&q, end, &ok);
            if (!ok) { fails++; continue; }
            if (op == 'w') {
                fill_pattern(sector, secbuf);
                if (u_sd_write(sector, secbuf) != 0) {
                    u_put("usd FAIL: sd_write\n");
                    fails++;
                    continue;
                }
            } else {
                unsigned i;
                for (i = 0u; i < SD_SECTOR_SZ; i++)
                    secbuf[i] = 0u;
                if (u_sd_read(sector, secbuf) != 0 ||
                    !check_pattern(sector, secbuf)) {
                    u_put("usd FAIL: sd_read/verify\n");
                    fails++;
                    continue;
                }
            }
            /* Catat "<op> <sector>=ok\n". */
            if (out_n + 16u < sizeof(out)) {
                out[out_n++] = op;
                out[out_n++] = ' ';
                out_n += put_u(out + out_n, sector);
                out[out_n++] = '=';
                out[out_n++] = 'o';
                out[out_n++] = 'k';
                out[out_n++] = '\n';
            }
        } else {
            u_put("usd FAIL: perintah tak dikenal\n");
            fails++;
        }
    }

    if (fails != 0) {
        u_put("usd FAIL: eksekusi perintah\n");
        u_exit();
    }

    /* Tulis hasil. */
    fd = u_open("/sd.out", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("usd FAIL: open /sd.out\n");
        u_exit();
    }
    if (out_n > 0u) {
        r = u_write((unsigned)fd, out, out_n);
        if (r < 0 || (unsigned)r != out_n) {
            u_put("usd FAIL: write /sd.out\n");
            u_close((unsigned)fd);
            u_exit();
        }
    }
    u_close((unsigned)fd);

    u_put("[usd] ok\n");
    if (!u_touch("/.usd_done"))
        u_put("usd WARN: sentinel /.usd_done gagal\n");

    u_exit();
    for (;;) { }
}
