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
 *   ufs   : (Fase 16) tunggu /.fat_cmd_ready -> eksekusi /fat.cmd
 *           (mkdir/w/r/ls/d di /sd FAT32) -> tulis /fat.out
 *           -> /.ufs_done
 *   umon  : (Fase 17) snapshot statistik kernel -> gambar frame TUI
 *           -> tulis /umon.out -> /.umon_done (mode live bila
 *           /umon.live ada; one-shot untuk verifikasi otomatis)
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

/* Fase 16: skrip uji FAT32 untuk ufs (pola usd). mkdir + tulis/baca
 * byte-exact + readdir + hapus, termasuk uji fragmentasi: A/B/C
 * masing-masing 1 cluster, B dihapus, D (6000 byte = 2 cluster)
 * memakai ulang cluster B -> chain tak-kontigu. */
static const char FAT_CMD[] =
    "mkdir /sd/T1\n"
    "w /sd/T1/A.TXT 3000\n"
    "r /sd/T1/A.TXT 3000\n"
    "ls /sd/T1\n"
    "d /sd/T1/A.TXT\n"
    "mkdir /sd/FRAG\n"
    "w /sd/FRAG/A.BIN 4096\n"
    "w /sd/FRAG/B.BIN 4096\n"
    "w /sd/FRAG/C.BIN 4096\n"
    "d /sd/FRAG/B.BIN\n"
    "w /sd/FRAG/D.BIN 6000\n"
    "r /sd/FRAG/D.BIN 6000\n"
    "r /sd/FRAG/A.BIN 4096\n"
    "r /sd/FRAG/C.BIN 4096\n"
    "ls /sd/FRAG\n"
    "ls /sd\n";

/* Buffer baca /fat.out (isinya ~600 byte: hasil semua perintah ufs
 * + listing direktori; buf[128] di _start tak cukup). */
static char fatbuf[1024];

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

    /* 1d. Fase 16: tulis skrip uji FAT32 untuk ufs, lalu sentinel.
     * ufs menunggu /.fat_cmd_ready (pola ucat/uls/ugpio/usd). */
    fd = u_open("/fat.cmd", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("FAIL: open /fat.cmd\n");
        fails++;
    } else {
        n = u_strlen(FAT_CMD);
        r = u_write((unsigned)fd, FAT_CMD, n);
        if (r < 0 || (unsigned)r != n) {
            u_put("FAIL: write /fat.cmd\n");
            fails++;
        }
        u_close((unsigned)fd);
        if (!u_touch("/.fat_cmd_ready")) {
            u_put("FAIL: sentinel /.fat_cmd_ready\n");
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

    /* Fase 16: tunggu ufs, lalu verifikasi /fat.out memuat hasil
     * operasi FAT32 yang diharapkan. */
    if (!u_wait_file("/.ufs_done")) {
        u_put("FAIL: ufs timeout\n");
        fails++;
    } else {
        u_put("[init] ufs selesai\n");
        for (i = 0; i < sizeof(fatbuf); i++)
            fatbuf[i] = 0;
        fd = u_open("/fat.out", O_RDONLY);
        if (fd < 0) {
            u_put("FAIL: open /fat.out\n");
            fails++;
        } else {
            r = u_read((unsigned)fd, fatbuf, sizeof(fatbuf) - 1u);
            u_close((unsigned)fd);
            if (r <= 0 ||
                !u_contains(fatbuf, (unsigned)r, "mkdir /sd/T1=ok") ||
                !u_contains(fatbuf, (unsigned)r, "w /sd/T1/A.TXT 3000=ok") ||
                !u_contains(fatbuf, (unsigned)r, "r /sd/T1/A.TXT 3000=ok") ||
                !u_contains(fatbuf, (unsigned)r, "ls> A.TXT") ||
                !u_contains(fatbuf, (unsigned)r, "d /sd/T1/A.TXT=ok") ||
                !u_contains(fatbuf, (unsigned)r, "mkdir /sd/FRAG=ok") ||
                !u_contains(fatbuf, (unsigned)r, "w /sd/FRAG/D.BIN 6000=ok") ||
                !u_contains(fatbuf, (unsigned)r, "r /sd/FRAG/D.BIN 6000=ok") ||
                !u_contains(fatbuf, (unsigned)r, "ls> D.BIN") ||
                !u_contains(fatbuf, (unsigned)r, "ls> HELLO.TXT")) {
                u_put("FAIL: /fat.out tidak sesuai operasi FAT32\n");
                fails++;
            } else {
                u_put("[init] /fat.out terverifikasi (FAT32 roundtrip)\n");
            }
        }
    }

    /* Fase 17: tunggu umon, lalu verifikasi /umon.out memuat
     * ringkasan statistik (cpu_pct=, mem_used_kb=, threads=). */
    if (!u_wait_file("/.umon_done")) {
        u_put("FAIL: umon timeout\n");
        fails++;
    } else {
        u_put("[init] umon selesai\n");
        for (i = 0; i < sizeof(fatbuf); i++)
            fatbuf[i] = 0;
        fd = u_open("/umon.out", O_RDONLY);
        if (fd < 0) {
            u_put("FAIL: open /umon.out\n");
            fails++;
        } else {
            r = u_read((unsigned)fd, fatbuf, sizeof(fatbuf) - 1u);
            u_close((unsigned)fd);
            if (r <= 0 ||
                !u_contains(fatbuf, (unsigned)r, "cpu_pct=") ||
                !u_contains(fatbuf, (unsigned)r, "mem_used_kb=") ||
                !u_contains(fatbuf, (unsigned)r, "threads=")) {
                u_put("FAIL: /umon.out tidak memuat statistik\n");
                fails++;
            } else {
                u_put("[init] /umon.out terverifikasi (statistik umon)\n");
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
