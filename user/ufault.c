/*
 * ufault.c - FND-2 fault-injection test program (console +
 * web-shell bridge).
 *
 * Spawned through the "embed:ufault" SYS_SPAWN test hook
 * (e.g. the shell's `run embed:ufault`). Takes exclusive
 * console ownership -- the state sh normally holds -- then
 * watches the web-shell bridge for up to ~8 s: if a web
 * command is pending it takes it (bridge state -> running)
 * and dies WITHOUT posting a result. Finally it dies on a
 * deliberate data abort either way.
 *
 * The FND-2 reclaim path must (a) hand the console back --
 * without it the owner index points at this dead child
 * forever and the shell can never read the console again
 * (SYS_READ_CONSOLE refuses every non-owner) -- and
 * (b) settle the bridge -- without it a taken command
 * wedges the web shell in "running" forever.
 *
 * NOT in uprogs[]: this program must only ever run as a
 * spawned child in a test, never at boot.
 */
#include "ulib/ulib.h"

/* SYS_WEBUI (103) sub 0: take a pending web-shell command.
 * Same raw SVC convention as sh's sh_webui(). */
static int
uf_webui_take(char *buf, unsigned len)
{
	int ret;
	__asm__ volatile(
		"mov r0, %1\n\t"
		"mov r1, %2\n\t"
		"mov r2, %3\n\t"
		"mov r7, #103\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: "r" (0u), "r" (buf), "r" (len)
		: "r0", "r1", "r2", "r7", "lr", "memory", "cc");
	return ret;
}

__attribute__((section(".text.start")))
void
_start(void)
{
	static char cmd[160];
	unsigned t0;
	int took = 0;

	u_put("UFAULT: taking console, then faulting\n");
	(void)sys_console_takeover();
	u_put("UFAULT: console is mine; "
	    "watching web-shell bridge\n");
	t0 = sys_uptime();
	while (sys_uptime() - t0 < 8000u) {
		if (uf_webui_take(cmd, sizeof(cmd)) >= 0) {
			took = 1;
			break;
		}
		sys_yield();
	}
	if (took)
		u_put("UFAULT: took web-shell command; "
		    "faulting without posting\n");
	else
		u_put("UFAULT: no web-shell command; faulting now\n");
	/* Deliberate data abort: 0x200000 is outside this
	 * program's pmap (only its code/stack/heap pages at
	 * 0x100000 are mapped). Routes to user_fault ->
	 * sched_exit_switch, like any real daemon crash. */
	*(volatile unsigned int *)0x200000u = 0xdeadbeefu;
	u_put("UFAULT: ERROR - the fault did not happen\n");
	u_exit();
	for (;;) {
	}
}
