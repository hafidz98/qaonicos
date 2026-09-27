/*
 * init.c - Program init userspace (Fase 10): "PID 1" nano OS.
 *
 * Bare-metal, tanpa libc. Di-link di INIT_PROG_VA (0x10012000) via
 * init.ld, di-embed sebagai blob -> init_img, dimuat kernel ke
 * task_user sebelum thread init dijadwalkan.
 *
 * KETERBATASAN (disengaja, bukan bug): tidak ada argv — utilitas
 * memakai path/konstanta fix yang disepakati di sini & ulib.h.
 * argv = calon Fase 11.
 *
 * "Spawn" di fase ini = kernel mendaftarkan SEMUA thread user saat
 * boot via sched_add_user (mekanisme yang sudah ada sejak Fase 8/9);
 * init mengoordinasi utilitas lewat file sentinel di ramfs, BUKAN
 * lewat syscall spawn/exec (itu kerja masa depan, post-shell roadmap
 * menempatkan shell PALING AKHIR).
 *
 * Protokol koordinasi:
 *   init  : tulis /motd.txt -> sentinel /.motd_ready
 *   ucat  : tunggu /.motd_ready -> cat /motd.txt -> /.ucat_done
 *   uls   : tunggu /.motd_ready -> ls -> /.uls_done
 *   uecho : tulis /echo.txt, verifikasi -> /.uecho_done
 *   init  : tunggu 3 sentinel -> verifikasi /echo.txt byte-exact
 *           -> "INIT TESTS PASSED" -> sentinel /.init_done
 *           (/.init_done = gerbang halt kernel di report()).
 */
#include "ulib.h"

static const char MOTD[] = "Selamat datang di Mach nano OS!\n";

__attribute__((section(".text.start")))
void _start(void)
{
    int fd, r, fails = 0;
    char buf[128];
    unsigned i, n;

    u_put("[init] init mulai\n");

    /* 1. Tulis /motd.txt, lalu sentinel sesudah close (ucat/uls
     * menunggu sentinel, bukan motd.txt langsung — menghindari
     * membaca file yang baru dibuat tapi masih kosong). */
    fd = u_open("/motd.txt", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("FAIL: open /motd.txt\n");
        fails++;
    } else {
        n = u_strlen(MOTD);
        r = u_write((unsigned)fd, MOTD, n);
        if (r < 0 || (unsigned)r != n) {
            u_put("FAIL: write /motd.txt\n");
            fails++;
        }
        u_close((unsigned)fd);
        if (!u_touch("/.motd_ready")) {
            u_put("FAIL: sentinel /.motd_ready\n");
            fails++;
        }
    }
    u_put("[init] motd ditulis, menunggu utilitas...\n");

    /* 2. Tunggu ketiga utilitas selesai (dengan timeout). */
    if (!u_wait_file("/.ucat_done")) {
        u_put("FAIL: ucat timeout\n");
        fails++;
    } else {
        u_put("[init] ucat selesai\n");
    }
    if (!u_wait_file("/.uls_done")) {
        u_put("FAIL: uls timeout\n");
        fails++;
    } else {
        u_put("[init] uls selesai\n");
    }
    if (!u_wait_file("/.uecho_done")) {
        u_put("FAIL: uecho timeout\n");
        fails++;
    } else {
        u_put("[init] uecho selesai\n");
    }

    /* 3. Verifikasi /echo.txt byte-exact terhadap UECHO_STR
     * (definisi bersama di ulib.h — uecho.c memakai yang sama). */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    fd = u_open("/echo.txt", O_RDONLY);
    if (fd < 0) {
        u_put("FAIL: open /echo.txt\n");
        fails++;
    } else {
        n = u_strlen(UECHO_STR);
        r = u_read((unsigned)fd, buf, sizeof(buf));
        if (r < 0 || (unsigned)r != n ||
            u_mcmp(buf, UECHO_STR, n) != 0) {
            u_put("FAIL: /echo.txt tidak byte-exact\n");
            fails++;
        } else {
            u_put("[init] /echo.txt terverifikasi byte-exact\n");
        }
        u_close((unsigned)fd);
    }

    if (fails == 0) {
        u_put("INIT TESTS PASSED\n");
        /* Sentinel HANYA bila semua verifikasi lolos (gerbang halt
         * kernel — report() menunggu ini sebelum "halting"). */
        if (!u_touch("/.init_done"))
            u_put("WARN: sentinel /.init_done gagal\n");
    } else {
        u_put("INIT TESTS FAILED\n");
    }

    u_exit();
    for (;;) { }
}
