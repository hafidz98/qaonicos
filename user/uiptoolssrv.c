/*
 * uiptoolssrv.c - IPC-4 mock "tools" server (embed-hook
 * spawn child, kernel-assigned progid 2 -- the qabotd
 * daemon_images index, so it may claim the reserved
 * name `tools`; the same binary without the override
 * must get UIPC_EPERM, which the harness proves from
 * sh). It answers the RPC methods the C6L-3 link ->
 * qabotd surface will consume:
 *   echo (id 0x3000+s): reply = request body XOR 0xA5,
 *     same id -- the byte-exact round-trip.
 *   dispatch (id 0x3100): structured result --
 *     request body [0xD15, arg], reply [0, 3,
 *     arg+1, arg+2, arg+3] -- the C6L-3 shape (a
 *     dispatch returning a structured result the
 *     caller validates).
 *   double (id 0x3200): reply twice; the second reply
 *     on the consumed send-once name must fail.
 *   die (id 0x3300): exit WITHOUT replying -- the
 *     client must get UIPC_EPEER, not a hang.
 *   stop (id 0x3400): reply OK, then exit.
 * The server parks in blocking recv (30 s deadline)
 * and counts message-wakes vs deadline-wakes: every
 * client RPC send must wake it (the first true
 * concurrent send-wake proof in this track), so
 * deadline-wakes must stay 0 across the flow. Two
 * consecutive deadline-wakes mean the client is gone
 * and the server exits idle.
 *
 * NOT in uprogs[]: test-only, "embed:uiptoolssrv".
 */
#include "ulib/uipc.h"

#define RECV_DEADLINE_MS	30000
#define IDLE_EXIT		2

static struct uipc_msg	msg;
static unsigned		port;
static unsigned		msg_wakes;
static unsigned		etime_wakes;
static unsigned		idle;
static unsigned		echo_n;

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
	u_put("UIPTOOLSSRV: FAIL ");
	u_put(what);
	u_put(" rc=");
	puti(rc);
	u_put("\n");
	sys_exit(1);
}

static void
wakes_line(void)
{
	u_put("UIPTOOLSSRV: wakes msg=");
	putu(msg_wakes);
	u_put(" etime=");
	putu(etime_wakes);
	u_put("\n");
}

/* Reply to the request in msg. The reply buffer is msg
 * itself (the request body is consumed first where the
 * reply depends on it). len is the TOTAL reply size. */
static int
reply_now(unsigned len)
{
	struct uipc_msg *r = &msg;
	unsigned once = r->reply;
	int id = r->id;

	r->reply = 0u;
	r->dest = 0u;
	r->id = id;
	r->size = len;
	return uipc_reply(once, r, len);
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
	rc = uipc_port_setqlimit(port, 16u);
	if (rc != 0)
		fail("qlimit", rc);
	rc = uipc_register("tools", port);
	if (rc != 0)
		fail("register tools", rc);
	u_put("UIPTOOLSSRV: registered tools\n");

	for (;;) {
		unsigned blen, j;
		int id;

		rc = uipc_recv(port, &msg, UIPC_MSG_MAX,
			       RECV_DEADLINE_MS);
		if (rc == UIPC_ETIME) {
			etime_wakes++;
			if (++idle >= IDLE_EXIT) {
				wakes_line();
				u_put("UIPTOOLSSRV: idle-exit\n");
				sys_exit(0);
			}
			continue;
		}
		if (rc < 0)
			fail("recv", rc);
		msg_wakes++;
		idle = 0;
		id = msg.id;
		blen = (unsigned)rc - UIPC_HDR_SIZE;

		if (id >= 0x3000 && id < 0x3100) {
			/* echo: body XOR 0xA5, same id */
			for (j = 0; j < blen; j++)
				msg.body[j] ^= 0xA5u;
			rc = reply_now((unsigned)rc);
			if (rc != 0)
				fail("echo reply", rc);
			echo_n++;
			if (echo_n == 25u || echo_n == 50u ||
			    echo_n == 75u || echo_n == 100u) {
				u_put("UIPTOOLSSRV: echo ");
				putu(echo_n);
				u_put("\n");
			}
			continue;
		}
		if (id == 0x3100) {
			/* dispatch: [0xD15, arg] -> [0, 3,
			 * arg+1, arg+2, arg+3] */
			unsigned arg, *w = (unsigned *)msg.body;

			if (blen < 8u || w[0] != 0xD15u)
				fail("dispatch req", (int)blen);
			arg = w[1];
			w[0] = 0u;
			w[1] = 3u;
			w[2] = arg + 1u;
			w[3] = arg + 2u;
			w[4] = arg + 3u;
			rc = reply_now(UIPC_HDR_SIZE + 20u);
			if (rc != 0)
				fail("dispatch reply", rc);
			u_put("UIPTOOLSSRV: dispatch OK\n");
			continue;
		}
		if (id == 0x3200) {
			/* double: reply OK, then reply again
			 * on the consumed send-once name.
			 * reply_now() zeroes msg.reply, so the
			 * once name is saved first. */
			unsigned once = msg.reply;

			msg.body[0] = 'O';
			msg.body[1] = 'K';
			rc = reply_now(UIPC_HDR_SIZE + 8u);
			if (rc != 0)
				fail("double reply1", rc);
			msg.reply = 0u;
			msg.dest = 0u;
			msg.id = id;
			msg.size = UIPC_HDR_SIZE + 8u;
			rc = uipc_reply(once, &msg,
					UIPC_HDR_SIZE + 8u);
			u_put("UIPTOOLSSRV: double2 rc=");
			puti(rc);
			u_put("\n");
			if (rc == 0)
				fail("double reply2 succeeded", rc);
			continue;
		}
		if (id == 0x3300) {
			/* die: exit WITHOUT replying */
			wakes_line();
			sys_exit(0);
		}
		if (id == 0x3400) {
			/* stop: reply OK, then exit */
			msg.body[0] = 'O';
			msg.body[1] = 'K';
			rc = reply_now(UIPC_HDR_SIZE + 8u);
			if (rc != 0)
				fail("stop reply", rc);
			wakes_line();
			sys_exit(0);
		}
		fail("unknown id", id);
	}
}
