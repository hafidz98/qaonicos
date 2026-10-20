/*
 * user/ufirst.c -- B3: first-boot provisioning (OS side).
 *
 * Runs every boot as a one-shot. If the device is not provisioned
 * yet (bootstate flags bit0 = 0): print the first-boot banner and
 * create default config files on littlefs. If provisioned: quiet.
 */
#include "ulib/ulib.h"

static int
sys_bootstate(unsigned sub)
{
	register unsigned _r0 __asm__("r0") = sub;
	int ret;
	__asm__ volatile(
		"mov r7, #98\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sys_lfs_simple(unsigned num)
{
	int ret;
	__asm__ volatile(
		"mov r7, %1\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (num)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sys_lfs_write(const char *path, const char *buf, unsigned len)
{
	register unsigned _r0 __asm__("r0") = (unsigned)path;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = len;
	int ret;
	__asm__ volatile(
		"mov r7, #97\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static int
sys_lfs_read(const char *path, char *buf, unsigned max)
{
	register unsigned _r0 __asm__("r0") = (unsigned)path;
	register unsigned _r1 __asm__("r1") = (unsigned)buf;
	register unsigned _r2 __asm__("r2") = max;
	int ret;
	__asm__ volatile(
		"mov r7, #96\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret) : "r" (_r0), "r" (_r1), "r" (_r2)
		: "r7", "lr", "memory", "cc");
	return ret;
}

static void
ensure_file(const char *path, const char *content, unsigned len)
{
	static char buf[32];

	if (sys_lfs_read(path, buf, sizeof(buf)) < 0)
		sys_lfs_write(path, content, len);
}

__attribute__((section(".text.start")))
void
_start(void)
{
	int flags = sys_bootstate(3u);

	if (flags >= 0 && (flags & 1) == 0) {
		u_put("\n");
		u_put("==========================================\n");
		u_put("  Welcome to QaonicOS -- first boot setup\n");
		u_put("==========================================\n");
		u_put("This device is not set up yet.\n");
		u_put("  * Wi-Fi: connect via the companion app\n");
		u_put("    (ESP32-C6 portal) when available.\n");
		u_put("  * Local setup: run 'setup' in the shell\n");
		u_put("    (setup name <x>, setup tz <+HHMM>,\n");
		u_put("     then 'setup done').\n");
		if (sys_lfs_simple(94u) == 0) {
			ensure_file("cfg-name", "qaon", 4u);
			ensure_file("cfg-tz", "+0700", 5u);
		}
		u_put("UFIRST: first-boot (not provisioned)\n");
	} else {
		u_put("UFIRST: provisioned\n");
	}
	sys_exit(0);
}
