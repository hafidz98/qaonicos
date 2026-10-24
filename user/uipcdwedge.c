/*
 * uipcdwedge.c - IPC-3 test server, watchdog-kill run
 * (embed-hook spawn child). Claims rendezvous slot
 * "ipc3", then yields forever without ever making a
 * non-YIELD syscall (the uyield pattern): the FND-3
 * supervisor must warn (15 s), declare a hang (30 s),
 * kill this child through the reclaim funnel, and wake
 * its parent with 0xD09D. This is the watchdog-death
 * leg of the IPC-3 teardown proof.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcdwedge".
 */
#include "ulib/uipc.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;

	rc = uipc_test_claim("ipc3");
	if (rc < 0) {
		u_put("UIPCDWEDGE: FAIL claim\n");
		sys_exit(1);
	}
	u_put("UIPCDWEDGE: claimed, yielding forever\n");
	for (;;)
		sys_yield();
}
