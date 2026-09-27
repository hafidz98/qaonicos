/*
 * ucat.c - Utilitas userspace "cat" (Fase 10).
 *
 * KETERBATASAN: tanpa argv (calon Fase 11) — path fix "/motd.txt"
 * yang disepakati dengan init.c. Dipanggil langsung oleh
 * koordinasi init via sentinel (BUKAN via shell — shell paling akhir).
 *
 * Alur: tunggu /.motd_ready (init selesai menulis) -> baca
 * /motd.txt -> tulis ke console (fd 1) -> sentinel /.ucat_done.
 */
#include "ulib.h"

__attribute__((section(".text.start")))
void _start(void)
{
    int fd, r;

    /* Buffer di stack user (2 halaman = 8KB, aman). */
    char buf[256];

    if (!u_wait_file("/.motd_ready")) {
        u_put("ucat FAIL: /.motd_ready timeout\n");
        u_exit();
    }
    fd = u_open("/motd.txt", O_RDONLY);
    if (fd < 0) {
        u_put("ucat FAIL: open /motd.txt\n");
        u_exit();
    }
    for (;;) {
        r = u_read((unsigned)fd, buf, sizeof(buf));
        if (r <= 0)
            break;
        u_write(1u, buf, (unsigned)r);
    }
    u_close((unsigned)fd);
    u_put("[ucat] ok\n");

    if (!u_touch("/.ucat_done"))
        u_put("ucat WARN: sentinel /.ucat_done gagal\n");

    u_exit();
    for (;;) { }
}
