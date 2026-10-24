/*
 * uipcsrv.c - IPC-1 test server (embed-hook spawn child).
 *
 * One half of the IPC-1 round-trip proof. The pair runs
 * SEQUENTIALLY (the spawn slot holds one child at a
 * time and SYS_SPAWN parks the parent), so the
 * conversation is store-and-forward through real Mach
 * port queues, one batch per spawn run:
 *
 *   run 1 (setup): alloc the service port (queue limit
 *     raised to the Mach maximum, 16), park its receive
 *     right in the kernel test rendezvous (uipc.h),
 *     queue a STATE message to itself (below), exit.
 *   run k (drain): claim the receive right back, read
 *     the state message, receive one batch of
 *     requests, verify sequence + payload byte-exact,
 *     reply to each on the send-once right its header
 *     carried, re-park the port, queue the next state
 *     message -- or, at 100 served, destroy the port
 *     and print the PASS marker.
 *
 * Coordination state rides IN the queue as a message
 * with id 0x5EED whose body is two counters. It is
 * queued while the queue is otherwise empty, so it is
 * always the first message out on the next run. (The
 * ramfs counter files used by other test pairs are
 * not an option here: ramfs fd tables are keyed by
 * task and never reclaimed -- RAMFS_MAX_TASKS is 16
 * and every spawn run is a fresh task. Noted in
 * DEVLOG-IPC1 as a pre-existing gap.) One queue slot
 * is therefore spent on state, so a batch is 15 of
 * the 16 queue slots; 100 messages = 7 batches
 * (15*6 + 10).
 *
 * Payload contract (the client computes the same):
 *   seq s = 0..99, id = 0x1000 + s,
 *   body length: s=0 -> 4, s=50 -> 4096 (the inline
 *     cap), else 8 + ((s*37) % 62) * 4,
 *   body byte j = (s*31 + j) & 0xFF.
 *   Reply: id = 0x2000 + s, same length, byte ^ 0xA5.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcsrv". The rendezvous is deleted at
 * IPC-4 in favour of the registry.
 */
#include "ulib/uipc.h"

#define TOTAL	100u
#define BATCH	15u		/* 16 queue slots - 1 state */
#define SLOT	"uipcsrv"
#define STATE_ID	0x5EED

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
	u_put("UIPCSRV: FAIL ");
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

/* Poll-receive one message; in the correct interleave
 * it is already queued, so the first poll normally
 * succeeds. The bound keeps a broken peer from hanging
 * this child (the FND-3 watchdog is the backstop). */
static int
recv_wait(unsigned port)
{
	unsigned tries;
	int rc = UIPC_EEMPTY;
	for (tries = 0; tries < 200000u; tries++) {
		rc = uipc_recv_poll(port, &msg, UIPC_MSG_MAX);
		if (rc != UIPC_EEMPTY)
			return rc;
		sys_yield();
	}
	return rc;
}

/* Queue the state message (counter = served) to our
 * own parked port. The caller has just re-published
 * the receive right, so RESOLVE can mint a send name. */
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

/* Read the state message that heads the queue. */
static unsigned
state_recv(unsigned port)
{
	int rc;
	rc = recv_wait(port);
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
		u_put("UIPCSRV: setup port published\n");
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
		rc = recv_wait(port);
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

	served += batch;
	if (served >= TOTAL) {
		(void)uipc_port_destroy(port);
		u_put("UIPCSRV: PASS served=100/100\n");
		sys_exit(0);
	}
	rc = uipc_test_publish(SLOT, port);
	if (rc != 0)
		fail("re-publish", rc);
	state_send(served);
	u_put("UIPCSRV: served ");
	putu(served);
	u_put("/100\n");
	sys_exit(0);
}
