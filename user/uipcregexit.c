/*
 * uipcregexit.c - IPC-4 anti-rendezvous leg
 * (embed-hook spawn child). Allocates a port,
 * registers the name `gone`, and exits cleanly.
 * The registry entry must SURVIVE as a dead entry
 * (unlike the deleted test rendezvous, whose parked
 * right could be claimed by anyone): a later lookup
 * of `gone` must return UIPC_EDEST, and the death
 * must have bumped the generation to 2.
 *
 * NOT in uprogs[]: test-only, "embed:uipcregexit".
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

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc, port;

	rc = uipc_port_alloc();
	if (rc < 0) {
		u_put("UIPCREGEXIT: FAIL alloc rc=-");
		putu((unsigned)-rc);
		u_put("\n");
		sys_exit(1);
	}
	port = rc;
	rc = uipc_register("gone", (unsigned)port);
	if (rc != 0) {
		u_put("UIPCREGEXIT: FAIL register rc=-");
		putu((unsigned)-rc);
		u_put("\n");
		sys_exit(1);
	}
	u_put("UIPCREGEXIT: registered gone\n");
	sys_exit(0);
}
