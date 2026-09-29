/*
 * scr_wifi.c - layar WiFi via protokol UART v1 (co-MCU mock di QEMU).
 *
 * Alur: STATUS -> PINDAI -> daftar SSID -> pilih -> (OPEN: CONNECT) /
 * (WPA2: PASSWORD via textedit -> CONNECT -> PASS) -> CONNECTED.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"
#include "scr_textedit.h"
#include "uartproto/uartproto.h"

#define MAXNET	8
#define SSID_SZ	32

static char	ssids[MAXNET][SSID_SZ];
static char	rssis[MAXNET][8];
static char	secs[MAXNET][8];
static int	nnet = 0;

static char	status_txt[48] = "MEMERIKSA...";
static int	connected = 0;

static int	state = 0;	/* 0=status, 1=list */
static int	sel = 0;
static int	note_kind = 0;	/* 0=tidak ada, 1=info, 2=error */
static char	note_txt[48] = "";

static char	pwd[33] = "";
static char	pending_ssid[SSID_SZ] = "";

static void
set_note(int kind, const char *t)
{
	unsigned i = 0;
	note_kind = kind;
	while (t[i] && i + 1 < sizeof(note_txt)) {
		note_txt[i] = t[i];
		i++;
	}
	note_txt[i] = 0;
}

static void
refresh_status(void)
{
	char resp[128];
	if (uproto_cmd("WIFI.STATUS", resp, sizeof(resp)) != 0) {
		set_note(2, "CO-MCU TAK RESPON");
		return;
	}
	if (resp[0] == 'C') {	/* CONNECTED <ssid> */
		unsigned i = 0, o = 0;
		while (resp[i] && resp[i] != ' ')
			i++;
		if (resp[i] == ' ')
			i++;
		while (resp[i] && o + 1 < sizeof(status_txt)) {
			status_txt[o++] = resp[i++];
			status_txt[o] = 0;
		}
		connected = 1;
	} else {
		unsigned i = 0;
		const char *d = "TERPUTUS";
		while (d[i]) {
			status_txt[i] = d[i];
			i++;
		}
		status_txt[i] = 0;
		connected = 0;
	}
}

/* Pecah "3\nSSID,RSSI,SEC\n..." ke tabel. */
static void
parse_scan(const char *resp)
{
	int i = 0, r;
	nnet = 0;
	/* lewati baris hitungan */
	while (resp[i] && resp[i] != '\n')
		i++;
	if (resp[i] == '\n')
		i++;
	while (resp[i] && nnet < MAXNET) {
		int c = 0;
		while (resp[i] && resp[i] != ',' && c + 1 < SSID_SZ)
			ssids[nnet][c++] = resp[i++];
		ssids[nnet][c] = 0;
		if (resp[i] == ',')
			i++;
		c = 0;
		while (resp[i] && resp[i] != ',' && c + 1 < 8)
			rssis[nnet][c++] = resp[i++];
		rssis[nnet][c] = 0;
		if (resp[i] == ',')
			i++;
		c = 0;
		while (resp[i] && resp[i] != '\n' && c + 1 < 8)
			secs[nnet][c++] = resp[i++];
		secs[nnet][c] = 0;
		if (resp[i] == '\n')
			i++;
		nnet++;
	}
	(void)r;
}

static void
wifi_enter(void)
{
	state = 0;
	sel = 0;
	note_kind = 0;
	refresh_status();
}

static void
do_scan(void)
{
	char resp[512];
	if (uproto_cmd("WIFI.SCAN", resp, sizeof(resp)) != 0) {
		set_note(2, "PINDAI GAGAL");
		return;
	}
	parse_scan(resp);
	if (nnet == 0) {
		set_note(1, "TAK ADA JARINGAN");
		return;
	}
	state = 1;
	sel = 0;
	note_kind = 0;
}

/* Dipanggil textedit saat password selesai. */
static void
pwd_done(int confirmed)
{
	char resp[128];
	if (!confirmed)
		return;
	if (uproto_cmd1("WIFI.CONNECT", pending_ssid, resp,
			sizeof(resp)) != 0) {
		set_note(2, "CONNECT GAGAL");
		return;
	}
	/* harap NEEDPASS */
	if (uproto_cmd1("WIFI.PASS", pwd, resp, sizeof(resp)) != 0) {
		set_note(2, "AUTH GAGAL");
		refresh_status();
		return;
	}
	set_note(1, "TERHUBUNG");
	refresh_status();
	pwd[0] = 0;
}

