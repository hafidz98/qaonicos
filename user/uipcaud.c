/*
 * uipcaud.c - IPC-5 AUD-rate queue sizing probe
 * (embed-hook spawn child, test-only, "embed:uipcaud").
 *
 * Measures, against the frozen uipc.h v1 contract, what
 * the C6L voice path can actually rely on:
 *
 *   A: stall-fill at qlimit 16, 4096 B frames, paced
 *      8/s (the AUD rate: 16 kHz x 16 bit = 32 KB/s) --
 *      how many frames fit, how long the queue takes
 *      to fill with a stalled consumer, what the
 *      sender observes at the limit (EFULL, poll).
 *   B: stall-fill at the DEFAULT qlimit (no SETQLIMIT
 *      call), 1024 B frames, paced ~31/s (the same
 *      32 KB/s in smaller frames) -- proves the
 *      default limit by count, not by assertion.
 *   C: sustained loopback at the AUD rate, 4096 B x
 *      240 frames over ~30 s, send + blocking recv
 *      per slot -- drops must be zero.
 *   D: 1 KB round-trip cost, unpaced send + poll recv
 *      x 200 -- microseconds per round trip.
 *   E: deadline-park cadence, 100 x park(10 ms) on an
 *      empty port -- iterations/s, the cadence the
 *      fan-in poll-loop pattern (DEVLOG-IPC5) relies
 *      on when linkd waits on more than one port.
 *
 * Self-send goes through register + lookup, the same
 * public path the sh `ipc` builtins use: the probe
 * holds a receive port and a send right minted by
 * the registry for its own name. Pacing polls
 * sys_uptime() (100 Hz ticks, 10 ms granularity) and
 * YIELDS each wait iteration: a spawn child running
 * 30 s without a scheduler switch trips FND-3's
 * class-(b) never-yields detector, and phase C spans
 * ~30 s -- so every wait is also scheduler activity.
 * Phase C prints a progress marker every 60 frames
 * so a stall is diagnosable from serial alone.
 *
 * Prints one greppable AUD: line per phase plus
 * AUD: DONE on success; any expectation failure
 * prints AUD: FAIL <phase> and exits 1.
 *
 * NOT in uprogs[]: test-only, "embed:uipcaud".
 */
#include "ulib/uipc.h"

static struct uipc_msg	tx;
static struct uipc_msg	rx;

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
puti(int v)
{
	if (v < 0) {
		u_put("-");
		putu((unsigned)-v);
	} else
		putu((unsigned)v);
}

/* Fill the tx body: first word = seq, rest = pattern. */
static void
fill_tx(unsigned seq, unsigned body_len)
{
	unsigned i;
	tx.reply = 0u;
	tx.dest = 0u;
	tx.id = (int)seq;
	tx.size = UIPC_HDR_SIZE + body_len;
	tx.body[0] = (unsigned char)(seq & 0xffu);
	tx.body[1] = (unsigned char)((seq >> 8) & 0xffu);
	tx.body[2] = (unsigned char)((seq >> 16) & 0xffu);
	tx.body[3] = (unsigned char)((seq >> 24) & 0xffu);
	for (i = 4; i < body_len; i++)
		tx.body[i] = (unsigned char)(seq & 0xffu);
}

static unsigned
seq_of(struct uipc_msg *m)
{
	return (unsigned)m->body[0]
	    | ((unsigned)m->body[1] << 8)
	    | ((unsigned)m->body[2] << 16)
	    | ((unsigned)m->body[3] << 24);
}

/* Busy-poll sys_uptime() until absolute time t. The
 * wait YIELDS each iteration: a spawn child that runs
 * 30 s without a scheduler switch trips FND-3's
 * class-(b) never-yields detector (watchdog reset),
 * and this probe's phase C spans ~30 s -- so every
 * wait is also scheduler activity. Yield waits one
 * 100 Hz tick, so slot quantization stays 10 ms. */
static void
wait_until(unsigned t)
{
	while (sys_uptime() < t)
		sys_yield();
}

