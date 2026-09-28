/*
 * uls.c - Utilitas userspace "ls" (Fase 10).
 *
 * KETERBATASAN: tanpa argv (calon Fase 11) — selalu me-list root
 * ramfs. Dipanggil langsung oleh koordinasi init via sentinel
 * (BUKAN via shell — shell paling akhir).
 *
 * Alur: tunggu /.motd_ready (agar listing deterministik memuat
 * motd.txt) -> SYS_LS -> tulis "nama\n" per file ke console ->
 * sentinel /.uls_done.
 *
 * Catatan: fs_list kernel menulis "nama\n" per file TANPA NUL di
 * ujung (return = jumlah file, bukan byte). Buffer di-nol-kan dulu,
 * lalu panjang output dihitung dengan scan terbatas (nama file tak
 * mengandung NUL, jadi NUL pertama = akhir data).
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void _start(void)
{
    int r;
    char buf[512];
    unsigned i, total;

    for (i = 0; i < sizeof(buf); i++)
        buf[i] = 0;

    if (!u_wait_file("/.motd_ready")) {
        u_put("uls FAIL: /.motd_ready timeout\n");
        u_exit();
    }
    r = u_ls(buf, sizeof(buf));
    if (r < 0) {
        u_put("uls FAIL: sys_ls\n");
        u_exit();
    }
    for (total = 0; total < sizeof(buf) && buf[total]; total++)
        ;
    if (total > 0)
        u_write(1u, buf, total);
    u_put("[uls] ok\n");

    if (!u_touch("/.uls_done"))
        u_put("uls WARN: sentinel /.uls_done gagal\n");

    u_exit();
    for (;;) { }
}
