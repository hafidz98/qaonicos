/*
 * fs.h - ramfs: filesystem dalam RAM (Fase 9, bring-up).
 *
 * Desain bring-up (disengaja minimal):
 *  - FLAT: satu root, TANPA subdirektori. Nama file tidak boleh
 *    mengandung '/' di tengah (ditolak). '/' di depan diabaikan
 *    ("/a.txt" == "a.txt").
 *  - GLOBAL: tabel file satu untuk seluruh sistem (seperti mount
 *    tunggal); tabel fd (open-file description + offset) milik
 *    PER-TASK di struct task (konsisten dengan desain Fase 6/8:
 *    task = protection domain pemilik vm_space + ipc_space).
 *  - Data file di pool BSS statis (deterministik, tanpa interplay
 *    dengan page allocator). fd basi (file di-delete lalu slot
 *    dipakai ulang) terdeteksi via generation counter.
 *
 * Batas-batas (lihat commit Fase 9 bila ingin menaikkan):
 *  - maks 16 file, nama maks 31 char (+NUL), tiap file maks 64 KB,
 *    fd per task maks 16 (0/1/2 = console).
 */
#ifndef _FS_H_
#define _FS_H_

#include <stdint.h>

struct task;    /* task.h; dihindari include sirkular */

/* Batas-batas bring-up. */
#define FS_MAX_FILES  16u
#define FS_NAME_MAX   32u               /* termasuk NUL */
#define FS_PATH_MAX   64u               /* path dari user, termasuk NUL */
#define FS_FILE_MAX   (64u * 1024u)     /* 64 KB per file */
#define FS_MAX_FD     16u               /* fd per task */

/* Flags open (disalin ke user/usys.h; nilai ala Linux). */
#define FS_O_RDONLY   0u
#define FS_O_WRONLY   1u
#define FS_O_RDWR     2u
#define FS_O_CREAT    0x40u
#define FS_O_ACCMODE  3u

/* Handle file terbuka (satu per fd >= 3 di tiap task). */
struct fs_fd {
    int used;
    unsigned fidx;      /* index ke tabel file global */
    unsigned gen;       /* generasi file saat open */
    uint32_t off;       /* offset baca/tulis */
    unsigned flags;
};

/* fidx khusus untuk fd console 0/1/2 (bukan file). */
#define FS_FD_CONSOLE 0xFFFFFFFFu

void fs_init(void);

/* Cari file (nama sudah dinormalisasi di dalam). -> index / -1. */
int fs_find(const char *name);

/* Buat file kosong baru. -> index / -1 (nama jelek / sudah ada /
 * tabel penuh). */
int fs_create(const char *name);

/* Open: cari, atau buat bila O_CREAT. -> fd (>=3) / -1. */
int fs_open(struct task *t, const char *name, unsigned flags);

/* Baca/tulis lewat fd. dst/src = buffer KERNEL (pemanggil syscall
 * sudah memvalidasi range user sebelumnya). -> byte / -1. */
int fs_read(struct task *t, unsigned fd, uint8_t *dst, uint32_t len);
int fs_write(struct task *t, unsigned fd, const uint8_t *src,
             uint32_t len);

int fs_close(struct task *t, unsigned fd);

/* Tulis "nama\n" tiap file ke dst (maks `max` byte). -> jumlah file
 * (walau tidak semua muat ditulis). */
int fs_list(char *dst, uint32_t max);

/* Hapus file. fd yang masih terbuka ke file ini menjadi basi
 * (terdeteksi via gen). -> 0 / -1. */
int fs_delete(const char *name);

#endif /* _FS_H_ */
