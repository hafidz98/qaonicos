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
 *   ugpio : (Fase 14) tunggu /.gpio_cmd_ready -> eksekusi /gpio.cmd
 *           -> tulis /gpio.out -> /.ugpio_done
 *   usd   : (Fase 15) tunggu /.sd_cmd_ready -> eksekusi /sd.cmd
 *           (w/r sektor SD) -> tulis /sd.out -> /.usd_done
 *   init  : tunggu 3 sentinel -> verifikasi /echo.txt byte-exact
 *           -> "INIT TESTS PASSED" -> sentinel /.init_done
 *           (/.init_done = gerbang halt kernel di report()).
 */
#include "ulib.h"

static const char MOTD[] = "Selamat datang di Mach nano OS!\n";

/* Fase 14: skrip perintah GPIO untuk ugpio (pola ucat: init menulis
 * perintah + sentinel, ugpio mengeksekusi). Roundtrip set->get di
 * beberapa pin, termasuk pin tertinggi bank 0 (31). */
static const char GPIO_CMD[] =
    "set 7 1\n"
    "get 7\n"
    "set 7 0\n"
    "get 7\n"
    "set 31 1\n"
    "get 31\n";

/* Fase 15: skrip uji SD card untuk usd (pola ugpio). Tulis pola ke
 * sektor lalu baca balik + verifikasi byte-exact. Sektor 0 tidak
 * dipakai (superblock "QAONSD01"). */
static const char SD_CMD[] =
    "w 10\n"
    "r 10\n"
    "w 100\n"
    "r 100\n"
    "w 1000\n"
    "r 1000\n";

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

    /* 1b. Fase 14: tulis skrip GPIO untuk ugpio, lalu sentinel.
     * ugpio menunggu /.gpio_cmd_ready (pola ucat/uls). */
    fd = u_open("/gpio.cmd", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("FAIL: open /gpio.cmd\n");
        fails++;
    } else {
        n = u_strlen(GPIO_CMD);
        r = u_write((unsigned)fd, GPIO_CMD, n);
        if (r < 0 || (unsigned)r != n) {
            u_put("FAIL: write /gpio.cmd\n");
            fails++;
        }
        u_close((unsigned)fd);
        if (!u_touch("/.gpio_cmd_ready")) {
            u_put("FAIL: sentinel /.gpio_cmd_ready\n");
            fails++;
        }
    }

    /* 1c. Fase 15: tulis skrip uji SD untuk usd, lalu sentinel.
     * usd menunggu /.sd_cmd_ready (pola ucat/uls/ugpio). */
    fd = u_open("/sd.cmd", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("FAIL: open /sd.cmd\n");
        fails++;
    } else {
        n = u_strlen(SD_CMD);
        r = u_write((unsigned)fd, SD_CMD, n);
        if (r < 0 || (unsigned)r != n) {
            u_put("FAIL: write /sd.cmd\n");
            fails++;
        }
        u_close((unsigned)fd);
        if (!u_touch("/.sd_cmd_ready")) {
            u_put("FAIL: sentinel /.sd_cmd_ready\n");
            fails++;
        }
    }

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
    /* Fase 14: tunggu ugpio, lalu verifikasi /gpio.out memuat hasil
     * roundtrip yang diharapkan ("7=1", "7=0", "31=1"). */
    if (!u_wait_file("/.ugpio_done")) {
        u_put("FAIL: ugpio timeout\n");
        fails++;
    } else {
        u_put("[init] ugpio selesai\n");
        for (i = 0; i < sizeof(buf); i++)
            buf[i] = 0;
        fd = u_open("/gpio.out", O_RDONLY);
        if (fd < 0) {
            u_put("FAIL: open /gpio.out\n");
            fails++;
        } else {
            r = u_read((unsigned)fd, buf, sizeof(buf) - 1u);
            u_close((unsigned)fd);
            if (r <= 0 ||
                !u_contains(buf, (unsigned)r, "7=1") ||
                !u_contains(buf, (unsigned)r, "7=0") ||
                !u_contains(buf, (unsigned)r, "31=1")) {
                u_put("FAIL: /gpio.out tidak sesuai roundtrip\n");
                fails++;
            } else {
                u_put("[init] /gpio.out terverifikasi (set/get roundtrip)\n");
            }
        }
    }

    /* Fase 15: tunggu usd, lalu verifikasi /sd.out memuat hasil
     * roundtrip sektor yang diharapkan ("r 10=ok", "r 100=ok",
     * "r 1000=ok"). */
    if (!u_wait_file("/.usd_done")) {
        u_put("FAIL: usd timeout\n");
        fails++;
    } else {
        u_put("[init] usd selesai\n");
        for (i = 0; i < sizeof(buf); i++)
            buf[i] = 0;
        fd = u_open("/sd.out", O_RDONLY);
        if (fd < 0) {
            u_put("FAIL: open /sd.out\n");
            fails++;
        } else {
            r = u_read((unsigned)fd, buf, sizeof(buf) - 1u);
            u_close((unsigned)fd);
            if (r <= 0 ||
                !u_contains(buf, (unsigned)r, "r 10=ok") ||
                !u_contains(buf, (unsigned)r, "r 100=ok") ||
                !u_contains(buf, (unsigned)r, "r 1000=ok")) {
                u_put("FAIL: /sd.out tidak sesuai roundtrip\n");
                fails++;
            } else {
                u_put("[init] /sd.out terverifikasi (sector roundtrip)\n");
            }
        }
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
