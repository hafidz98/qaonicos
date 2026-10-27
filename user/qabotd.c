/*
 * user/qabotd.c -- Qabot daemon persisten (Q4).
 *
 * Loop interaktif: baca 1 baris prompt dari console -> jalankan harness
 * ReAct (qb_run_task, provider real) -> cetak respons.  Ekspresi wajah
 * Qabot di-drive via syscall 78 seiring fase kerja:
 *   IDLE      saat menganggur menunggu prompt (wajah dasbor),
 *   THINKING  selama provider.chat() (LLM berpikir),
 *   SPEAKING  saat emit respons final,
 *   LISTENING hanya untuk sesi suara (VOICE) -- bukan idle.
 * Teks status tampil di wajah (face_set_status_text).
 *
 * Provider: mode ANONIM + TCP polos ke proxy penerjemah AI Horde di
 * host (10.0.2.2:18090 via slirp) — LLM gratis, tanpa API key, tanpa
 * login (Q4b/Q4c).  Konfirmasi tool (verdict CONFIRM) ditanyakan
 * interaktif ke user via console (Q4, hook confirm_fn).
 *
 * Daemon persisten: tidak pernah SYS_EXIT (pola face/uiapp).
 *
 * K2: qabotd juga melayani IPC `tools` (nama reserved,
 * progid 2) untuk forward DISPATCH linkd -- lihat blok
 * "IPC `tools` service" di bawah. Loop utama adalah
 * poll loop (IPC + console interleave), bukan readline
 * blocking, agar servis IPC tidak menunggu prompt.
 */
#include "ulib/ulib.h"
#include "ulib/uipc.h"
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

/* --- IPC `tools` service (K2) ---
 *
 * qabotd claims the reserved IPC name `tools` through its
 * kernel-assigned spawn identity (daemon_images index 2,
 * D-IPC-2) and serves linkd's peer-dispatch forwards:
 * RPC id 0x3101, body = tool NUL + args-json NUL, reply
 * id echoes 0x3101, body = result JSON NUL (see
 * user/uartproto/linkd.h). The real policy gate decides
 * every call; NOTHING bypasses it:
 *   ALLOW   -> execute the real tool, result JSON wraps
 *              the tool's k=v output as a string.
 *   CONFIRM -> {"ok":false,"error":"needs_confirm"},
 *              NEVER executed. The peer path is
 *              unattended by definition, and in dev
 *              mode the console is sh-owned (reads
 *              return -1), so asking on the console
 *              would wedge; interactive confirmation
 *              stays in the console harness loop
 *              (qd_ask_confirm), and confirm UX is
 *              ONB scope (same as the C6L-5 stub).
 *   BLOCK (unknown tool, incl. self_destruct) ->
 *              {"ok":false,"error":"refused"}.
 *   Tool ran but failed (bad/missing args, backend
 *     failure) -> {"ok":false,"error":"tool_error"}.
 *   Malformed body -> {"ok":false,"error":"bad_args"}.
 *   Unknown RPC id -> refused (never exit, never hang;
 *     the stub FAILs here, a persistent daemon must
 *     not).
 *
 * Coexistence with the test stubs (same progid 2):
 * a stub REGISTER replaces this entry (same-owner
 * replace, IPC-4) while it lives, and IPC-3 teardown
 * clears alive when it dies. qabotd therefore
 * re-registers on a 5 s tick ONLY when a lookup of
 * `tools` fails -- it never steals a live entry,
 * whether its own port or a stub's. The transient
 * tools_unavailable gap between a stub's death and
 * the next tick is contract-correct (LINKD-CONTRACT:
 * unanswered -> tools_unavailable, never a hang).
 *
 * Single-threaded limitation (documented): while a
 * console task runs (qb_run_task, up to 30 s), IPC
 * is unserved and dispatches time out at linkd (5 s)
 * -> tools_unavailable. The verify dispatches while
 * qabotd is idle at the prompt.
 */
#define	QD_TOOL_RPC_ID		0x3101u
#define	QD_RECV_MS		100	/* IPC park slice, linkd shape */
#define	QD_TICK_MS		5000u	/* re-register tick */
#define	QD_EXEC_OUT_MAX		512u
#define	QD_JSON_MAX		1024u

