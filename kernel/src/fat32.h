/*
 * fat32.h - Filesystem FAT32 di atas kartu SD (blkdev dev 1).
 *
 * Fase 16: mount di "/sd". Namespace path:
 *   "/sd/..."  -> FAT32 di kartu SD (via sd_read/sd_write)
 *   path lain  -> ramfs (fs.c, tak tersentuh fase ini)
 *
 * Cakupan (disengaja minimal, seperti ramfs Fase 9):
 *  - 8.3 short names SAJA (LFN tidak didukung; entri LFN diabaikan
 *    saat lookup/listing).
 *  - Subdirektori didukung (mkdir + path bersarang).
 *  - File: create/truncate+write sekaligus, read, delete.
 *  - Tidak ada permission, timestamp, atau atribut selain
 *    direktori/file.
 *
 * Semua operasi sinkron (polled I/O, tanpa IRQ) dan berjalan atomik
 * terhadap thread lain: syscall tak mem-block, jadi buffer statis
 * di sini aman dipakai bergantian.
 */
#ifndef _FAT32_H_
#define _FAT32_H_

#include <stdint.h>

/* Mount dari dev 1 (kartu SD). Baca + validasi BPB.
 * 0 = ok, -1 = gagal (SD tak ada / bukan FAT32). */
int fat32_mount(void);

/* 1 bila sudah di-mount. */
int fat32_mounted(void);

/* path = "/sd/..." (absolut). mkdir: 0 ok / -1 gagal. */
int fat32_mkdir(const char *path);

/* Tulis file: buat baru atau truncate bila sudah ada, lalu tulis
 * `len` byte dari awal. Kembalikan byte tertulis / -1.
 * dst/src = buffer KERNEL atau VA user yang sudah divalidasi
 * pemanggil (pola sys_sd_read_user: tulis langsung ke VA user). */
int fat32_write_file(const char *path, const uint8_t *data, uint32_t len);
int fat32_read_file(const char *path, uint8_t *dst, uint32_t max);

/* Hapus file (bukan direktori). 0 ok / -1 gagal. */
int fat32_delete(const char *path);

/* Daftar isi direktori (path "/sd" atau "/sd/DIR"). Tulis
 * "NAMA.EXT\n" per file dan "NAMA/\n" per subdirektori (tanpa "."
 * dan ".."). Kembalikan jumlah entri / -1. */
int fat32_listdir(const char *path, char *dst, uint32_t max);

#endif /* _FAT32_H_ */
