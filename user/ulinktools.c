/*
 * ulinktools.c - C6L-5 stub `tools` consumer (test-only).
 *
 * A spawn child with kernel-assigned progid 2 (the
 * qabotd daemon_images index, via the embed hook),
 * so it may claim the reserved IPC name `tools`.
 * It speaks the linkd -> tools protocol (see
 * user/uartproto/linkd.h): RPC id 0x3101, body =
 * tool NUL + args-json NUL; reply id echoes 0x3101,
 * body = result JSON NUL.
 *
 * Policy mirror (C6L-3 §5, from user/qabot/tools.c
 * + policy.c -- read, not copied; production qabotd
 * implements this same shape through its real gate):
 *   ALLOW: get_info (real guest uptime_ms),
 *     gpio_read (pin table: value = pin & 1).
 *   CONFIRM: gpio_write, file_write -> never
 *     executed, answer {"ok":false,"error":
 *     "needs_confirm"} (the confirm UX is ONB scope).
 *   BLOCK: everything else, including
 *     self_destruct (not in the registry; policy.c
 *     blocks unknown tools) -> {"ok":false,
 *     "error":"refused"}.
 *
 * Parks in blocking receive (30 s deadline); two
 * consecutive deadline wakes mean the client is
 * gone and the server exits idle (the uiptoolssrv
 * shape). It ALSO exits after serving 4 dispatches
 * ("ULINKTOOLS: done 4", exit 0): the shell's `run`
 * waits for the child, so a never-exiting server
 * would block the shell forever -- and with a live
 * driver (ulink5d) issuing TEST.SCRIPT every ~2 s,
 * the idle-exit never fires (C6L-5 third run: the
 * shell stayed blocked and every later check
 * failed). NOT in uprogs[]: test-only,
 * "embed:ulinktools". It must never run at the same
 * time as embed:uiptoolssrv -- both claim `tools`,
 * and the second claim gets EBUSY.
 */
#include "ulib/uipc.h"

#define	RECV_DEADLINE_MS	30000
#define	IDLE_EXIT		2
#define	TOOL_RPC_ID		0x3101u

static struct uipc_msg	msg;
static unsigned		port;
static unsigned		idle;
static unsigned		served;

