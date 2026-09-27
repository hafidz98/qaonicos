/*
 * uecho.c - Utilitas userspace "echo ke file" (Fase 10).
 *
 * KETERBATASAN: tanpa argv (calon Fase 11) — string fix UECHO_STR
 * (definisi bersama di ulib.h, diverifikasi init.c secara
 * byte-exact). Dipanggil langsung oleh koordinasi init via sentinel
 * (BUKAN via shell — shell paling akhir).
 *
 * Alur: tulis UECHO_STR ke /echo.txt -> baca balik -> verifikasi
 * byte-exact -> sentinel /.uecho_done (hanya bila verifikasi lolos).
 */
#include "ulib.h"

__attribute__((section(".text.start")))
void _start(void)
{
    int fd, r, fails = 0;
    char buf[64];
    unsigned i, n;

    n = u_strlen(UECHO_STR);

    fd = u_open("/echo.txt", O_CREAT | O_RDWR);
    if (fd < 0) {
        u_put("uecho FAIL: open /echo.txt\n");
        u_exit();
    }
    r = u_write((unsigned)fd, UECHO_STR, n);
    if (r < 0 || (unsigned)r != n) {
        u_put("uecho FAIL: write\n");
        fails++;
    }
    u_close((unsigned)fd);

    /* Baca balik & verifikasi byte-exact. */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = 0;
    fd = u_open("/echo.txt", O_RDONLY);
    if (fd < 0) {
        u_put("uecho FAIL: open baca\n");
        fails++;
    } else {
        r = u_read((unsigned)fd, buf, sizeof(buf));
        if (r < 0 || (unsigned)r != n ||
            u_mcmp(buf, UECHO_STR, n) != 0) {
            u_put("uecho FAIL: tidak byte-exact\n");
            fails++;
        } else {
            u_put("[uecho] verifikasi ok\n");
        }
        u_close((unsigned)fd);
    }

    if (fails == 0) {
        if (!u_touch("/.uecho_done"))
            u_put("uecho WARN: sentinel /.uecho_done gagal\n");
    } else {
        u_put("uecho FAILED\n");
    }

    u_exit();
    for (;;) { }
}
