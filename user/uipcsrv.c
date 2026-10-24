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
 *     right in the kernel test rendezvous (uipc.h), exit.
 *   run k (drain): claim the receive right back, receive
 *     one batch of requests, verify sequence + payload
 *     byte-exact, reply to each on the send-once right
 *     its header carried, then either park the port
 *     again or -- at 100 served -- destroy it and print
 *     the PASS marker.
 *
 * Progress lives in the ramfs file /ipc1.srv (one ASCII
 * counter) -- the house cross-program coordination
 * pattern. Batch = 16 because upstream Mach caps a
 * port queue at MACH_PORT_QLIMIT_MAX = 16 messages.
 *
 * Payload contract (the client computes the same):
 *   seq s = 0..99, id = 0x1000 + s,
 *   body length: s=0 -> 4, s=50 -> 4096 (the inline
 *     cap), else 8 + (s*37) % 251,
 *   body byte j = (s*31 + j) & 0xFF.
 *   Reply: id = 0x2000 + s, same length, byte ^ 0xA5.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcsrv". Deleted-with-the-rendezvous note:
 * IPC-4 replaces the rendezvous with the registry.
 */
#include "ulib/uipc.h"

#define TOTAL	100u
#define BATCH	16u
#define SLOT	"uipcsrv"
#define STATE	"/ipc1.srv"

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

static void
state_write(unsigned served)
{
	char b[12];
	int i = 0, fd;
	unsigned v = served;
	if (v == 0u)
		b[i++] = '0';
	else {
		char t[12];
		int n = 0;
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
state_read(unsigned *served)
{
	char b[16];
	int fd, n, i;
	unsigned v = 0u;
	fd = u_open(STATE, O_RDONLY);
	if (fd < 0)
		return -1;
	n = u_read(fd, b, sizeof(b) - 1u);
	(void)u_close(fd);
	if (n <= 0)
		return -1;
	for (i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++)
		v = v * 10u + (unsigned)(b[i] - '0');
	*served = v;
	return 0;
}

/* Poll-receive one message; the batch is already queued
 * in the correct interleave, so the first poll normally
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

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;
	unsigned port, served, batch, i, j;

	rc = uipc_test_claim(SLOT);
	if (rc == UIPC_ENORV) {
		/* Setup run: create + park the service port. */
		rc = uipc_port_alloc();
		if (rc < 0)
			fail("alloc", rc);
		port = (unsigned)rc;
		rc = uipc_port_setqlimit(port, BATCH);
		if (rc != 0)
			fail("setqlimit", rc);
		state_write(0u);
		rc = uipc_test_publish(SLOT, port);
		if (rc != 0)
			fail("publish", rc);
		u_put("UIPCSRV: setup port published\n");
		sys_exit(0);
	}
	if (rc < 0)
		fail("claim", rc);
	port = (unsigned)rc;

	if (state_read(&served) != 0)
		fail("state read", -1);
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
		(void)sys_delete(STATE);
		u_put("UIPCSRV: PASS served=100/100\n");
		sys_exit(0);
	}
	state_write(served);
	rc = uipc_test_publish(SLOT, port);
	if (rc != 0)
		fail("re-publish", rc);
	u_put("UIPCSRV: served ");
	putu(served);
	u_put("/100\n");
	sys_exit(0);
}
