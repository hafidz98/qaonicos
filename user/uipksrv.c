/*
 * uipksrv.c - IPC-2 test server (embed-hook spawn child).
 *
 * The IPC-1 pair re-expressed on the BLOCKING receive
 * (docs/rencana-ipc/PLAN.md IPC-2): same store-and-
 * forward batch protocol as uipcsrv.c (sequential
 * spawn runs, coordination state as a queued STATE
 * message, batch 15 of the 16-slot queue, payload
 * contract identical), but every receive is
 * uipc_recv(..., 10000) -- a 10 s deadline instead of
 * the poll/yield loop. In the correct interleave the
 * messages are already queued, so the calls return at
 * once; the parking path itself is exercised by the
 * PARK PROBE at the end of every drain run: a blocking
 * receive with a 400 ms deadline on the just-emptied
 * queue must park and come back UIPC_ETIME, measured
 * with the guest clock.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipksrv". The rendezvous is deleted at IPC-4.
 */
#include "ulib/uipc.h"

#define TOTAL	100u
#define BATCH	15u		/* 16 queue slots - 1 state */
#define SLOT	"uipksrv"
#define STATE_ID	0x5EED
#define RECV_MS	10000		/* blocking deadline for work */
#define PROBE_MS	400		/* park-probe deadline */

static struct uipc_msg	msg;
static unsigned int	reply_names[BATCH];

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
	u_put("UIPKSRV: FAIL ");
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

/* Blocking receive one message (deadline RECV_MS).
 * In the correct interleave it is already queued. */
static int
recv_block(unsigned port)
{
	return uipc_recv(port, &msg, UIPC_MSG_MAX, RECV_MS);
}

/* Queue the state message (counter = served) to our
 * own parked port. */
static void
state_send(unsigned served)
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
	msg.body[0] = (unsigned char)(served & 0xFFu);
	msg.body[1] = (unsigned char)((served >> 8) & 0xFFu);
	msg.body[2] = (unsigned char)((served >> 16) & 0xFFu);
	msg.body[3] = (unsigned char)((served >> 24) & 0xFFu);
	msg.body[4] = 0u;
	msg.body[5] = 0u;
	msg.body[6] = 0u;
	msg.body[7] = 0u;
	rc = uipc_send(self, &msg, UIPC_HDR_SIZE + 8u);
	if (rc != 0)
		fail("state send", rc);
}

static unsigned
state_recv(unsigned port)
{
	int rc;
	rc = recv_block(port);
	if (rc < 0)
		fail("state recv", rc);
	if (msg.id != STATE_ID ||
	    (unsigned)rc != UIPC_HDR_SIZE + 8u)
		fail("state header", msg.id);
	return (unsigned)msg.body[0] |
	    ((unsigned)msg.body[1] << 8) |
	    ((unsigned)msg.body[2] << 16) |
	    ((unsigned)msg.body[3] << 24);
}

/* Park on the (empty) service queue with a short
 * deadline: must return UIPC_ETIME after roughly
 * PROBE_MS of guest time. This is the run's proof
 * that the blocking form really parks and really
 * expires -- the IPC-1 poll form returns EEMPTY at
 * once and fails here. */
static void
park_probe(unsigned port)
{
	unsigned t0, el;
	int rc;
	t0 = sys_uptime();
	rc = uipc_recv(port, &msg, UIPC_MSG_MAX, PROBE_MS);
	el = sys_uptime() - t0;
	if (rc != UIPC_ETIME)
		fail("park-probe rc", rc);
	if (el < 250u || el > 5000u)
		fail("park-probe elapsed", (int)el);
	u_put("UIPKSRV: park-probe ETIME elapsed=");
	putu(el);
	u_put(" OK\n");
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;
	unsigned port, served, batch, i, j;

	rc = uipc_test_claim(SLOT);
	if (rc == UIPC_ENORV) {
		/* Setup run: create + park the service port,
		 * then queue state(0) into it. */
		rc = uipc_port_alloc();
		if (rc < 0)
			fail("alloc", rc);
		port = (unsigned)rc;
		rc = uipc_port_setqlimit(port, 16u);
		if (rc != 0)
			fail("setqlimit", rc);
		rc = uipc_test_publish(SLOT, port);
		if (rc != 0)
			fail("publish", rc);
		state_send(0u);
		u_put("UIPKSRV: setup port published\n");
		sys_exit(0);
	}
	if (rc < 0)
		fail("claim", rc);
	port = (unsigned)rc;

	served = state_recv(port);
	batch = TOTAL - served;
	if (batch > BATCH)
		batch = BATCH;

	/* Drain the batch, verifying seq order + payload. */
	for (i = 0; i < batch; i++) {
		unsigned s = served + i;
		unsigned bl = body_len(s);
		rc = recv_block(port);
		if (rc < 0)
			fail("recv", rc);
		if ((unsigned)rc != UIPC_HDR_SIZE + bl ||
		    msg.id != (int)(0x1000u + s) ||
		    msg.size != UIPC_HDR_SIZE + bl)
			fail("header mismatch", (int)s);
		if (msg.reply == 0u)
			fail("no reply right", (int)s);
		for (j = 0; j < bl; j++)
			if (msg.body[j] != pat(s, j))
				fail("payload mismatch", (int)s);
		reply_names[i] = msg.reply;
	}

	/* Reply to the whole batch, in order. */
	for (i = 0; i < batch; i++) {
		unsigned s = served + i;
		unsigned bl = body_len(s);
		msg.reply = 0u;
		msg.dest = 0u;
		msg.id = (int)(0x2000u + s);
		msg.size = UIPC_HDR_SIZE + bl;
		for (j = 0; j < bl; j++)
			msg.body[j] = (unsigned char)(pat(s, j) ^ 0xA5u);
		rc = uipc_reply(reply_names[i], &msg,
				UIPC_HDR_SIZE + bl);
		if (rc != 0)
			fail("reply send", rc);
	}

	/* The queue is empty again: prove the park. */
	park_probe(port);

	served += batch;
	if (served >= TOTAL) {
		(void)uipc_port_destroy(port);
		u_put("UIPKSRV: PASS served=100/100\n");
		sys_exit(0);
	}
	rc = uipc_test_publish(SLOT, port);
	if (rc != 0)
		fail("re-publish", rc);
	state_send(served);
	u_put("UIPKSRV: served ");
	putu(served);
	u_put("/100\n");
	sys_exit(0);
}
