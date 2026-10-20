/*
 * user/utimegate.c -- B3: time-quality gating test.
 *
 * Expectations at boot (NTP daemon disabled in the sandbox):
 *   1. quality == 0 (nothing synced yet)
 *   2. after sys_time_set (manual set): quality still 0
 *   3. after sys_time_sync (what the NTP daemon calls): quality 1
 * Prints UTIMEGATE: PASS/FAIL.
 */
#include "ulib/ulib.h"

__attribute__((section(".text.start")))
void
_start(void)
{
	int fails = 0;

	if (sys_time_quality() != 0)
		fails++;
	sys_time_set(1790683200u);
	if (sys_time_quality() != 0)
		fails++;
	if (sys_time_sync() != 0)
		fails++;
	if (sys_time_quality() != 1)
		fails++;
	/* Restore the honest state: nothing really synced this boot. */
	sys_time_unsync();

	if (fails == 0)
		u_put("UTIMEGATE: PASS\n");
	else
		u_put("UTIMEGATE: FAIL\n");
	sys_exit(fails);
}
