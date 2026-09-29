/*
 * user/qabot/main.c -- Qabot harness v1: 3 skenario uji scripted.
 *
 * One-shot (pola umon): jalan di boot, cetak PASS/FAIL per skenario,
 * lalu sys_exit. Provider = mock in-process.
 */
#include "qabot.h"
#include "../ulib/ulib.h"

static const char *s1_script[] = {
	"TOOL:get_info",
	"FINAL:uptime terbaca",
};

static const char *s2_script[] = {
	"TOOL:gpio_write pin=5 val=1",
	"FINAL:gpio ditulis",
};

static const char *s3_script[] = {
	"TOOL:self_destruct target=all",
	"FINAL:ditolak",
};

static int
run_one(const char *tag, const char *prompt,
    const char **script, unsigned n)
{
	static struct qb_run	r;	/* ~13KB: BSS, bukan stack (stack user 1 page) */
	struct qb_mock_ctx	m;
	struct qb_provider	p;
	int			ok;

	r.nev = 0;
	r.auto_confirm = 1;	/* mode uji */
	qb_mock_init(&m, script, n);
	p.chat = qb_mock_provider.chat;
	p.ctx = &m;

	puts("QABOT: skenario ");
	puts(tag);
	puts(" mulai\n");
	qb_run_task(&r, prompt, &p);

	ok = 1;
	/* Syarat umum: selesai DONE dalam guard, event log urut. */
	if (r.status != QB_DONE)
		ok = 0;
	if (!qb_ev_contains(&r, "task mulai"))
		ok = 0;
	if (!qb_ev_contains(&r, "task selesai"))
		ok = 0;

	if (tag[1] == '1') {
		/* S1: get_info tereksekusi via ALLOW. */
		if (!qb_ev_contains(&r, "gate: get_info -> ALLOW"))
			ok = 0;
		if (!qb_ev_contains(&r, "exec: get_info -> uptime_ms="))
			ok = 0;
	} else if (tag[1] == '2') {
		/* S2: gpio_write lewat CONFIRM (auto-yes tercatat). */
		if (!qb_ev_contains(&r, "gate: gpio_write -> CONFIRM"))
			ok = 0;
		if (!qb_ev_contains(&r, "confirm: auto-yes"))
			ok = 0;
		if (!qb_ev_contains(&r, "exec: gpio_write -> pin=5"))
			ok = 0;
	} else {
		/* S3: self_destruct di-BLOCK, tidak ada eksekusi. */
		if (!qb_ev_contains(&r, "gate: self_destruct -> BLOCK"))
			ok = 0;
		if (qb_ev_contains(&r, "exec: self_destruct"))
			ok = 0;
	}

	/* Cetak event log skenario (observability). */
	{
		unsigned	e;
		char		ts[16];

		for (e = 0; e < r.nev; e++) {
			qb_snprintf(ts, sizeof ts, "[%u] ", r.ev[e].t_ms);
			puts(ts);
			puts(r.ev[e].text);
			puts("\n");
		}
	}

	puts("QABOT: ");
	puts(tag);
	puts(ok ? " PASS\n" : " FAIL\n");
	return ok;
}

int
main(void)
{	int	pass = 0;

	puts("QABOT: harness v1 (mock provider) mulai\n");
	pass += run_one("S1", "berapa uptime?",
	    s1_script, sizeof s1_script / sizeof s1_script[0]);
	pass += run_one("S2", "nyalakan gpio 5",
	    s2_script, sizeof s2_script / sizeof s2_script[0]);
	pass += run_one("S3", "hapus semua",
	    s3_script, sizeof s3_script / sizeof s3_script[0]);

	if (pass == 3)
		puts("QABOT: 3/3 PASS\n");
	else {
		char nb[32];

		qb_snprintf(nb, sizeof nb, "QABOT: FAIL %u/3\n",
		    (unsigned)pass);
		puts(nb);
	}
	sys_exit(pass == 3 ? 0 : 1);
	return 0;
}

/* Entry point: init.ld ENTRY(_start), section .text.start di-order pertama. */
__attribute__((section(".text.start")))
void
_start(void)
{
	main();
}
