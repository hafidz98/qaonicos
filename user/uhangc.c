/*
 * uhangc.c - FND-3 hang-injection test program (class a
 * hang while owning the console).
 *
 * Spawned through the "embed:uhangc" SYS_SPAWN test hook.
 * Takes exclusive console ownership -- the state sh
 * normally holds -- then yields forever WITHOUT faulting.
 * Nothing else in the system can release console
 * ownership (there is no idle path for it), so the only
 * way back is the FND-3 supervisor killing this child
 * through the FND-2 reclaim funnel: owner back to the
 * boot default, parent woken with the watchdog exit
 * code, console readable again. Deterministic companion
 * to uhangd (whose display-token race with uiapp's idle
 * auto-release is documented in DEVLOG-FND3.md).
 *
 * NOT in uprogs[]: test-only, spawned on demand.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	u_put("UHANGC: taking console, then yielding forever\n");
	(void)sys_console_takeover();
	u_put("UHANGC: console is mine; yielding forever\n");
	for (;;)
		sys_yield();
}
