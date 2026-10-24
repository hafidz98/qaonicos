/*
 * uipkcli.c - IPC-2 test client (embed-hook spawn child).
 *
 * Second half of the IPC-2 round-trip proof; see
 * uipksrv.c for the pair protocol. Differences from
 * the IPC-1 client: replies are collected with the
 * BLOCKING uipc_recv(..., 10000), and the setup run
 * opens with the DEADLINE PROBE -- a blocking receive
 * with a 500 ms deadline on the fresh, silent reply
 * port must park and return UIPC_ETIME after roughly
 * 500 ms of guest time. (The IPC-1 negative probes
 * stay with verify-ipc1.py; they are poll-shaped and
 * unchanged.)
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipkcli".
 */
#include "ulib/uipc.h"

#define TOTAL	100u
#define BATCH	15u		/* 16 queue slots - 1 state */
#define SLOT	"uipkcli"
#define SRVSLOT	"uipksrv"
#define STATE_ID	0x5EED
#define RECV_MS	10000		/* blocking deadline for work */
#define PROBE_MS	500		/* deadline-probe timeout */

static struct uipc_msg	msg;

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
fail(const char *what, int rc)
{
	u_put("UIPKCLI: FAIL ");
	u_put(what);
	u_put(" rc=");
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
	u_put("\n");
	sys_exit(1);
}

static unsigned
body_len(unsigned s)
{
	if (s == 0u)
		return 4u;
	if (s == 50u)
		return UIPC_BODY_MAX;
	return 8u + ((s * 37u) % 62u) * 4u;	/* multiple of 4 */
}

static unsigned char
pat(unsigned s, unsigned j)
{
	return (unsigned char)((s * 31u + j) & 0xFFu);
}

static void
put_word(unsigned off, unsigned v)
{
	msg.body[off] = (unsigned char)(v & 0xFFu);
	msg.body[off + 1u] = (unsigned char)((v >> 8) & 0xFFu);
	msg.body[off + 2u] = (unsigned char)((v >> 16) & 0xFFu);
	msg.body[off + 3u] = (unsigned char)((v >> 24) & 0xFFu);
}

static unsigned
get_word(unsigned off)
{
	return (unsigned)msg.body[off] |
	    ((unsigned)msg.body[off + 1u] << 8) |
	    ((unsigned)msg.body[off + 2u] << 16) |
	    ((unsigned)msg.body[off + 3u] << 24);
}

static void
state_send(unsigned sent, unsigned got)
{
	int rc;
	unsigned self;
	rc = uipc_test_resolve(SLOT);
	if (rc < 0)
		fail("state resolve", rc);
	self = (unsigned)rc;
	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = STATE_ID;
	msg.size = UIPC_HDR_SIZE + 8u;
	put_word(0u, sent);
	put_word(4u, got);
	rc = uipc_send(self, &msg, UIPC_HDR_SIZE + 8u);
	if (rc != 0)
		fail("state send", rc);
}

static void
state_recv(unsigned port, unsigned *sent, unsigned *got)
{
	int rc;
	rc = uipc_recv(port, &msg, UIPC_MSG_MAX, RECV_MS);
	if (rc < 0)
		fail("state recv", rc);
	if (msg.id != STATE_ID ||
	    (unsigned)rc != UIPC_HDR_SIZE + 8u)
		fail("state header", msg.id);
	*sent = get_word(0u);
	*got = get_word(4u);
}

static void
send_batch(unsigned rport, unsigned sent, unsigned batch)
{
	int rc;
	unsigned i, j, srv;
	rc = uipc_test_resolve(SRVSLOT);
	if (rc < 0)
		fail("resolve server", rc);
	srv = (unsigned)rc;
	for (i = 0; i < batch; i++) {
		unsigned s = sent + i;
		unsigned bl = body_len(s);
		msg.reply = rport;
		msg.dest = 0u;
		msg.id = (int)(0x1000u + s);
		msg.size = UIPC_HDR_SIZE + bl;
		for (j = 0; j < bl; j++)
			msg.body[j] = pat(s, j);
		rc = uipc_send(srv, &msg, UIPC_HDR_SIZE + bl);
		if (rc != 0)
			fail("send", rc);
	}
}

/* Blocking receive on the silent reply port with a
 * 500 ms deadline: must park and return UIPC_ETIME
 * after roughly PROBE_MS of guest time. */
static void
deadline_probe(unsigned rport)
{
	unsigned t0, el;
	int rc;
	t0 = sys_uptime();
	rc = uipc_recv(rport, &msg, UIPC_MSG_MAX, PROBE_MS);
	el = sys_uptime() - t0;
	if (rc != UIPC_ETIME)
		fail("deadline-probe rc", rc);
	if (el < 350u || el > 5000u)
		fail("deadline-probe elapsed", (int)el);
	u_put("UIPKCLI: deadline-probe ETIME elapsed=");
	putu(el);
	u_put(" OK\n");
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;
	unsigned rport, sent, got, i, j;

	rc = uipc_test_claim(SLOT);
	if (rc == UIPC_ENORV) {
		/* Setup run: reply port + deadline probe +
		 * batch 0. */
		rc = uipc_port_alloc();
		if (rc < 0)
			fail("alloc", rc);
		rport = (unsigned)rc;
		rc = uipc_port_setqlimit(rport, 16u);
		if (rc != 0)
			fail("setqlimit", rc);
		deadline_probe(rport);
		send_batch(rport, 0u, BATCH);
		rc = uipc_test_publish(SLOT, rport);
		if (rc != 0)
			fail("publish", rc);
		state_send(BATCH, 0u);
		u_put("UIPKCLI: progress sent=15 got=0\n");
		sys_exit(0);
	}
	if (rc < 0)
		fail("claim", rc);
	rport = (unsigned)rc;

	state_recv(rport, &sent, &got);

	/* Verify the outstanding batch's replies, in
	 * order, collected with the blocking receive. */
	for (i = got; i < sent; i++) {
		unsigned bl = body_len(i);
		rc = uipc_recv(rport, &msg, UIPC_MSG_MAX, RECV_MS);
		if (rc < 0)
			fail("recv reply", rc);
		if ((unsigned)rc != UIPC_HDR_SIZE + bl ||
		    msg.id != (int)(0x2000u + i) ||
		    msg.size != UIPC_HDR_SIZE + bl)
			fail("reply header mismatch", (int)i);
		for (j = 0; j < bl; j++)
			if (msg.body[j] !=
			    (unsigned char)(pat(i, j) ^ 0xA5u))
				fail("reply payload mismatch", (int)i);
	}
	got = sent;

	if (got >= TOTAL) {
		(void)uipc_port_destroy(rport);
		u_put("UIPKCLI: PASS replies=100/100\n");
		sys_exit(0);
	}

	{
		unsigned batch = TOTAL - sent;
		if (batch > BATCH)
			batch = BATCH;
		send_batch(rport, sent, batch);
		sent += batch;
	}
	rc = uipc_test_publish(SLOT, rport);
	if (rc != 0)
		fail("re-publish", rc);
	state_send(sent, got);
	u_put("UIPKCLI: progress sent=");
	putu(sent);
	u_put(" got=");
	putu(got);
	u_put("\n");
	sys_exit(0);
}