static void
connect_open(const char *ssid)
{
	char resp[128];
	if (uproto_cmd1("WIFI.CONNECT", ssid, resp, sizeof(resp)) != 0) {
		set_note(2, "CONNECT GAGAL");
		return;
	}
	set_note(1, "TERHUBUNG");
	refresh_status();
}

static void
wifi_event(int ev)
{
	unsigned i;
	if (note_kind) {
		note_kind = 0;
		return;
	}
	if (state == 0) {
		int nrow = connected ? 2 : 1;
		switch (ev) {
		case EV_UP:
			if (sel > 0)
				sel--;
			break;
		case EV_DOWN:
			if (sel < nrow - 1)
				sel++;
			break;
		case EV_OK:
			if (sel == 0) {
				do_scan();
			} else {
				char resp[64];
				if (uproto_cmd("WIFI.DISC", resp,
					       sizeof(resp)) == 0) {
					set_note(1, "TERPUTUS");
					refresh_status();
				}
			}
			break;
		case EV_BACK:
			ui_pop();
			break;
		default:
			break;
		}
		(void)i;
		return;
	}
	/* state == 1: daftar SSID */
	switch (ev) {
	case EV_UP:
		if (sel > 0)
			sel--;
		break;
	case EV_DOWN:
		if (sel < nnet - 1)
			sel++;
		break;
	case EV_OK: {
		int is_open = (secs[sel][0] == 'O');	/* OPEN */
		if (is_open) {
			connect_open(ssids[sel]);
		} else {
			unsigned k = 0;
			while (ssids[sel][k] && k + 1 < sizeof(pending_ssid)) {
				pending_ssid[k] = ssids[sel][k];
				k++;
			}
			pending_ssid[k] = 0;
			pwd[0] = 0;
			textedit_begin("PASSWORD WIFI", pwd, sizeof(pwd),
				       1, pwd_done);
			ui_push(&scr_textedit);
		}
		break;
	}
	case EV_BACK:
		state = 0;
		sel = 0;
		break;
	default:
		break;
	}
}

static void
wifi_render(void)
{
	int i, y;
	ui_text_center(22, "WIFI", C_DIM);

	if (state == 0) {
		ui_text_center(44, status_txt,
			       connected ? C_OK : C_DIM);
		{
			const char *rows[2] = { "PINDAI JARINGAN", "PUTUSKAN" };
			int nrow = connected ? 2 : 1;
			for (i = 0; i < nrow; i++) {
				y = 84 + i * 30;
				if (i == sel) {
					ui_rect(24, y - 4, UI_W - 48, 22, C_SEL);
					ui_border(24, y - 4, UI_W - 48, 22,
						  C_ACCENT);
				}
				ui_text_center(y, rows[i],
					       i == sel ? C_ACCENT : C_FG);
			}
		}
		ui_text_center(200, "CO-MCU: MOCK (QEMU)", C_DIM);
	} else {
		ui_text_center(40, "PILIH JARINGAN", C_DIM);
		for (i = 0; i < nnet; i++) {
			char line[48];
			unsigned a = 0, b = 0;
			y = 62 + i * 26;
			if (y > 200)
				break;
			while (ssids[i][a] && a + 1 < sizeof(line)) {
				line[a] = ssids[i][a];
				a++;
			}
			line[a++] = ' ';
			while (rssis[i][b] && a + 1 < sizeof(line))
				line[a++] = rssis[i][b++];
			line[a] = 0;
			if (i == sel) {
				ui_rect(16, y - 4, UI_W - 32, 22, C_SEL);
				ui_border(16, y - 4, UI_W - 32, 22, C_ACCENT);
			}
			ui_text(24, y, line, i == sel ? C_ACCENT : C_FG);
			ui_text(UI_W - 60, y, secs[i], C_DIM);
		}
	}

	if (note_kind) {
		ui_rect(28, 150, UI_W - 56, 56, 0x08A5);
		ui_border(28, 150, UI_W - 56, 56,
			  note_kind == 2 ? C_WARN : C_OK);
		ui_text_center(172, note_txt,
			       note_kind == 2 ? C_WARN : C_OK);
	}
}

const screen_t scr_wifi = {
	wifi_enter, 0, wifi_event, wifi_render
};
