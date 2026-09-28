/*
 * umon.c - System monitor TUI userspace (Fase 17).
 *
 * Membaca statistik REAL kernel via SYS_STAT/SYS_TLIST lalu
 * menggambar dashboard text-UI (library tui.h) di console UART.
 *
 * Tanpa argv (shell paling akhir, Fase 18): mode dipilih via file
 *   /umon.live  -> mode live: refresh tiap ~1 detik sampai tombol 'q'
 *   (tanpa file) -> one-shot: satu snapshot lalu exit.
 * Pola koordinasi = utilitas lain: tulis ringkasan teks ke /umon.out
 * (untuk verifikasi byte/grep oleh init) + sentinel /.umon_done.
 *
 * Bare-metal, tanpa libc. Di-link di UMON_PROG_VA (0x10060000) via
 * umon.ld, di-embed sebagai blob -> umon_img.
 */
#include "ulib.h"
#include "tui.h"

static struct qaon_stat st;
static struct qaon_tentry te[32];

#define UMON_SUM_SZ 512u
static char sumbuf[UMON_SUM_SZ];

/* uptime_ms saat ini (0 bila SYS_STAT gagal). */
static unsigned stat_uptime(void)
{
    if (u_stat(&st) != 0)
        return 0u;
    return st.uptime_ms;
}

/* Tunggu >= ms (poll uptime_ms + yield; ~20ms per iterasi). */
static void wait_ms(unsigned ms)
{
    unsigned t0 = stat_uptime();
    unsigned i;

    for (;;) {
        for (i = 0u; i < 20u; i++)
            u_yield();
        if (stat_uptime() - t0 >= ms)
            return;
    }
}

static void sum_app(const char *s)
{
    unsigned n = 0u;
    while (sumbuf[n])
        n++;
    while (*s && n < UMON_SUM_SZ - 1u)
        sumbuf[n++] = *s++;
    sumbuf[n] = 0;
}

static void sum_app_u(unsigned v)
{
    char tmp[10];
    int n = 0, k;
    if (v == 0u) {
        sum_app("0");
        return;
    }
    while (v > 0u && n < 10) {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    }
    for (k = n - 1; k >= 0; k--) {
        char c[2] = { tmp[k], 0 };
        sum_app(c);
    }
}

/* Tulis ringkasan teks ke /umon.out (diverifikasi init via grep). */
static int write_summary(void)
{
    int fd, r = 0;
    unsigned i;

    for (i = 0u; i < UMON_SUM_SZ; i++)
        sumbuf[i] = 0;
    sum_app("umon snapshot\nuptime_ms=");
    sum_app_u(st.uptime_ms);
    sum_app("\ncpu_pct=");
    sum_app_u(st.cpu_pct);
    sum_app("\nmem_used_kb=");
    sum_app_u(st.mem_used_kb);
    sum_app("\nmem_total_kb=");
    sum_app_u(st.mem_total_kb);
    sum_app("\nblk_total_sec=");
    sum_app_u(st.blk_total_sec);
    sum_app("\nblk_used_sec=");
    sum_app_u(st.blk_used_sec);
    sum_app("\nnet_rx_kb=");
    sum_app_u(st.net_rx_kb);
    sum_app("\nnet_tx_kb=");
    sum_app_u(st.net_tx_kb);
    sum_app("\nthreads=");
    sum_app_u(st.nthreads);
    sum_app("\n");

    fd = u_open("/umon.out", O_CREAT | O_RDWR);
    if (fd < 0)
        return -1;
    {
        unsigned n = 0u;
        while (sumbuf[n])
            n++;
        if (u_write((unsigned)fd, sumbuf, n) != (int)n)
            r = -1;
    }
    u_close((unsigned)fd);
    return r;
}

static const char *state_name(unsigned s)
{
    if (s == 0u)
        return "RUNNABLE";
    if (s == 1u)
        return "BLOCKED";
    if (s == 2u)
        return "DEAD";
    return "?";
}

