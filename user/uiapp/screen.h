/*
 * screen.h - interface layar uiapp (RENCANA-app §2.2).
 *
 * Setiap layar (menu, monitor, settings, power) implement struct ini.
 * Screen manager = stack sederhana: ui_push / ui_pop / ui_reset.
 * Event: EV_* dari ulib.h (0..7), EV_TICK untuk animasi berkala.
 */
#ifndef UIAPP_SCREEN_H
#define UIAPP_SCREEN_H

typedef struct screen {
	void (*enter)(void);		/* dipanggil saat push */
	void (*exit)(void);		/* dipanggil saat pop */
	void (*on_event)(int ev);	/* input */
	void (*render)(void);		/* gambar ke strip aktif */
} screen_t;

void	ui_push(const screen_t *s);
void	ui_pop(void);
void	ui_reset(const screen_t *s);	/* pop semua, push s */
int	ui_depth(void);
const screen_t *ui_top(void);

/* Minta main loop melepas token display (kembali ke Qabot). */
void	ui_request_release(void);
int	ui_release_requested(void);
void	ui_clear_release(void);

#endif /* UIAPP_SCREEN_H */