static void
fail(const char *phase)
{
	u_put("AUD: FAIL ");
	u_put(phase);
	u_put("\n");
	sys_exit(1);
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc, port, send;
	unsigned t0, el, i, fill, drained, drops, etime_n;

	/* ---- Phase A: stall-fill, qlimit 16, 4096 B, 8/s. */
	port = uipc_port_alloc();
	if (port < 0)
		fail("A alloc");
	rc = uipc_port_setqlimit((unsigned)port, 16u);
	if (rc != 0)
		fail("A qlimit");
	rc = uipc_register("audsink", (unsigned)port);
	if (rc != 0)
		fail("A register");
	send = uipc_lookup("audsink");
	if (send < 0)
		fail("A lookup");
	fill = 0u;
	t0 = sys_uptime();
	for (i = 0u; i < 40u; i++) {
		wait_until(t0 + i * 125u);
	 fill_tx((unsigned)i, 4096u);
		rc = uipc_send((unsigned)send, &tx,
			       UIPC_HDR_SIZE + 4096u);
		if (rc == -4) {	/* EFULL: queue full */
			fill = i;
			break;
		}
		if (rc != 0)
			fail("A send");
	}
	el = sys_uptime() - t0;
	u_put("AUD: A fill=");
	putu(fill);
	u_put(" elapsed_ms=");
	putu(el);
	u_put("\n");
	if (fill != 16u)
		fail("A count");
	drained = 0u;
	for (;;) {
		rc = uipc_recv_poll((unsigned)port, &rx,
				    sizeof(rx));
		if (rc == -5)	/* EEMPTY */
			break;
		if (rc < 0 || seq_of(&rx) != drained)
			fail("A drain");
		drained++;
	}
	u_put("AUD: A drained=");
	putu(drained);
	u_put("\n");
	if (drained != 16u)
		fail("A drained-count");
	uipc_port_destroy((unsigned)port);

	/* ---- Phase B: default qlimit, 1024 B, ~31/s. */
	port = uipc_port_alloc();
	if (port < 0)
		fail("B alloc");
	/* No SETQLIMIT: the default is what is measured. */
	rc = uipc_register("audsink2", (unsigned)port);
	if (rc != 0)
		fail("B register");
	send = uipc_lookup("audsink2");
	if (send < 0)
		fail("B lookup");
	fill = 0u;
	t0 = sys_uptime();
	for (i = 0u; i < 40u; i++) {
		wait_until(t0 + i * 32u);
	 fill_tx((unsigned)i, 1024u);
		rc = uipc_send((unsigned)send, &tx,
			       UIPC_HDR_SIZE + 1024u);
		if (rc == -4) {
			fill = i;
			break;
		}
		if (rc != 0)
			fail("B send");
	}
	el = sys_uptime() - t0;
	u_put("AUD: B fill=");
	putu(fill);
	u_put(" elapsed_ms=");
	putu(el);
	u_put("\n");
	if (fill != 5u)
		fail("B count");
	drained = 0u;
	for (;;) {
		rc = uipc_recv_poll((unsigned)port, &rx,
				    sizeof(rx));
		if (rc == -5)
			break;
		if (rc < 0 || seq_of(&rx) != drained)
			fail("B drain");
		drained++;
	}
	if (drained != 5u)
		fail("B drained-count");
	uipc_port_destroy((unsigned)port);

	/* ---- Phase C: sustained AUD loopback, 240 x 4096 B. */
	port = uipc_port_alloc();
	if (port < 0)
		fail("C alloc");
	rc = uipc_port_setqlimit((unsigned)port, 16u);
	if (rc != 0)
		fail("C qlimit");
	rc = uipc_register("audsink3", (unsigned)port);
	if (rc != 0)
		fail("C register");
	send = uipc_lookup("audsink3");
	if (send < 0)
		fail("C lookup");
	drops = 0u;
	t0 = sys_uptime();
	for (i = 0u; i < 240u; i++) {
		wait_until(t0 + i * 125u);
		if (i != 0u && (i % 60u) == 0u) {
			u_put("AUD: C progress=");
			putu(i);
			u_put("\n");
		}
		fill_tx(i, 4096u);
		rc = uipc_send((unsigned)send, &tx,
			       UIPC_HDR_SIZE + 4096u);
		if (rc != 0) {
			drops++;
			continue;
		}
		rc = uipc_recv((unsigned)port, &rx, sizeof(rx),
			       2000);
		if (rc < 0 || seq_of(&rx) != i)
			drops++;
	}
	el = sys_uptime() - t0;
	u_put("AUD: C sent=240 recv=");
	putu(240u - drops);
	u_put(" drops=");
	putu(drops);
	u_put(" elapsed_ms=");
	putu(el);
	u_put(" rate_x10=");
	/* frames/s x 10, integer math only */
	putu(el ? (240u * 10000u) / el : 0u);
	u_put("\n");
	if (drops != 0u)
		fail("C drops");
	uipc_port_destroy((unsigned)port);

	/* ---- Phase D: 1 KB round-trip cost, unpaced. */
	port = uipc_port_alloc();
	if (port < 0)
		fail("D alloc");
	rc = uipc_register("audsink4", (unsigned)port);
	if (rc != 0)
		fail("D register");
	send = uipc_lookup("audsink4");
	if (send < 0)
		fail("D lookup");
	t0 = sys_uptime();
	for (i = 0u; i < 200u; i++) {
	 fill_tx(i, 1024u);
		rc = uipc_send((unsigned)send, &tx,
			       UIPC_HDR_SIZE + 1024u);
		if (rc != 0)
			fail("D send");
		rc = uipc_recv_poll((unsigned)port, &rx,
				    sizeof(rx));
		if (rc < 0 || seq_of(&rx) != i)
			fail("D recv");
	}
	el = sys_uptime() - t0;
	u_put("AUD: D iters=200 elapsed_ms=");
	putu(el);
	u_put(" us_per_roundtrip=");
	putu(el ? (el * 1000u) / 200u : 0u);
	u_put("\n");
	uipc_port_destroy((unsigned)port);

	/* ---- Phase E: deadline-park cadence, 100 x 10 ms. */
	port = uipc_port_alloc();
	if (port < 0)
		fail("E alloc");
	etime_n = 0u;
	t0 = sys_uptime();
	for (i = 0u; i < 100u; i++) {
		rc = uipc_recv((unsigned)port, &rx, sizeof(rx),
			       10);
		if (rc == -10)	/* ETIME */
			etime_n++;
	}
	el = sys_uptime() - t0;
	u_put("AUD: E parks=100 etime=");
	putu(etime_n);
	u_put(" elapsed_ms=");
	putu(el);
	u_put(" iters_per_s=");
	putu(el ? (100u * 1000u) / el : 0u);
	u_put("\n");
	if (etime_n != 100u)
		fail("E etime");
	uipc_port_destroy((unsigned)port);

	u_put("AUD: DONE\n");
	sys_exit(0);
}
