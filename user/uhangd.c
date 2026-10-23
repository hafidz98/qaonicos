/*
 * uhangd.c - FND-3 hang-injection test program (class a
 * hang while holding the display).
 *
 * Spawned through the "embed:uhangd" SYS_SPAWN test hook,
 * which lends the child the PROG_UIAPP progid (test
 * scaffolding in the hook only: the display token is
 * progid-keyed, so a child that must hang holding it has
 * to carry the holder progid -- same arrangement as
 * FND-2's ufaultd). The child asks the face for the token
 * the product way (EV_MENU -> grant -> acquire), then
 * yields forever WITHOUT faulting. The FND-3 supervisor
 * must kill it through the FND-2 reclaim funnel: the
 * token returns to the face, the face renders again, and
 * a fresh grant/acquire cycle works. Differs from
 * ufaultd exactly in the death: supervisor kill (exit
 * code = watchdog) instead of a data abort.
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

	u_put("UHANGD: requesting display via EV_MENU\n");
	(void)sys_event_inject(EV_MENU);
	for (i = 0; i < 2000u; i++) {
		if (sys_display_acquire() == 1) {
			holder = 1;
			break;
		}
		sys_yield();
	}
	if (holder)
		u_put("UHANGD: display holder = me; "
		    "yielding forever\n");
	else
		u_put("UHANGD: WARN - never became holder; "
		    "yielding forever anyway\n");
	for (;;)
		sys_yield();
}
