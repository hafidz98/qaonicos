/*
 * ufaultd.c - FND-2 fault-injection test program (display).
 *
 * Spawned through the "embed:ufaultd" SYS_SPAWN test hook,
 * which lends the child the PROG_UIAPP progid (test
 * scaffolding in the hook only: the display token is
 * progid-keyed, so a child that must die holding it has
 * to carry the holder progid). The child asks the face
 * for the token the product way -- an EV_MENU event, the
 * face grants -- waits until it is the holder, and then
 * dies on a deliberate data abort. The FND-2 reclaim path
 * must return the token to the face; without it the face
 * yields forever (it never renders or routes events
 * while the token is out) and the display is dead until
 * power cycle.
 *
 * NOT in uprogs[]: test-only, spawned on demand.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	unsigned i;
	int holder = 0;

	u_put("UFAULTD: requesting display via EV_MENU\n");
	(void)sys_event_inject(EV_MENU);
	for (i = 0; i < 2000u; i++) {
		if (sys_display_acquire() == 1) {
			holder = 1;
			break;
		}
		sys_yield();
	}
	if (holder)
		u_put("UFAULTD: display holder = me; faulting now\n");
	else
		u_put("UFAULTD: WARN - never became holder; "
		    "faulting anyway\n");
	/* Deliberate data abort (see ufault.c). */
	*(volatile unsigned int *)0x200000u = 0xdeadbeefu;
	u_put("UFAULTD: ERROR - the fault did not happen\n");
	u_exit();
	for (;;) {
	}
}