static struct uipc_msg	qd_msg;
static unsigned		qd_tools_port;
static int		qd_tools_ok;

static int
qd_streq(const char *a, const char *b)
{
	unsigned i = 0u;

	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

/* Copy a NUL-terminated string from body at byte off.
 * Returns length, or -1. */
static int
qd_rdstr(const unsigned char *b, unsigned blen, unsigned off,
	 char *dst, unsigned cap)
{
	unsigned i = 0u;

	while (off + i < blen && i <= cap) {
		dst[i] = (char)b[off + i];
		if (b[off + i] == 0)
			return (int)i;
		i++;
	}
	dst[cap] = 0;
	return -1;
}

/* Reply to the request in qd_msg, reusing its buffer. */
static void
qd_reply_json(const char *json)
{
	unsigned once = qd_msg.reply;
	int id = qd_msg.id;
	unsigned n = 0u;

	while (json[n]) {
		qd_msg.body[n] = (unsigned char)json[n];
		n++;
	}
	qd_msg.body[n++] = 0;
	qd_msg.reply = 0u;
	qd_msg.dest = 0u;
	qd_msg.id = id;
	qd_msg.size = UIPC_HDR_SIZE + n;
	if (uipc_reply(once, &qd_msg, UIPC_HDR_SIZE + n) != 0)
		qd_puts("QABOTD: reply fail\n");
}

/* Append src to the JSON string field at out[*n],
 * escaping '"' and '\', dropping control chars.
 * Stops at cap (no overflow, truncates instead). */
static void
qd_json_cat(char *out, unsigned *n, const char *src, unsigned cap)
{
	unsigned i = 0u;

	while (src[i] && *n + 2u < cap) {
		char c = src[i++];
		if (c == '"' || c == '\\') {
			out[(*n)++] = '\\';
			out[(*n)++] = c;
		} else if (c >= 0x20 && c < 0x7f) {
			out[(*n)++] = c;
		}
		/* else: drop control/non-ASCII bytes */
	}
	out[*n] = 0;
}

/* Loose args-json parser: "key":number / "key":"str"
 * pairs into tc. Extra pairs past QB_ARGS_MAX are
 * ignored; a value longer than QB_ARGV_MAX-1 rejects
 * the whole call (bad_args, never silent truncation).
 * No escape handling in strings (documented limit;
 * dispatch args are capped at 192 bytes anyway). */
static int
qd_parse_args(const char *js, struct qb_toolcall *tc)
{
	unsigned i = 0u;

	tc->nargs = 0u;
	while (js[i]) {
		char key[QB_ARGK_MAX];
		char val[QB_ARGV_MAX];
		unsigned k = 0u, v = 0u;

		if (js[i] != '"') {
			i++;
			continue;
		}
		i++;
		while (js[i] && js[i] != '"' && k + 1u < QB_ARGK_MAX)
			key[k++] = js[i++];
		if (!js[i])
			return -1;
		key[k] = 0;
		i++;	/* closing quote */
		while (js[i] == ' ' || js[i] == '\t' || js[i] == ':')
			i++;
		if (js[i] == '"') {
			i++;
			while (js[i] && js[i] != '"' &&
			       v + 1u < QB_ARGV_MAX)
				val[v++] = js[i++];
			if (!js[i])
				return -1;
			val[v] = 0;
			i++;	/* closing quote */
		} else if (js[i] >= '0' && js[i] <= '9') {
			while (js[i] >= '0' && js[i] <= '9' &&
			       v + 1u < QB_ARGV_MAX)
				val[v++] = js[i++];
			if (js[i] >= '0' && js[i] <= '9')
				return -1;	/* overlong number */
			val[v] = 0;
		} else {
			/* Unknown value shape: skip the pair. */
			while (js[i] && js[i] != ',' && js[i] != '}')
				i++;
			continue;
		}
		if (tc->nargs < QB_ARGS_MAX) {
			unsigned j;
			for (j = 0u; j <= k; j++)
				tc->akey[tc->nargs][j] = key[j];
			for (j = 0u; j <= v; j++)
				tc->aval[tc->nargs][j] = val[j];
			tc->nargs++;
		}
	}
	return 0;
}

/* Serve one tools RPC in qd_msg (total len rc).
 * Replies exactly once, never exits. */
static void
qd_serve_dispatch(int rc)
{
	static char tool[64];
	static char args[512];
	static char exec_out[QD_EXEC_OUT_MAX];
	static char json[QD_JSON_MAX];
	unsigned blen = (unsigned)rc - UIPC_HDR_SIZE;
	struct qb_toolcall tc;
	const struct qb_tooldef *t;
	int tl, al, verdict;
	char reason[64];

	if (qd_msg.id != (int)QD_TOOL_RPC_ID) {
		qd_reply_json("{\"ok\":false,\"error\":\"refused\"}");
		qd_puts("QABOTD: dispatch ? refused\n");
		return;
	}
	tl = qd_rdstr(qd_msg.body, blen, 0u, tool, 63u);
	al = (tl >= 0) ? qd_rdstr(qd_msg.body, blen,
				  (unsigned)tl + 1u, args, 511u) : -1;
	if (tl < 0 || al < 0) {
		qd_reply_json("{\"ok\":false,\"error\":\"bad_args\"}");
		qd_puts("QABOTD: dispatch ? bad_args\n");
		return;
	}
	/* Tool name into the call struct (cap 31; a
	 * longer name cannot match a registry entry,
	 * so truncation cannot fake a tool). */
	{
		unsigned i;
		for (i = 0u; i < 31u && tool[i]; i++)
			tc.name[i] = tool[i];
		tc.name[i] = 0;
	}
	if (qd_parse_args(args, &tc) != 0) {
		qd_reply_json("{\"ok\":false,\"error\":\"bad_args\"}");
		qd_puts("QABOTD: dispatch ? bad_args\n");
		return;
	}
	t = qb_tool_find(tc.name);
	if (!t) {
		/* Unknown tool = BLOCK (policy.c). */
		qd_reply_json("{\"ok\":false,\"error\":\"refused\"}");
		qd_puts("QABOTD: dispatch ");
		qd_puts(tc.name);
		qd_puts(" refused\n");
		return;
	}
	verdict = qb_policy_check(tc.name, &tc, reason, sizeof(reason));
	if (verdict == QB_CONFIRM) {
		qd_reply_json(
		    "{\"ok\":false,\"error\":\"needs_confirm\"}");
		qd_puts("QABOTD: dispatch ");
		qd_puts(tc.name);
		qd_puts(" needs_confirm\n");
		return;
	}
	if (verdict == QB_BLOCK) {
		qd_reply_json("{\"ok\":false,\"error\":\"refused\"}");
		qd_puts("QABOTD: dispatch ");
		qd_puts(tc.name);
		qd_puts(" refused\n");
		return;
	}
	/* ALLOW: run the real tool. */
	if (t->exec(&tc, exec_out, sizeof(exec_out)) != 0) {
		qd_reply_json("{\"ok\":false,\"error\":\"tool_error\"}");
		qd_puts("QABOTD: dispatch ");
		qd_puts(tc.name);
		qd_puts(" tool_error\n");
		return;
	}
	{
		unsigned n = 0u;
		const char *pre = "{\"ok\":true,\"result\":\"";
		const char *post = "\"}";

		while (*pre)
			json[n++] = *pre++;
		qd_json_cat(json, &n, exec_out, QD_JSON_MAX - 4u);
		while (*post)
			json[n++] = *post++;
		json[n] = 0;
	}
	qd_reply_json(json);
	qd_puts("QABOTD: dispatch ");
	qd_puts(tc.name);
	qd_puts(" ok\n");
}

/* (Re-)register `tools` on qd_tools_port. Returns 0 on
 * success. Prints a marker only on success or on the
 * first failure path (callers throttle repeats). */
static int
qd_tools_register(void)
{
	int rc = uipc_register("tools", qd_tools_port);

	if (rc == 0) {
		qd_tools_ok = 1;
		return 0;
	}
	qd_tools_ok = 0;
	return rc;
}

void
_start(void)
{
	int r;
	int rc;
	unsigned n = 0u;	/* console line length */
	int prompt_shown = 0;
	unsigned last_tick;

	qd_puts("qabotd: Qabot daemon persisten (Q4)\n");
	qd_puts("qabotd: ketik prompt lalu Enter; baris kosong = diam.\n");

	/* Provider anonim + TCP polos -> proxy AI Horde di host. */
	qb_real_init(&qd_ctx, "");	/* "" = anonim */
	qb_real_set_plain(1, QD_PROXY_IP, QD_PROXY_PORT, "qabot-proxy");

	/* K2: claim the reserved `tools` name through the
	 * kernel-assigned spawn identity (progid 2). A
	 * failure degrades to console-only (the tick
	 * below keeps retrying); it must never kill
	 * this persistent daemon. */
	rc = uipc_port_alloc();
	if (rc < 0) {
		qd_puts("QABOTD: FAIL port alloc\n");
	} else {
		qd_tools_port = (unsigned)rc;
		if (qd_tools_register() == 0)
			qd_puts("QABOTD: registered tools\n");
		else
			qd_puts("QABOTD: FAIL register tools\n");
	}
	last_tick = sys_uptime();

	/* Poll loop (replaces the old blocking readline):
	 * IPC service + console input interleave. In dev
	 * mode the console is sh-owned, so console reads
	 * return -1 and the loop idles on IPC -- the
	 * observable behaviour (one "qabot> " prompt,
	 * face IDLE) is unchanged. The 100 ms IPC park
	 * keeps the FND-3 watchdog fed (park/yield,
	 * never a 30 s spin). */
	for (;;) {
		unsigned now;
		int c;
		int line_done = 0;

		/* Re-register tick: recover the name after
		 * a stub's death, never steal a live entry. */
		now = sys_uptime();
		if (now - last_tick >= QD_TICK_MS) {
			last_tick = now;
			if (qd_tools_port != 0u &&
			    uipc_lookup("tools") < 0 &&
			    qd_tools_register() == 0)
				qd_puts("QABOTD: tools re-registered\n");
		}

		/* IPC service (when registered). */
		if (qd_tools_ok) {
			rc = uipc_recv(qd_tools_port, &qd_msg,
				       UIPC_MSG_MAX, QD_RECV_MS);
			if (rc > 0)
				qd_serve_dispatch(rc);
			else if (rc < 0 && rc != UIPC_ETIME)
				qd_puts("QABOTD: recv error\n");
		}

		/* Console drain: non-blocking chars into the
		 * line buffer (echo, backspace -- the old
		 * readline semantics); a newline completes
		 * the line and runs the task below. */
		while ((c = sys_read_console()) >= 0) {
			char ec;

			if (c == '\r' || c == '\n') {
				ec = '\n';
				sys_write(1, &ec, 1u);
				line_done = 1;
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
		if (line_done) {
			qd_line[n] = 0;
			n = 0u;
			prompt_shown = 0;
			if (qd_line[0] != 0) {
				sys_face_expr(QD_EXPR_THINKING,
				    "berpikir...");
				qd_run.auto_confirm = 0;
				qd_run.confirm_fn = qd_ask_confirm;
				r = qb_run_task(&qd_run, qd_line,
				    &qb_real_provider);

				sys_face_expr(QD_EXPR_SPEAKING,
				    "berbicara...");
				qd_puts("qabot: ");
				qd_puts(qd_run.final);
				qd_puts("\n");
				{
					static char st[64];
					(void)qb_snprintf(st, sizeof(st),
					    "qabot: selesai (status=%d, "
					    "steps=%u)\n",
					    r, qd_run.steps);
					qd_puts(st);
				}
			}
			/* Empty line = silent (old behaviour). */
		}
		if (!prompt_shown) {
			/* Idle = the dashboard face (eyes).
			 * LISTENING is reserved for real voice
			 * sessions, not for waiting on a typed
			 * prompt (Boot UX feedback 2026-10-21). */
			sys_face_expr(QD_EXPR_IDLE, "");
			qd_puts("qabot> ");
			prompt_shown = 1;
		}
		sys_yield();
	}
}
