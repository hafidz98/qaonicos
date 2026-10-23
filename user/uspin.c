/*
 * uspin.c - FND-3 hang-injection test program (class b:
 * never yields).
 *
 * Spawned through the "embed:uspin" SYS_SPAWN test hook
 * (e.g. the shell's `run embed:uspin`). Prints one banner,
 * then spins in pure userspace: no syscalls, no yield --
 * the cooperative scheduler never runs again, no switch
 * point executes, the network pump stops. The only part
 * of the kernel that still runs is the timer IRQ, and
 * the FND-3 IRQ-side starvation check must fire there:
 * bootstate watchdog mark (reason 3) + platform reset,
 * landing the next boot in the existing safe mode.
 *
 * NOT in uprogs[]: test-only, spawned on demand.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	u_put("USPIN: spinning forever, never yielding\n");
	for (;;)
		__asm__ volatile ("" ::: "memory");
}
