/*
 * uipccli.c - IPC-1 test client (embed-hook spawn child).
 *
 * Second half of the IPC-1 round-trip proof; see
 * uipcsrv.c for the pair protocol (sequential spawn
 * runs, store-and-forward batches, coordination state
 * as a queued STATE message, id 0x5EED, body = two
 * counters "sent" and "got"). Batch is 15: the port
 * queue caps at 16 (MACH_PORT_QLIMIT_MAX) and one slot
 * is spent on the state message.
 *
 *   run 1 (setup): alloc the reply port (qlimit 16),
 *     run the phase's NEGATIVE PROBES on a throwaway
 *     probe port, send batch 0 (15 requests, each
 *     carrying a send-once right to the reply port),
 *     park the reply port, queue state(15, 0), exit.
 *   run k (work): claim the reply port; read state;
 *     verify the outstanding batch's replies byte-
 *     exact and in order; at 100 verified print the
 *     PASS marker; else send the next batch, re-park
 *     the port and queue the new state.
 *
 * Negative probes (plan IPC-1) -- each must return the
 * documented error code, never hang, never fault:
 *   recv on an empty queue        -> -5 UIPC_EEMPTY
 *   send past the queue limit (3) -> -4 UIPC_EFULL
 *   send to a destroyed port      -> -3 UIPC_EDEST
 *   body of 4097 B (cap is 4096)  -> -6 UIPC_E2BIG
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipccli".
 */
#include "ulib/uipc.h"

#define TOTAL	100u
#define BATCH	15u		/* 16 queue slots - 1 state */
#define SLOT	"uipccli"
#define SRVSLOT	"uipcsrv"
#define PROBE	"uipcprobe"
#define STATE_ID	0x5EED

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
	u_put("UIPCCLI: FAIL ");
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

/* Queue state(sent, got) to our own parked reply port.
 * The caller has just re-published the receive right. */
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

/* Read the state message that heads the queue. */
static void
state_recv(unsigned port, unsigned *sent, unsigned *got)
{
	int rc;
	rc = recv_wait(port);
	if (rc < 0)
		fail("state recv", rc);
	if (msg.id != STATE_ID ||
	    (unsigned)rc != UIPC_HDR_SIZE + 8u)
		fail("state header", msg.id);
	*sent = get_word(0u);
	*got = get_word(4u);
}

/* Send request batch [sent, sent+batch) to the server. */
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

/* The four negative probes, on a throwaway port parked
 * in the "uipcprobe" rendezvous slot. */
static void
probes(unsigned rport)
{
	int rc;
	unsigned x, sx, xr, i;

	/* Empty queue: recv on the fresh reply port. */
	rc = uipc_recv_poll(rport, &msg, UIPC_MSG_MAX);
	if (rc != UIPC_EEMPTY)
		fail("probe empty", rc);
	u_put("UIPCCLI: probe empty-queue rc=-5 OK\n");

	/* Full queue: probe port with qlimit 3. */
	rc = uipc_port_alloc();
	if (rc < 0)
		fail("probe alloc", rc);
	x = (unsigned)rc;
	rc = uipc_port_setqlimit(x, 3u);
	if (rc != 0)
		fail("probe setqlimit", rc);
	rc = uipc_test_publish(PROBE, x);
	if (rc != 0)
		fail("probe publish", rc);
	rc = uipc_test_resolve(PROBE);
	if (rc < 0)
		fail("probe resolve", rc);
	sx = (unsigned)rc;
	msg.reply = 0u;
	msg.dest = 0u;
	msg.id = 900;
	msg.size = UIPC_HDR_SIZE + 4u;
	for (i = 0; i < 4u; i++)
		msg.body[i] = 0x5Au;
	for (i = 0; i < 3u; i++) {
		rc = uipc_send(sx, &msg, UIPC_HDR_SIZE + 4u);
		if (rc != 0)
			fail("probe fill", rc);
	}
	rc = uipc_send(sx, &msg, UIPC_HDR_SIZE + 4u);
	if (rc != UIPC_EFULL)
		fail("probe full", rc);
	u_put("UIPCCLI: probe full-queue rc=-4 OK\n");

	/* Dead port: claim the receive right back, destroy
	 * the port, then send on the held send name. */
	rc = uipc_test_claim(PROBE);
	if (rc < 0)
		fail("probe claim", rc);
	xr = (unsigned)rc;
	rc = uipc_port_destroy(xr);
	if (rc != 0)
		fail("probe destroy", rc);
	rc = uipc_send(sx, &msg, UIPC_HDR_SIZE + 4u);
	if (rc != UIPC_EDEST)
		fail("probe dead", rc);
	u_put("UIPCCLI: probe dead-port rc=-3 OK\n");

	/* Oversize: 4097 B body against the 4096 cap. The
	 * size check precedes name resolution, so the dead
	 * send name is fine here. */
	rc = uipc_send(sx, &msg, UIPC_HDR_SIZE + UIPC_BODY_MAX + 1u);
	if (rc != UIPC_E2BIG)
		fail("probe oversize", rc);
	u_put("UIPCCLI: probe oversize rc=-6 OK\n");
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;
	unsigned rport, sent, got, i, j;

	rc = uipc_test_claim(SLOT);
	if (rc == UIPC_ENORV) {
		/* Setup run: reply port + probes + batch 0. */
		rc = uipc_port_alloc();
		if (rc < 0)
			fail("alloc", rc);
		rport = (unsigned)rc;
		rc = uipc_port_setqlimit(rport, 16u);
		if (rc != 0)
			fail("setqlimit", rc);
		probes(rport);
		send_batch(rport, 0u, BATCH);
		rc = uipc_test_publish(SLOT, rport);
		if (rc != 0)
			fail("publish", rc);
		state_send(BATCH, 0u);
		u_put("UIPCCLI: progress sent=15 got=0\n");
		sys_exit(0);
	}
	if (rc < 0)
		fail("claim", rc);
	rport = (unsigned)rc;

	state_recv(rport, &sent, &got);

	/* Verify the outstanding batch's replies, in order. */
	for (i = got; i < sent; i++) {
		unsigned bl = body_len(i);
		rc = recv_wait(rport);
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
		u_put("UIPCCLI: PASS replies=100/100\n");
		sys_exit(0);
	}

	/* Send the next batch, then park the reply port
	 * and queue the new state behind this run's work
	 * (the queue is empty again at that point). */
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
	u_put("UIPCCLI: progress sent=");
	putu(sent);
	u_put(" got=");
	putu(got);
	u_put("\n");
	sys_exit(0);
}
