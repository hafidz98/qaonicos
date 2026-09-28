/*
 * ulib.h - Pustaka mini userspace bersama (Fase 10).
 *
 * Wrapper syscall + helper string untuk program-program userspace
 * (init, ucat, uls, uecho). Di-compile ke DALAM tiap biner program
 * (bukan shared library — tiap program berdiri sendiri, mengikuti
 * pola self-contained hello.c/fstest.c).
 *
 * Nomor syscall DISALIN MANUAL ke immediate asm di ulib.c (pola
 * hello.c); usys.h adalah acuan tunggal bila nomor berubah.
 */
#ifndef _ULIB_H_
#define _ULIB_H_

#include "usys.h"

int u_write(unsigned fd, const char *buf, unsigned len);
int u_open(const char *path, unsigned flags);
int u_read(unsigned fd, char *buf, unsigned len);
int u_close(unsigned fd);
int u_ls(char *buf, unsigned max);
void u_yield(void);

/* Tidak kembali (menandai thread DEAD di kernel). */
void u_exit(void);

/* Tulis string NUL-terminated ke console (fd 1). */
void u_put(const char *s);

unsigned u_strlen(const char *s);

/* 0 bila a[0..n) == b[0..n) (ala memcmp). */
int u_mcmp(const char *a, const char *b, unsigned n);

/* 1 bila `needle` muncul di buf[0..len). */
int u_contains(const char *buf, unsigned len, const char *needle);

/* String yang ditulis uecho.c ke /echo.txt dan diverifikasi init.c
 * secara byte-exact. Satu definisi bersama di sini agar kedua sisi
 * tidak bisa beda diam-diam. */
#define UECHO_STR "uecho: halo dari utilitas\n"

/* Batas iterasi u_wait_file (yield tiap 256 iterasi). */
#define ULIB_POLL_MAX 1000000u

/* Tunggu sampai `path` bisa di-open O_RDONLY (poll + yield).
 * Kembalikan 1 bila muncul, 0 bila timeout. fd sementara SELALU
 * di-close di dalam (tidak bocor ke tabel fd task). */
int u_wait_file(const char *path);

/* Buat file sentinel kosong (open O_CREAT|O_RDWR lalu close).
 * Kembalikan 1 bila sukses, 0 bila gagal. */
int u_touch(const char *path);

/* Fase 14: GPIO (SYS_GPIO_SET=40, SYS_GPIO_GET=41).
 * u_gpio_set: 0 ok, -1 pin liar. u_gpio_get: 0/1, -1 pin liar. */
int u_gpio_set(unsigned pin, unsigned val);
int u_gpio_get(unsigned pin);

/* Fase 15: kartu SD (SYS_SD_READ=50, SYS_SD_WRITE=51).
 * u_sd_read/u_sd_write: 0 ok, -1 gagal. buf = 512 byte di VA user. */
int u_sd_read(unsigned sector, unsigned char *buf);
int u_sd_write(unsigned sector, const unsigned char *buf);

/* Fase 16: filesystem FAT32 di /sd (SYS_MKDIR=52, SYS_FAT_WRITE=53,
 * SYS_FAT_READ=54, SYS_FAT_DELETE=55, SYS_READDIR=56). Path absolut
 * "/sd/...". u_mkdir/u_fat_delete: 0 ok, -1 gagal.
 * u_fat_write: byte tertulis / -1. u_fat_read: byte dibaca / -1.
 * u_readdir: jumlah entri / -1 (format "NAMA.EXT\n", "DIR/\n"). */
int u_mkdir(const char *path);
int u_fat_write(const char *path, const unsigned char *buf, unsigned len);
int u_fat_read(const char *path, unsigned char *buf, unsigned max);
int u_fat_delete(const char *path);
int u_readdir(const char *path, char *buf, unsigned max);

#endif /* _ULIB_H_ */
