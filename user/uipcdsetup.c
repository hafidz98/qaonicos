/*
 * uipcdsetup.c - IPC-3 test server, setup run (embed-hook
 * spawn child). Allocates a fresh receive port, sets
 * qlimit 16, publishes it in rendezvous slot "ipc3",
 * and exits. The port survives in the rendezvous until
 * some later run claims it; that is the IPC-1/2
 * publish-then-exit pattern, reused here so the IPC-3
 * driver can stage deaths of the port's owner.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcdsetup". The rendezvous is deleted at
 * IPC-4.
 */
#include "ulib/uipc.h"

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
	u_put("UIPCDSETUP: FAIL ");
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
	unsigned port;

	rc = uipc_port_alloc();
	if (rc < 0)
		fail("alloc", rc);
	port = (unsigned)rc;
	rc = uipc_port_setqlimit(port, 16u);
	if (rc != 0)
		fail("setqlimit", rc);
	rc = uipc_test_publish("ipc3", port);
	if (rc != 0)
		fail("publish", rc);
	u_put("UIPCDSETUP: port published\n");
	sys_exit(0);
}