/* Gambar satu frame penuh 80x24. */
static void draw_frame(int nte)
{
    unsigned i, shown, sec;
    unsigned mem_pct, blk_pct;

    tui_begin();
    tui_clear();

    /* Baris 1: judul + uptime. */
    tui_at(1u, 2u, "QaonicOS System Monitor");
    tui_at(1u, 58u, "uptime ");
    sec = st.uptime_ms / 1000u;
    tui_putu(sec);
    tui_puts("s");
    tui_at(2u, 2u, "----------------------------------------------------------------------------");

    /* Baris 3-6: panel statistik. */
    tui_at(3u, 2u, "CPU  ");
    tui_bar(62u, st.cpu_pct);
    tui_puts(" ");
    tui_putu(st.cpu_pct);
    tui_puts("%");

    mem_pct = st.mem_total_kb ?
        (st.mem_used_kb * 100u / st.mem_total_kb) : 0u;
    tui_at(4u, 2u, "MEM  ");
    tui_bar(62u, mem_pct);
    tui_puts(" ");
    tui_putu(mem_pct);
    tui_puts("% ");
    tui_putu(st.mem_used_kb);
    tui_puts("/");
    tui_putu(st.mem_total_kb);
    tui_puts(" KB");

    blk_pct = st.blk_total_sec ?
        (st.blk_used_sec * 100u / st.blk_total_sec) : 0u;
    tui_at(5u, 2u, "DISK ");
    tui_bar(62u, blk_pct);
    tui_puts(" ");
    tui_putu(blk_pct);
    tui_puts("% ");
    tui_putu(st.blk_used_sec);
    tui_puts("/");
    tui_putu(st.blk_total_sec);
    tui_puts(" sec");

    tui_at(6u, 2u, "NET  RX ");
    tui_putu(st.net_rx_kb);
    tui_puts(" KB  TX ");
    tui_putu(st.net_tx_kb);
    tui_puts(" KB");
    tui_at(7u, 2u, "----------------------------------------------------------------------------");

    /* Baris 8-23: daftar thread (maks 14 baris muat). */
    tui_at(8u, 2u, "Threads (");
    tui_putu(st.nthreads);
    tui_puts(")");
    tui_at(9u, 4u, "id  state     mode");
    shown = 0u;
    for (i = 0u; i < (unsigned)nte && shown < 14u; i++) {
        tui_at(10u + shown, 4u, "");
        tui_putu(te[i].id);
        tui_puts("   ");
        tui_puts(state_name(te[i].state));
        tui_puts(te[i].state == 0u ? "  " :
                 te[i].state == 1u ? "   " : "      ");
        tui_puts(te[i].user ? "user" : "kernel");
        shown++;
    }
    if ((unsigned)nte > shown) {
        tui_at(10u + shown, 4u, "+");
        tui_putu((unsigned)nte - shown);
        tui_puts(" more");
    }

    /* Baris 24: status bar. */
    tui_status(24u, " umon: q=keluar (mode live) | snapshot -> /umon.out");
    tui_flush();
}

/* Ambil statistik + daftar thread terbaru. */
static int sample(void)
{
    int n;

    if (u_stat(&st) != 0)
        return -1;
    n = u_tlist(te, 32u);
    if (n < 0)
        return -1;
    return n;
}

__attribute__((section(".text.start")))
void _start(void)
{
    int nte, fd, live;

    u_put("[umon] mulai\n");

    /* Beri waktu ~1.2 dtk agar window statistik (500ms) terisi angka
     * yang bermakna dan boot burst selesai. */
    wait_ms(1200u);

    nte = sample();
    if (nte < 0) {
        u_put("FAIL: umon sample\n");
        u_exit();
    }
    draw_frame(nte);
    u_put("\n[umon] frame digambar\n");

    if (write_summary() != 0) {
        u_put("FAIL: tulis /umon.out\n");
        u_exit();
    }
    u_put("[umon] /umon.out ditulis\n");
    if (!u_touch("/.umon_done")) {
        u_put("FAIL: sentinel /.umon_done\n");
        u_exit();
    }

    /* Mode live: ada /umon.live -> refresh tiap detik sampai 'q'.
     * (Tanpa file: one-shot, langsung exit — untuk verifikasi
     * otomatis 5/5 run.) */
    fd = u_open("/umon.live", O_RDONLY);
    live = (fd >= 0);
    if (live)
        u_close((unsigned)fd);
    if (!live) {
        u_put("[umon] selesai (one-shot)\n");
        u_exit();
    }

    u_put("[umon] mode live (q=keluar)\n");
    for (;;) {
        int c;
        wait_ms(1000u);
        nte = sample();
        if (nte >= 0)
            draw_frame(nte);
        /* Kuras input: 'q'/'Q' -> keluar, sisanya diabaikan. */
        for (;;) {
            c = u_console_getc();
            if (c < 0)
                break;
            if (c == 'q' || c == 'Q') {
                u_put("\n[umon] keluar (q)\n");
                u_exit();
            }
        }
    }
}