static void
putu(unsigned v)
{
	char b[12];
	int i = 0;
	if (v == 0u) {
		u_put("0");
		return;
	}
	while (v != 0u) {
		b[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	}
	while (i > 0) {
		char s[2];
		s[0] = b[--i];
		s[1] = '\0';
		u_put(s);
	}
}

static void
puti(int rc)
{
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
}

static void
fail(const char *what, int rc)
{
	u_put("ULINKTOOLS: FAIL ");
	u_put(what);
	u_put(" rc=");
	puti(rc);
	u_put("\n");
	sys_exit(1);
}

static int
streq(const char *a, const char *b)
{
	unsigned i = 0u;

	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

/* Copy a NUL-terminated string from the request
 * body at byte off. Returns length, or -1. */
static int
rdstr(const unsigned char *b, unsigned blen, unsigned off,
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

static unsigned
wrstr(unsigned char *b, unsigned off, const char *s)
{
	unsigned i = 0u;

	while (s[i]) {
		b[off + i] = (unsigned char)s[i];
		i++;
	}
	b[off + i] = 0;
	return off + i + 1u;
}

/* Reply to the request in msg, reusing its buffer. */
static int
reply_now(unsigned len)
{
	unsigned once = msg.reply;
	int id = msg.id;

	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = id;
	msg.size = len;
	return uipc_reply(once, &msg, len);
}

/* Reply [result-json NUL] with the request id.
 * Counts served dispatches; after the 4th, the
 * server exits 0 ("done 4") so the waiting shell
 * regains control (see the header comment). The
 * exit happens AFTER the reply is sent: exiting
 * first would make linkd report tools_unavailable
 * for the 4th dispatch. */
static void
reply_json(const char *json)
{
	unsigned n = wrstr(msg.body, 0u, json);

	if (reply_now(UIPC_HDR_SIZE + n) != 0)
		fail("reply", -1);
	if (++served >= 4u) {
		u_put("ULINKTOOLS: done 4\n");
		sys_exit(0);
	}
}

/* Append a string to out at *n (byte loop). */
static void
cat_str(char *out, unsigned *n, const char *s)
{
	unsigned i = 0u;

	while (s[i]) {
		out[*n] = s[i];
		(*n)++;
		i++;
	}
	out[*n] = 0;
}

/* Append a decimal number to out at *n. */
static void
cat_num(char *out, unsigned *n, unsigned v)
{
	char tmp[12];
	unsigned i = 0u;

	if (v == 0u) {
		out[*n] = '0';
		(*n)++;
		out[*n] = 0;
		return;
	}
	while (v != 0u) {
		tmp[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	}
	while (i > 0u) {
		out[*n] = tmp[--i];
		(*n)++;
	}
	out[*n] = 0;
}

/* Parse {"pin":N} loosely: the first run of digits
 * after "pin". Returns -1 if none. */
static int
parse_pin(const char *args)
{
	unsigned i = 0u;

	while (args[i]) {
		if (args[i] == 'p' && args[i + 1u] == 'i' &&
		    args[i + 2u] == 'n') {
			unsigned j = i + 3u, v = 0u, got = 0u;

			while (args[j] == ' ' || args[j] == ':' ||
			       args[j] == '"' || args[j] == '\t')
				j++;
			while (args[j] >= '0' && args[j] <= '9') {
				v = v * 10u + (unsigned)(args[j] - '0');
				got = 1u;
				j++;
			}
			if (got)
				return (int)v;
			return -1;
		}
		i++;
	}
	return -1;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;

	rc = uipc_port_alloc();
	if (rc < 0)
		fail("alloc", rc);
	port = (unsigned)rc;
	rc = uipc_register("tools", port);
	if (rc != 0)
		fail("register tools", rc);
	u_put("ULINKTOOLS: registered tools\n");

	for (;;) {
		unsigned blen;
		int id;

		rc = uipc_recv(port, &msg, UIPC_MSG_MAX,
			       RECV_DEADLINE_MS);
		if (rc == UIPC_ETIME) {
			if (++idle >= IDLE_EXIT) {
				u_put("ULINKTOOLS: idle-exit\n");
				sys_exit(0);
			}
			continue;
		}
		if (rc < 0)
			fail("recv", rc);
		idle = 0;
		id = msg.id;
		blen = (unsigned)rc - UIPC_HDR_SIZE;
		if (id != (int)TOOL_RPC_ID) {
			u_put("ULINKTOOLS: FAIL unknown id\n");
			sys_exit(1);
		}
		{
			char tool[64], args[512];
			int tl, al;

			tl = rdstr(msg.body, blen, 0u, tool, 63u);
			if (tl < 0)
				fail("tool parse", -1);
			al = rdstr(msg.body, blen,
				   (unsigned)tl + 1u, args, 511u);
			if (al < 0)
				fail("args parse", -1);
			if (streq(tool, "get_info")) {
				char out[96];
				unsigned n = 0u;

				out[0] = 0;
				cat_str(out, &n,
					"{\"ok\":true,\"result\":{\"uptime_ms\":");
				cat_num(out, &n, sys_uptime());
				cat_str(out, &n, "}}");
				reply_json(out);
				u_put("ULINKTOOLS: get_info ok\n");
				continue;
			}
			if (streq(tool, "gpio_read")) {
				int pin = parse_pin(args);
				char out[96];
				unsigned n = 0u;

				if (pin < 0) {
					reply_json("{\"ok\":false,\"error\":\"ARG\"}");
					continue;
				}
				out[0] = 0;
				cat_str(out, &n,
					"{\"ok\":true,\"result\":{\"pin\":");
				cat_num(out, &n, (unsigned)pin);
				cat_str(out, &n, ",\"value\":");
				cat_num(out, &n, (unsigned)pin & 1u);
				cat_str(out, &n, "}}");
				reply_json(out);
				u_put("ULINKTOOLS: gpio_read ok\n");
				continue;
			}
			if (streq(tool, "gpio_write") ||
			    streq(tool, "file_write")) {
				reply_json("{\"ok\":false,\"error\":\"needs_confirm\"}");
				u_put("ULINKTOOLS: confirm\n");
				continue;
			}
			reply_json("{\"ok\":false,\"error\":\"refused\"}");
			u_put("ULINKTOOLS: refused\n");
		}
	}
}
