/*
 * scr_textedit.h - entri teks generik (password WiFi, API key).
 *
 * Pola pakai: textedit_begin(judul, buf, bufsz, masked, done);
 * ui_push(&scr_textedit);
 * done(confirmed) dipanggil saat OK/BACK (buf sudah final bila confirmed).
 *
 * Kontrol: ATAS/BAWAH = ganti huruf; KIRI = geser kiri (di posisi 0 =
 * hapus karakter terakhir); KANAN = geser kanan/tambah huruf;
 * OK = selesai; BACK = batal.
 *
 * Keterbatasan jujur v1: charset hanya huruf KAPITAL + angka + simbol
 * dasar (font 5x7 uiapp).  Provisioning utama tetap via BLE dari HP
 * (RENCANA-app §3); keyboard layar ini fallback.
 */
#ifndef SCR_TEXTEDIT_H
#define SCR_TEXTEDIT_H

#include "screen.h"

typedef void (*textedit_done_fn)(int confirmed);

void	textedit_begin(const char *title, char *buf, unsigned bufsz,
		       int masked, textedit_done_fn done);
extern const screen_t scr_textedit;

#endif /* SCR_TEXTEDIT_H */
