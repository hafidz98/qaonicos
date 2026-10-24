/*
 * bridge.c - bridge backend for the link layer (C6L-2).
 *
 * Guest side of the host-bridge transport: the link layer
 * talks to a host Python peer (scripts/mock-c6.py) over
 * the guest's single TCP client connection (SYS_TCP_*,
 * syscalls 73-77) to the slirp gateway 10.0.2.2:18082.
 * No hostfwd is involved: slirp forwards guest->host
 * connections to a listener on the host directly.
 *
 * Contract (link.h): backends never block. Every wait
 * here yields (sys_yield), which is what pumps the
 * network: net_pump() runs at the top of every
 * sched_yield_switch, even when no other daemon is
 * runnable, and it refreshes the FND-3 activity stamp.
 * A backend that spun on TCP without yielding would
 * wedge the network pump and trip the class-(b)
 * watchdog (DEVLOG-IPC5 §11: park or yield, never spin
 * 30 s without a switch). The link layer's own command
 * timeout (2000 ms) bounds any single call, but the
 * yields below are what make progress possible at all.
 *
 * Single-connection arbitration (D-C6L-4): this backend
 * owns the TCP client connection for the whole session,
 * from open() to close(). The kernel enforces one
 * client connection (tcp_client_connect fails unless
 * CLOSED), and the product flow never overlaps a bridge
 * session with qabotd's provider connection: link
 * sessions run in boot one-shots before GUI/qabotd
 * activity, and strict request/response alternation
 * means no second speaker can interleave on the wire.
 * C6L-5's linkd will arbitrate once consumers share
 * the transport; until then, overlapping use is a
 * caller error, and open() fails loudly (-1) instead
 * of stealing a live connection.
 *
 * Recovery: a dead peer is reported, never hidden.
 * read() returns -1 when the TCP state is CLOSED
 * (peer FIN/RST processed by net_pump) and write()/
 * send_frame() return -1 when not ESTABLISHED. The
 * caller recovers by close + reopen (uproto_backend_
 * select), which reconnects with bounded backoff.
 * open() never retries forever: 3 attempts, 1 s each.
 */

#include "ulib/ulib.h"
#include "uartproto/link.h"

#define	BRIDGE_IP	0x0A000202u	/* 10.0.2.2, slirp gateway */
#define	BRIDGE_PORT	18082u		/* scripts/mock-c6.py --port */
#define	BRIDGE_ATTEMPTS	3u
#define	BRIDGE_ATTEMPT_MS	1000u
#define	BRIDGE_SETTLE_MS	1000u
#define	BRIDGE_SEND_BUDGET_MS	2000u

/* TCP status codes (SYS_TCP_STATUS, syscall 74). */
#define	TCP_CLOSED	0
#define	TCP_SYN_SENT	1
#define	TCP_ESTAB	2

static int
wait_status(int want, unsigned budget_ms)
{
	unsigned t0 = sys_uptime();

	for (;;) {
		int st = sys_tcp_status();

		if (st == want)
			return 0;
		if (st == TCP_CLOSED && want != TCP_CLOSED)
			return -1;
		if (sys_uptime() - t0 >= budget_ms)
			return -1;
		sys_yield();
	}
}

static int
bridge_open(void)
{
	unsigned attempt;

	/* Idempotent: an already-established session is fine
	 * (the link layer re-opens lazily after select). */
	if (sys_tcp_status() == TCP_ESTAB)
		return 0;
	/* A lingering half-open session (SYN_SENT from a
	 * previous attempt, or FIN_SENT after close) blocks
	 * connect (kernel: one client connection). Give it
	 * a bounded window to settle to CLOSED first; a
	 * RST from the peer settles it immediately. */
	if (sys_tcp_status() != TCP_CLOSED) {
		if (wait_status(TCP_CLOSED, BRIDGE_SETTLE_MS) != 0)
			return -1;
	}
	for (attempt = 0; attempt < BRIDGE_ATTEMPTS; attempt++) {
		if (sys_tcp_connect(BRIDGE_IP, BRIDGE_PORT) != 0)
			return -1;
		if (wait_status(TCP_ESTAB, BRIDGE_ATTEMPT_MS) == 0)
			return 0;
		/* Not established: the state is CLOSED again
		 * (RST) or still SYN_SENT; loop retries the
		 * whole attempt, bounded by BRIDGE_ATTEMPTS. */
		if (sys_tcp_status() != TCP_CLOSED) {
			if (wait_status(TCP_CLOSED,
			    BRIDGE_SETTLE_MS) != 0)
				return -1;
		}
	}
	return -1;
}

static int
bridge_close(void)
{
	sys_tcp_close();
	return 0;
}

/* Send all bytes, stop-and-wait (the kernel send is
 * unacked-windowed: 0 means "retry after a pump").
 * Returns 0, or -1 on dead link / budget expiry. */
static int
bridge_write_all(const unsigned char *buf, unsigned len)
{
	unsigned sent = 0u;
	unsigned t0 = sys_uptime();

	if (sys_tcp_status() != TCP_ESTAB)
		return -1;
	while (sent < len) {
		unsigned chunk = len - sent;
		int n;

		if (chunk > 1200u)
			chunk = 1200u;
		n = sys_tcp_send(buf + sent, chunk);
		if (n < 0)
			return -1;
		if (n > 0)
			sent += (unsigned)n;
		if (sent >= len)
			break;
		if (sys_uptime() - t0 >= BRIDGE_SEND_BUDGET_MS)
			return -1;
		sys_yield();
	}
	return 0;
}

static int
bridge_write(const unsigned char *buf, unsigned len)
{
	return bridge_write_all(buf, len);
}

static int
bridge_read(unsigned char *buf, unsigned len)
{
	int n;

	/* The link layer loops on 0 until its own 2 s
	 * deadline; we must not spin: yield so net_pump
	 * runs and the watchdog stays fed. */
	n = sys_tcp_recv(buf, len);
	if (n > 0)
		return n;
	if (sys_tcp_status() == TCP_CLOSED)
		return -1;	/* peer FIN/RST: link dead */
	sys_yield();
	return 0;
}

static int
bridge_send_frame(const unsigned char *frame, unsigned flen)
{
	return bridge_write_all(frame, flen);
}

struct uproto_backend uproto_backend_bridge = {
	"bridge",
	bridge_open,
	bridge_write,
	bridge_read,
	bridge_send_frame,
	bridge_close,
};
