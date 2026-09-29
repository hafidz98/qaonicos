/*
 * user/uqabotr.c -- Uji provider real Qabot Q2c (daemon sementara).
 *
 * Menjalankan qb_run_task() dengan provider HTTPS+JSON (provider_real.c)
 * melawan mock server OpenAI-compatible di host (10.0.2.2:18444, TLS,
 * cert dari CA test yang sama). Skenario: prompt "baca pin 40" -> server
 * membalas tool_calls gpio_read -> qabot mengeksekusi tool REAL ->
 * hasil dikirim balik -> server membalas content final.
 *
 * Hasil: "QABOT: PROVIDER_REAL PASS" / "QABOT: PROVIDER_REAL FAIL (...)".
 * SEMENTARA: dihapus/dinonaktifkan setelah verifikasi (pola Q2b).
 */
#include "ulib/ulib.h"
#include "qabot/qabot.h"

__attribute__((section(".text.start")))
void _start(void);

/* Jam fake untuk uji (sama seperti utlscli; cert valid 2026-09-29+). */
#define	FAKE_NOW	1790683200u

static struct qb_run		qr_run;
static struct qb_real_ctx	qr_ctx;

void
_start(void)
{
	int r;

	sys_time_set(FAKE_NOW);
	qb_real_init(&qr_ctx, 0);	/* 0 = baca NVS llm.key / fallback */

	qr_run.auto_confirm = 1;	/* mode uji: CONFIRM auto-yes */
	r = qb_run_task(&qr_run, "baca pin 40", &qb_real_provider);

	if (r == QB_DONE &&
	    qb_ev_contains(&qr_run, "gpio_read") &&
	    qb_ev_contains(&qr_run, "Pin 40 terbaca")) {
		puts("QABOT: PROVIDER_REAL PASS\n");
	} else {
		static char fb[64];
		unsigned e;
		qb_snprintf(fb, sizeof(fb),
		    "QABOT: PROVIDER_REAL FAIL (status=%d)\n", r);
		puts(fb);
		puts("--- event log ---\n");
		for (e = 0; e < qr_run.nev; e++) {
			puts(qr_run.ev[e].text);
			puts("\n");
		}
		puts("--- end ---\n");
	}
	for (;;)
		sys_yield();
}
