/*
 * uradio.c - K5: marker-driven radio-state driver (QA only).
 *
 * Spawn-only test (run from the shell as
 * `run embed:uradio`; NOT a boot one-shot: it needs
 * the active scheduler, like every linkd client).
 *
 * The shell cannot pass arguments to a spawned
 * program (`run` takes only a path), so the command
 * to issue arrives via a littlefs marker file
 * `radio-cmd`, written by the shell's `put` builtin
 * (`put /data/radio-cmd WIFI.CONNECT WARUNGKOPI`,
 * read back with `cat` before running -- the C6L-5
 * marker pattern). uradio reads the marker, splits
 * it on the first space into command + argument,
 * issues it through linkd (the SAME path the K5
 * status-bar poll uses: linkcli -> IPC `link` ->
 * wire -> host mock peer), prints one
 * `URADIO: <line> -> <resp|ERR>` line, clears the
 * marker (writes "0"), and exits 0. A missing or
 * empty marker prints `URADIO: NOMARKER` and exits
 * 1, so a harness never mistakes a skipped run for
 * a successful drive.
 *
 * Only the verify harness writes this marker, and
 * only test-only radio commands go through it
 * (WIFI.CONNECT/DISC, BLE.ON/OFF, WIFI.STATUS,
 * BLE.STATUS against the mock peer's test
 * networks). It never touches production state:
 * the mock peer's NVS is host-process memory.
 *
 * QA image only (QA=1, Tier A): the production
 * image omits it, so `run embed:uradio` fails
 * there by design.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"

/* Littlefs syscalls are not exposed by ulib (same
 * file-local trap wrappers as user/ulink5d.c and
 * user/uiapp/lv_port.c): 94 = MOUNT, 96 = READ,
 * 97 = WRITE. */
static int
radio_lfs_mount(void)
{
	int ret;
	__asm__ volatile(
		"mov r7, #94\n\t"
		"svc #0\n\t"
		"mov %0, r0"
		: "=r" (ret)
		: : "r7", "lr", "memory", "cc");
	return ret;
}

static int
radio_lfs_read(const char *path, char *buf, unsigned max)
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

static int
radio_lfs_write(const char *path, const char *buf, unsigned len)
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

__attribute__((section(".text.start")))
void
_start(void)
{
	char line[160];
	char cmd[64];
	char arg[96];
	char resp[128];
	unsigned i, j;
	int n, rc;

	radio_lfs_mount();
	n = radio_lfs_read("radio-cmd", line, sizeof(line) - 1u);
	if (n <= 0 || line[0] == '0' || line[0] == 0) {
		puts("URADIO: NOMARKER\n");
		sys_exit(1);
	}
	line[n] = 0;
	/* Strip a trailing newline: `put` joins its
	 * words without one, but a hand-written marker
	 * may carry one. */
	for (i = 0u; line[i]; i++) {
		if (line[i] == '\n') {
			line[i] = 0;
			break;
		}
	}
	/* Split on the first space: command + argument. */
	i = 0u;
	while (line[i] && line[i] != ' ')
		i++;
	j = 0u;
	while (j + 1u < sizeof(cmd) && j < i) {
		cmd[j] = line[j];
		j++;
	}
	cmd[j] = 0;
	if (line[i] == ' ')
		i++;
	j = 0u;
	while (line[i] && j + 1u < sizeof(arg)) {
		arg[j++] = line[i++];
	}
	arg[j] = 0;

	if (arg[0])
		rc = uproto_cmd1(cmd, arg, resp, sizeof(resp));
	else
		rc = uproto_cmd(cmd, resp, sizeof(resp));
	puts("URADIO: ");
	puts(line);
	if (rc == 0) {
		puts(" -> ");
		puts(resp);
		puts("\n");
	} else {
		puts(" -> ERR\n");
	}
	/* Clear before exiting: a stale marker must
	 * never re-execute on a later run. The lfs
	 * write returns 0 on success (the shell `put`
	 * convention), not a byte count. */
	if (radio_lfs_write("radio-cmd", "0", 1) != 0)
		puts("URADIO: clear fail\n");
	sys_exit(0);
}
