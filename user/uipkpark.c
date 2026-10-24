/*
 * uipkpark.c - IPC-2 long-park probe (embed-hook child).
 *
 * Allocates a private port nobody sends to and parks
 * in uipc_recv with a 65 s deadline. While it is
 * parked the spawn slot is WAITING on IPC and the
 * shell is WAITING on the spawn: the system is in the
 * all-parked state the plan calls out, and the FND-3
 * watchdog must stay silent (the waiter is state 2,
 * exempt-but-observed) and class (b) must not fire
 * (the kernel idles in SVC context, interrupts on).
 * The only exit is the deadline: the program prints
 * the wake verdict with its guest-clock elapsed time
 * and PASSes iff the return was UIPC_ETIME after
 * roughly 65 s.
 *
 * NOT in uprogs[]: test-only, "embed:uipkpark".
 */
#include "ulib/uipc.h"

#define PARK_MS	65000

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
	u_put("UIPKPARK: FAIL ");
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

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;
	unsigned port, t0, el;

	rc = uipc_port_alloc();
	if (rc < 0)
		fail("alloc", rc);
	port = (unsigned)rc;

	u_put("UIPKPARK: parking deadline=65000\n");
	t0 = sys_uptime();
	rc = uipc_recv(port, &msg, UIPC_MSG_MAX, PARK_MS);
	el = sys_uptime() - t0;
	u_put("UIPKPARK: woke rc=");
	if (rc < 0) {
		u_put("-");
		putu((unsigned)-rc);
	} else
		putu((unsigned)rc);
	u_put(" elapsed=");
	putu(el);
	u_put("\n");
	(void)uipc_port_destroy(port);
	if (rc != UIPC_ETIME)
		fail("wake rc", rc);
	if (el < 60000u || el > 90000u)
		fail("park elapsed", (int)el);
	u_put("UIPKPARK: PASS\n");
	sys_exit(0);
}
