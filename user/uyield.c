/*
 * uyield.c - FND-3 hang-injection test program (class a:
 * yields-but-wedged).
 *
 * Spawned through the "embed:uyield" SYS_SPAWN test hook
 * (e.g. the shell's `run embed:uyield`). Prints one banner
 * (its only non-YIELD syscall ever), then yields forever:
 * the cooperative scheduler keeps switching, so from the
 * outside the system looks alive, but this program never
 * makes progress again. The FND-3 supervisor must notice
 * the frozen progress stamp, warn, declare a hang, kill
 * the child through the FND-2 reclaim funnel and wake the
 * parent (sh) with the watchdog exit code instead of
 * leaving the shell waiting forever.
 *
 * NOT in uprogs[]: test-only, spawned on demand.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	u_put("UYIELD: yielding forever, never progressing\n");
	for (;;)
		sys_yield();
}
