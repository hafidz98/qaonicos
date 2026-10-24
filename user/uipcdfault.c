/*
 * uipcdfault.c - IPC-3 test server, fault-death run
 * (embed-hook spawn child). Claims rendezvous slot
 * "ipc3", then deliberately faults (write to 0x200000,
 * outside this program's pmap — the ufault pattern).
 * The fault routes to user_fault -> sched_exit_switch,
 * so this is the fault-death leg of the IPC-3 teardown
 * proof: reclaim must destroy the claimed port exactly
 * as on a clean exit.
 *
 * NOT in uprogs[]: test-only, spawned on demand via
 * "embed:uipcdfault".
 */
#include "ulib/uipc.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	int rc;

	rc = uipc_test_claim("ipc3");
	if (rc < 0) {
		u_put("UIPCDFAULT: FAIL claim\n");
		sys_exit(1);
	}
	u_put("UIPCDFAULT: claimed, faulting now\n");
	*(volatile unsigned int *)0x200000u = 0xdeadbeefu;
	u_put("UIPCDFAULT: ERROR - the fault did not happen\n");
	sys_exit(1);
	for (;;) {
	}
}
