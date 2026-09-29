/*
 * scr_llm.c - pengaturan agen/LLM via KV.* co-MCU (mock di QEMU).
 *
 * Provider: OPENROUTER / OPENAI / CUSTOM (disimpan KV llm_provider).
 * Model: teks bebas via textedit (KV llm_model).
 * API key: masked via textedit (KV llm_key -> NVS co-MCU di HW).
 *
 * JUJUR: daftar model canned untuk mock; di HW diisi dari HP.
 */
#include "ulib/ulib.h"
#include "screen.h"
#include "ui_draw.h"
#include "scr_textedit.h"
#include "uartproto/uartproto.h"

static const char *providers[3] = { "OPENROUTER", "OPENAI", "CUSTOM" };

static char	prov[16] = "OPENROUTER";
static char	model[33] = "AUTO";
static char	apikey[65] = "";
static int	have_key = 0;
static int	sel = 0;
static int	note_kind = 0;
static char	note_txt[48] = "";

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
kv_load(const char *k, char *v, unsigned vsz, const char *dflt)
{
	char resp[96];
	unsigned i = 0;
	if (uproto_cmd1("KV.GET", k, resp, sizeof(resp)) == 0) {
		while (resp[i] && i + 1 < vsz) {
			v[i] = resp[i];
			i++;
		}
		v[i] = 0;
	} else {
		while (dflt[i] && i + 1 < vsz) {
			v[i] = dflt[i];
			i++;
		}
		v[i] = 0;
	}
}

static void
llm_enter(void)
{
	char resp[96];
	sel = 0;
	note_kind = 0;
	kv_load("llm_provider", prov, sizeof(prov), "OPENROUTER");
	kv_load("llm_model", model, sizeof(model), "AUTO");
	have_key = (uproto_cmd1("KV.GET", "llm_key", resp,
				sizeof(resp)) == 0);
}

static void
prov_cycle(void)
{
	int i;
	char resp[64];
	for (i = 0; i < 3; i++)
		if (prov[0] == providers[i][0] && prov[1] == providers[i][1])
			break;
	i = (i + 1) % 3;
	{
		unsigned k = 0;
		while (providers[i][k] && k + 1 < sizeof(prov)) {
			prov[k] = providers[i][k];
			k++;
		}
		prov[k] = 0;
	}
	if (uproto_cmd2("KV.SET", "llm_provider", prov, resp,
			sizeof(resp)) != 0)
		set_note(2, "SIMPAN GAGAL");
}

static void
model_done(int confirmed)
{
	char resp[64];
	if (!confirmed)
		return;
	if (uproto_cmd2("KV.SET", "llm_model", model, resp,
			sizeof(resp)) != 0)
		set_note(2, "SIMPAN GAGAL");
	else
		set_note(1, "MODEL DISIMPAN");
}

static void
key_done(int confirmed)
{
	char resp[64];
	if (!confirmed)
		return;
	if (uproto_cmd2("KV.SET", "llm_key", apikey, resp,
			sizeof(resp)) != 0)
		set_note(2, "SIMPAN GAGAL");
	else {
		set_note(1, "API KEY DISIMPAN");
		have_key = 1;
	}
	/* bersihkan buffer RAM */
	{
		unsigned i;
		for (i = 0; i < sizeof(apikey); i++)
			apikey[i] = 0;
	}
}

static void
llm_event(int ev)
{
	if (note_kind) {
		note_kind = 0;
		return;
	}
	switch (ev) {
	case EV_UP:
		if (sel > 0)
			sel--;
		break;
	case EV_DOWN:
		if (sel < 2)
			sel++;
		break;
	case EV_OK:
		if (sel == 0) {
			prov_cycle();
		} else if (sel == 1) {
			textedit_begin("MODEL LLM", model, sizeof(model),
				       0, model_done);
			ui_push(&scr_textedit);
		} else {
			apikey[0] = 0;
			textedit_begin("API KEY", apikey, sizeof(apikey),
				       1, key_done);
			ui_push(&scr_textedit);
		}
		break;
	case EV_BACK:
		ui_pop();
		break;
	default:
		break;
	}
}

static void
llm_render(void)
{
	int i, y;
	char line[48];
	ui_text_center(22, "AGEN LLM", C_DIM);

	{
		const char *labels[3] = { "PROVIDER", "MODEL", "API KEY" };
		for (i = 0; i < 3; i++) {
			y = 60 + i * 44;
			if (i == sel) {
				ui_rect(16, y - 6, UI_W - 32, 38, C_SEL);
				ui_border(16, y - 6, UI_W - 32, 38, C_ACCENT);
			}
			ui_text(26, y - 2, labels[i], C_DIM);
			if (i == 0) {
				ui_text(26, y + 12, prov,
					i == sel ? C_ACCENT : C_FG);
			} else if (i == 1) {
				unsigned a = 0;
				while (model[a] && a + 1 < sizeof(line)) {
					line[a] = model[a];
					a++;
				}
				line[a] = 0;
				ui_text(26, y + 12, line,
					i == sel ? C_ACCENT : C_FG);
			} else {
				ui_text(26, y + 12,
					have_key ? "SUDAH DIISI" : "KOSONG",
					i == sel ? C_ACCENT :
					(have_key ? C_OK : C_WARN));
			}
		}
	}
	ui_text_center(208, "CO-MCU: MOCK (QEMU)", C_DIM);

	if (note_kind) {
		ui_rect(28, 140, UI_W - 56, 56, 0x08A5);
		ui_border(28, 140, UI_W - 56, 56,
			  note_kind == 2 ? C_WARN : C_OK);
		ui_text_center(162, note_txt,
			       note_kind == 2 ? C_WARN : C_OK);
	}
}

const screen_t scr_llm = {
	llm_enter, 0, llm_event, llm_render
};
