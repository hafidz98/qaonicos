/*
 * user/qabotd.c -- Qabot daemon persisten (Q4).
 *
 * Loop interaktif: baca 1 baris prompt dari console -> jalankan harness
 * ReAct (qb_run_task, provider real) -> cetak respons.  Ekspresi wajah
 * Qabot di-drive via syscall 78 seiring fase kerja:
 *   LISTENING saat menunggu/mengetik prompt,
 *   THINKING  selama provider.chat() (LLM berpikir),
 *   SPEAKING  saat emit respons final,
 *   IDLE      saat selesai / menganggur.
 * Teks status tampil di wajah (face_set_status_text).
 *
 * Provider: mode ANONIM + TCP polos ke proxy penerjemah AI Horde di
 * host (10.0.2.2:18090 via slirp) — LLM gratis, tanpa API key, tanpa
 * login (Q4b/Q4c).  Konfirmasi tool (verdict CONFIRM) ditanyakan
 * interaktif ke user via console (Q4, hook confirm_fn).
 *
 * Daemon persisten: tidak pernah SYS_EXIT (pola face/uiapp).
 */
#include "ulib/ulib.h"
#include "qabot/qabot.h"

__attribute__((section(".text.start")))
void _start(void);

/* Ekspresi face (disalin dari user/face/face.h; qabotd tak link face). */
#define	QD_EXPR_IDLE		0u
#define	QD_EXPR_LISTENING	5u
#define	QD_EXPR_SPEAKING	6u
#define	QD_EXPR_THINKING	7u

#define	QD_PROXY_IP	0x0A000202u	/* 10.0.2.2 (host via slirp) */
#define	QD_PROXY_PORT	18090u		/* proxy penerjemah AI Horde */

#define	QD_LINE_MAX	200u

static struct qb_run		qd_run;
static struct qb_real_ctx	qd_ctx;
static char			qd_line[QD_LINE_MAX + 1u];

/* Cetak string NUL (helper lokal; puts ada di ulib). */
static void
qd_puts(const char *s)
{
	puts(s);
}

/* Tanya konfirmasi ke user via console: "boleh jalankan? [y/N]".
 * Kembalikan 1 bila user menjawab 'y'/'Y'. */
static int
qd_ask_confirm(const char *toolname, const char *reason)
{
	static char q[96];
	int c, ans = 0;

	/* Wajah: dengarkan jawaban user. */
	sys_face_expr(QD_EXPR_LISTENING, "konfirmasi?");

	(void)qb_snprintf(q, sizeof(q),
	    "qabot: tool '%s' butuh konfirmasi (%s). jalankan? [y/N] ",
	    toolname, reason);
	qd_puts(q);
	for (;;) {
		c = sys_read_console();
		if (c < 0) {
			sys_yield();
			continue;
		}
		if (c == 'y' || c == 'Y')
			ans = 1;
		/* Echo jawaban lalu newline. */
		{
			char ec[2];
			ec[0] = (char)c;
			ec[1] = '\n';
			sys_write(1, ec, 2u);
		}
		break;
	}
	return ans;
}

/* Baca satu baris dari console (echo, backspace). Kembalikan panjang.
 * Wajah sudah LISTENING oleh pemanggil. */
static unsigned
qd_readline(void)
{
	unsigned n = 0u;
	int c;
	char ec;

	for (;;) {
		c = sys_read_console();
		if (c < 0) {
			sys_yield();
			continue;
		}
		if (c == '\r' || c == '\n') {
			ec = '\n';
			sys_write(1, &ec, 1u);
			break;
		}
		if (c == 0x7fu || c == 0x08) {	/* backspace */
			if (n > 0) {
				n--;
				qd_puts("\b \b");
			}
			continue;
		}
		if (c < 0x20 || c > 0x7e)
			continue;	/* abaikan kontrol */
		if (n < QD_LINE_MAX) {
			qd_line[n++] = (char)c;
			ec = (char)c;
			sys_write(1, &ec, 1u);
		}
	}
	qd_line[n] = 0;
	return n;
}

void
_start(void)
{
	int r;
	unsigned n;

	qd_puts("qabotd: Qabot daemon persisten (Q4)\n");
	qd_puts("qabotd: ketik prompt lalu Enter; baris kosong = diam.\n");

	/* Provider anonim + TCP polos -> proxy AI Horde di host. */
	qb_real_init(&qd_ctx, "");	/* "" = anonim */
	qb_real_set_plain(1, QD_PROXY_IP, QD_PROXY_PORT, "qabot-proxy");

	for (;;) {
		sys_face_expr(QD_EXPR_LISTENING, "mendengarkan...");
		qd_puts("qabot> ");
		n = qd_readline();
		if (n == 0u)
			continue;

		sys_face_expr(QD_EXPR_THINKING, "berpikir...");
		qd_run.auto_confirm = 0;
		qd_run.confirm_fn = qd_ask_confirm;
		r = qb_run_task(&qd_run, qd_line, &qb_real_provider);

		sys_face_expr(QD_EXPR_SPEAKING, "berbicara...");
		qd_puts("qabot: ");
		qd_puts(qd_run.final);
		qd_puts("\n");
		{
			static char st[64];
			(void)qb_snprintf(st, sizeof(st),
			    "qabot: selesai (status=%d, steps=%u)\n",
			    r, qd_run.steps);
			qd_puts(st);
		}

		sys_face_expr(QD_EXPR_IDLE, "");
	}
}
