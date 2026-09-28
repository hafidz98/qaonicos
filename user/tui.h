/*
 * tui.h - Library text-UI minimal di atas console UART (Fase 17).
 *
 * Murni userspace: semua output lewat u_write(1). Satu frame dibangun
 * di buffer lalu di-flush sekaligus (satu syscall) agar tidak
 * ter-interleave dengan cetakan thread lain.
 *
 * Koordinat 1-based (ala ANSI), layar diasumsikan 80x24.
 * Box memakai karakter ASCII (+ - |) agar aman di semua terminal.
 */
#ifndef _TUI_H_
#define _TUI_H_

/* Mulai frame baru (kosongkan buffer). */
void tui_begin(void);

/* Kirim buffer frame ke console. */
void tui_flush(void);

/* Bersihkan layar + kursor ke (1,1). */
void tui_clear(void);

/* Pindah kursor ke baris r, kolom c (1-based). */
void tui_goto(unsigned r, unsigned c);

/* Tulis string / char / desimal unsigned ke buffer. */
void tui_puts(const char *s);
void tui_putc(char c);
void tui_putu(unsigned v);

/* Tulis string di posisi (r,c). */
void tui_at(unsigned r, unsigned c, const char *s);

/* Box: sudut (r,c), lebar w, tinggi h (termasuk border), judul
 * opsional (boleh 0). */
void tui_box(unsigned r, unsigned c, unsigned w, unsigned h,
             const char *title);

/* Bar kemajuan: "[####------]" lebar total w (termasuk kurung),
 * pct 0-100. Digambar di posisi kursor saat ini. */
void tui_bar(unsigned w, unsigned pct);

/* Status bar: satu baris penuh (80 kolom) video terbalik di baris r. */
void tui_status(unsigned r, const char *s);

#endif /* _TUI_H_ */
