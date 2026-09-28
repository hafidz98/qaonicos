/*
 * ufs.c - Utilitas uji filesystem FAT32 userspace (Fase 16).
 *
 * Tanpa argv (shell paling akhir): "baris perintah" dibaca dari
 * /fat.cmd yang ditulis init, satu perintah per baris:
 *   mkdir <path>            -> buat direktori
 *   w <path> <len>          -> tulis pola deterministik (seed dari path)
 *   r <path> <len>          -> baca, verifikasi pola byte-exact
 *   ls <path>               -> readdir; listing dicatat ke /fat.out
 *   d <path>                -> hapus file
 * Hasil tiap perintah ditulis ke /fat.out sebagai "<cmd>=ok\n"
 * (untuk ls, entri listing dicatat sebagai "ls> <nama>\n"),
 * lalu sentinel /.ufs_done. Pola koordinasi = ucat/ugpio/usd:
 * init menulis /fat.cmd -> sentinel /.fat_cmd_ready; ufs menunggu
 * sentinel itu, mengeksekusi perintah, menulis hasil.
 *
 * Bare-metal, tanpa libc. Di-link di UFS_PROG_VA (0x10058000) via
 * ufs.ld, di-embed sebagai blob -> ufs_img.
 */
#include "ulib.h"

#define UFS_BUF_SZ 8192u

static unsigned char databuf[UFS_BUF_SZ];
static char lsbuf[1024];

static unsigned path_seed(const char *p, const char *end)
{
    unsigned s = 0u;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        s += (unsigned char)*p;
        p++;
    }
    return s & 0xFFu;
}

static void fill_pattern(const char *path, const char *pend,
                         unsigned char *buf, unsigned len)
{
    unsigned seed = path_seed(path, pend);
    unsigned i;
    for (i = 0u; i < len; i++)
        buf[i] = (unsigned char)((i * 31u + seed) & 0xFFu);
}

static int check_pattern(const char *path, const char *pend,
                         const unsigned char *buf, unsigned len)
{
    unsigned seed = path_seed(path, pend);
    unsigned i;
    for (i = 0u; i < len; i++)
        if (buf[i] != (unsigned char)((i * 31u + seed) & 0xFFu))
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

/* Salin path (sampai spasi/EOL) ke zpath + NUL. -> panjang, 0 = gagal. */
static unsigned copy_path(const char *p, const char *end,
                          char *zpath, unsigned max)
{
    unsigned n = 0u;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' &&
           *p != '\n' && n + 1u < max) {
        zpath[n++] = *p++;
    }
    zpath[n] = '\0';
    return n;
}

