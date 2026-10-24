/*
 * uipcdclean.c - IPC-3 test server, clean-death run
 * (embed-hook spawn child). Claims rendezvous slot
 * "ipc3" — taking the receive right into this task's
 * space — and exits cleanly. The exit path runs
 * daemon_reclaim, so this is the clean-death leg of
 * the IPC-3 teardown proof: the claimed port (and any
 * message queued on it) must die with this task.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcdclean".
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
	int rc;

	rc = uipc_test_claim("ipc3");
	if (rc < 0) {
		u_put("UIPCDCLEAN: FAIL claim rc=");
		if (rc < 0) {
			u_put("-");
			putu((unsigned)-rc);
		} else
			putu((unsigned)rc);
		u_put("\n");
		sys_exit(1);
	}
	u_put("UIPCDCLEAN: claimed, exiting cleanly\n");
	sys_exit(0);
}
