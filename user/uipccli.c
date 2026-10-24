/*
 * uipccli.c - IPC-1 test client (embed-hook spawn child).
 *
 * Second half of the IPC-1 round-trip proof; see
 * uipcsrv.c for the pair protocol (sequential spawn
 * runs, store-and-forward batches of 16, ramfs state).
 * Client state is /ipc1.cli: "sent got" ASCII counters.
 *
 *   run 1 (setup): alloc the reply port (qlimit 16),
 *     run the phase's NEGATIVE PROBES on a throwaway
 *     probe port, write state, send batch 0 (16
 *     requests, each carrying a send-once right to the
 *     reply port), park the reply port, exit.
 *   run k (work): claim the reply port; verify the
 *     replies for the outstanding batch byte-exact and
 *     in order; at 100 verified print the PASS marker;
 *     else send the next batch and park the port again.
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
#define BATCH	16u
#define SLOT	"uipccli"
#define SRVSLOT	"uipcsrv"
#define PROBE	"uipcprobe"
#define STATE	"/ipc1.cli"

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
	return 8u + (s * 37u) % 251u;
}

static unsigned char
pat(unsigned s, unsigned j)
{
	return (unsigned char)((s * 31u + j) & 0xFFu);
}

static void
state_write(unsigned sent, unsigned got)
{
	char b[24], t[12];
	int i = 0, n, fd;
	unsigned v = sent;
	if (v == 0u)
		b[i++] = '0';
	else {
		n = 0;
		while (v != 0u) {
			t[n++] = (char)('0' + (int)(v % 10u));
			v /= 10u;
		}
		while (n > 0)
			b[i++] = t[--n];
	}
	b[i++] = ' ';
	v = got;
	if (v == 0u)
		b[i++] = '0';
	else {
		n = 0;
		while (v != 0u) {
			t[n++] = (char)('0' + (int)(v % 10u));
			v /= 10u;
		}
		while (n > 0)
			b[i++] = t[--n];
	}
	b[i] = '\0';
	fd = u_open(STATE, O_CREAT | O_RDWR);
	if (fd < 0)
		fail("state open", fd);
	(void)u_write((unsigned)fd, b, (unsigned)i);
	(void)u_close(fd);
}

static int
state_read(unsigned *sent, unsigned *got)
{
	char b[24];
	int fd, n, i = 0;
	unsigned v = 0u;
	fd = u_open(STATE, O_RDONLY);
	if (fd < 0)
		return -1;
	n = u_read(fd, b, sizeof(b) - 1u);
	(void)u_close(fd);
	if (n <= 0)
		return -1;
	while (i < n && b[i] >= '0' && b[i] <= '9') {
		v = v * 10u + (unsigned)(b[i] - '0');
		i++;
	}
	*sent = v;
	while (i < n && b[i] == ' ')
		i++;
	v = 0u;
	while (i < n && b[i] >= '0' && b[i] <= '9') {
		v = v * 10u + (unsigned)(b[i] - '0');
		i++;
	}
	*got = v;
	return 0;
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
		rc = uipc_port_setqlimit(rport, BATCH);
		if (rc != 0)
			fail("setqlimit", rc);
		probes(rport);
		send_batch(rport, 0u, BATCH);
		state_write(BATCH, 0u);
		rc = uipc_test_publish(SLOT, rport);
		if (rc != 0)
			fail("publish", rc);
		u_put("UIPCCLI: progress sent=16 got=0\n");
		sys_exit(0);
	}
	if (rc < 0)
		fail("claim", rc);
	rport = (unsigned)rc;

	if (state_read(&sent, &got) != 0)
		fail("state read", -1);

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
		(void)sys_delete(STATE);
		u_put("UIPCCLI: PASS replies=100/100\n");
		sys_exit(0);
	}

	/* Send the next batch, then park the reply port. */
	{
		unsigned batch = TOTAL - sent;
		if (batch > BATCH)
			batch = BATCH;
		send_batch(rport, sent, batch);
		sent += batch;
	}
	state_write(sent, got);
	rc = uipc_test_publish(SLOT, rport);
	if (rc != 0)
		fail("re-publish", rc);
	u_put("UIPCCLI: progress sent=");
	putu(sent);
	u_put(" got=");
	putu(got);
	u_put("\n");
	sys_exit(0);
}