__attribute__((section(".text.start")))
void _start(void)
{
    static char cmd[1024];
    static char out[2048];
    static char zpath[64];
    unsigned out_n = 0u;
    int fd, r, fails = 0;
    const char *p, *end, *line, *line_end;

    if (!u_wait_file("/.fat_cmd_ready")) {
        u_put("ufs FAIL: /.fat_cmd_ready timeout\n");
        u_exit();
    }

    fd = u_open("/fat.cmd", O_RDONLY);
    if (fd < 0) {
        u_put("ufs FAIL: open /fat.cmd\n");
        u_exit();
    }
    r = u_read((unsigned)fd, cmd, sizeof(cmd) - 1u);
    u_close((unsigned)fd);
    if (r <= 0) {
        u_put("ufs FAIL: baca /fat.cmd\n");
        u_exit();
    }
    p = cmd;
    end = cmd + r;

#define EMIT(s) do { \
        const char *_e = (s); \
        while (*_e && out_n + 1u < sizeof(out)) \
            out[out_n++] = *_e++; \
    } while (0)

    while (p < end) {
        unsigned plen, len, i;
        int ok;
        const char *q;

        line = skip_ws(p, end);
        line_end = line;
        while (line_end < end && *line_end != '\n')
            line_end++;
        p = line_end < end ? line_end + 1u : end;
        if (line >= line_end)
            continue;

        if (line[0] == 'm' && line_end - line > 6 &&
            line[1] == 'k' && line[2] == 'd' && line[3] == 'i' &&
            line[4] == 'r' && line[5] == ' ') {
            /* mkdir <path> */
            q = skip_ws(line + 6, line_end);
            plen = copy_path(q, line_end, zpath, sizeof(zpath));
            if (plen == 0u || u_mkdir(zpath) != 0) {
                u_put("ufs FAIL: mkdir\n");
                fails++;
                continue;
            }
            EMIT("mkdir ");
            EMIT(zpath);
            EMIT("=ok\n");
        } else if (line[0] == 'w' && line[1] == ' ') {
            /* w <path> <len> */
            q = skip_ws(line + 2, line_end);
            plen = copy_path(q, line_end, zpath, sizeof(zpath));
            q = skip_ws(q + plen, line_end);
            len = parse_u(&q, line_end, &ok);
            if (plen == 0u || !ok || len == 0u || len > UFS_BUF_SZ) {
                u_put("ufs FAIL: argumen w\n");
                fails++;
                continue;
            }
            fill_pattern(zpath, zpath + plen, databuf, len);
            r = u_fat_write(zpath, databuf, len);
            if (r < 0 || (unsigned)r != len) {
                u_put("ufs FAIL: fat_write\n");
                fails++;
                continue;
            }
            EMIT("w ");
            EMIT(zpath);
            EMIT(" ");
            out_n += put_u(out + out_n, len);
            EMIT("=ok\n");
        } else if (line[0] == 'r' && line[1] == ' ') {
            /* r <path> <len> */
            q = skip_ws(line + 2, line_end);
            plen = copy_path(q, line_end, zpath, sizeof(zpath));
            q = skip_ws(q + plen, line_end);
            len = parse_u(&q, line_end, &ok);
            if (plen == 0u || !ok || len == 0u || len > UFS_BUF_SZ) {
                u_put("ufs FAIL: argumen r\n");
                fails++;
                continue;
            }
            for (i = 0u; i < len; i++)
                databuf[i] = 0u;
            r = u_fat_read(zpath, databuf, len);
            if (r < 0 || (unsigned)r != len ||
                !check_pattern(zpath, zpath + plen, databuf, len)) {
                u_put("ufs FAIL: fat_read/verify\n");
                fails++;
                continue;
            }
            EMIT("r ");
            EMIT(zpath);
            EMIT(" ");
            out_n += put_u(out + out_n, len);
            EMIT("=ok\n");
        } else if (line[0] == 'l' && line[1] == 's' && line[2] == ' ') {
            /* ls <path> */
            const char *le, *ls;
            q = skip_ws(line + 3, line_end);
            plen = copy_path(q, line_end, zpath, sizeof(zpath));
            if (plen == 0u) {
                u_put("ufs FAIL: argumen ls\n");
                fails++;
                continue;
            }
            for (i = 0u; i < sizeof(lsbuf); i++)
                lsbuf[i] = 0;
            r = u_readdir(zpath, lsbuf, sizeof(lsbuf) - 1u);
            if (r < 0) {
                u_put("ufs FAIL: readdir\n");
                fails++;
                continue;
            }
            EMIT("ls ");
            EMIT(zpath);
            EMIT("=ok\n");
            /* Catat tiap entri listing untuk verifikasi init. */
            ls = lsbuf;
            le = lsbuf;
            while (*le) {
                if (*le == '\n') {
                    EMIT("ls> ");
                    while (ls < le && out_n + 1u < sizeof(out))
                        out[out_n++] = *ls++;
                    EMIT("\n");
                    ls = le + 1u;
                }
                le++;
            }
        } else if (line[0] == 'd' && line[1] == ' ') {
            /* d <path> */
            q = skip_ws(line + 2, line_end);
            plen = copy_path(q, line_end, zpath, sizeof(zpath));
            if (plen == 0u || u_fat_delete(zpath) != 0) {
                u_put("ufs FAIL: fat_delete\n");
                fails++;
                continue;
            }
            EMIT("d ");
            EMIT(zpath);
            EMIT("=ok\n");
        } else {
            u_put("ufs FAIL: perintah tak dikenal\n");
            fails++;
        }
    }

#undef EMIT

    if (fails != 0) {
        u_put("ufs FAIL: eksekusi perintah\n");
        u_exit();
    }

    fd = u_open("/fat.out", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("ufs FAIL: open /fat.out\n");
        u_exit();
    }
    if (out_n > 0u) {
        r = u_write((unsigned)fd, out, out_n);
        if (r < 0 || (unsigned)r != out_n) {
            u_put("ufs FAIL: write /fat.out\n");
            u_close((unsigned)fd);
            u_exit();
        }
    }
    u_close((unsigned)fd);

    u_put("[ufs] ok\n");
    if (!u_touch("/.ufs_done"))
        u_put("ufs WARN: sentinel /.ufs_done gagal\n");

    u_exit();
    for (;;) { }
}
