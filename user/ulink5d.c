/*
 * ulink5d.c - C6L-5 second concurrent IPC client
 * (daemon_images entry, marker-gated like uipcld).
 *
 * At startup it mounts /data and reads
 * "link5-test"; unless the content is exactly "1"
 * it prints one dormant line and exits cleanly, so
 * every other boot is unaffected.
 *
 * When active it is the second live linkd client:
 * it issues link commands through the stub (linkcli)
 * in a slow loop, so its traffic interleaves with
 * the shell-driven spawn client embed:ulink5. The
 * daemon + spawn-child pair is the only true
 * two-client shape (one spawn slot exists, and a
 * spawn child cannot spawn, so two spawn children
 * can never run concurrently).
 *
 * Loop per iteration (paced ~1 s):
 *   TIME.GET (stub) -> KV set "link5d"="v<n>"
 *   -> KV get "link5d" (must round-trip "v<n>")
 *   -> TEST.SCRIPT (stub CMD) -> EVT poll
 *     (budget 5000 ms; must deliver WIFI.DROPPED).
 * The TEST.SCRIPT leg drives the peer-dispatch
 * forwarding while the shell runs embed:ulinktools:
 * with no tools server linkd answers
 * tools_unavailable; while ulinktools runs, the
 * real policy results come back. A failed
 * iteration prints FAILED and continues (the
 * transport-death window legitimately fails it);
 * it never exits mid-test, so its markers also
 * prove a surviving client recovers after the
 * peer returns.
 *
 * NOT in uprogs[]: test-only daemon, launched by
 * `gui` alongside linkd.
 */
#include "ulib/ulib.h"
#include "uartproto/uartproto.h"

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
putu(unsigned v)
{
	char b[12];
	int i = 0;
	if (v == 0u) {
		u_put("0");
		return;
	}
	while (v != 0u) {
		b[i++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	}
	while (i > 0) {
		char s[2];
		s[0] = b[--i];
		s[1] = '\0';
		u_put(s);
	}
}

static void
sleep_ms(unsigned ms)
{
	unsigned t0 = sys_uptime();

	while (sys_uptime() - t0 < ms)
		sys_yield();
}

static int
streq(const char *a, const char *b)
{
	unsigned i = 0u;

	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

static char	marker[8];
static char	buf[1024];
static char	val[64];

__attribute__((section(".text.start")))
void
_start(void)
{
	unsigned n, rc;
	int r;

	if (sys_lfs_simple(94u) != 0) {	/* SYS_LFS_MOUNT (uipcld's number; 95 is LIST) */
		u_put("ULINK5D: dormant (no lfs)\n");
		sys_exit(0);
	}
	r = sys_lfs_read("link5-test", marker, 7u);
	if (r < 0 || marker[0] != '1') {
		u_put("ULINK5D: dormant\n");
		sys_exit(0);
	}

	/* Wait for a working link (peer + linkd up),
	 * bounded 30 s; a daemon must not spin on a
	 * boot where the peer never appears. */
	{
		unsigned t0 = sys_uptime();

		while (uproto_cmd("TIME.GET", buf, sizeof(buf)) != 0) {
			if (sys_uptime() - t0 > 30000u) {
				u_put("ULINK5D: no link\n");
				sys_exit(0);
			}
			sys_yield();
		}
	}
	u_put("ULINK5D: ready\n");

	n = 0u;
	for (;;) {
		int bad = 0;

		n++;
		rc = (unsigned)uproto_cmd("TIME.GET", buf, sizeof(buf));
		if ((int)rc != 0)
			bad = 1;
		/* Own key, own value: a crossed response
		 * (the spawn client's value) fails here. */
		{
			char v[16];
			unsigned j = 0u, k = 0u, vlen = 0u;

			v[0] = 'v';
			j = 1u;
			if (n >= 100u) {
				v[j++] = (char)('0' + (int)(n / 100u));
			}
			if (n >= 10u) {
				v[j++] =
				    (char)('0' + (int)((n / 10u) % 10u));
			}
			v[j++] = (char)('0' + (int)(n % 10u));
			v[j] = 0;
			if (uproto_kv_set("link5d", v, j) != 0)
				bad = 1;
			else if (uproto_kv_get("link5d", val,
					       sizeof(val), &vlen) != 0)
				bad = 1;
			else {
				while (val[k] && v[k]) {
					if (val[k] != v[k]) {
						bad = 1;
						break;
					}
					k++;
				}
				if (val[k] != v[k])
					bad = 1;
			}
		}
		/* Peer dispatches + EVT, every iteration:
		 * with no tools server the peer sees
		 * tools_unavailable; while ulinktools runs
		 * it sees the real policy results. The
		 * peer arms the script ONLY on the exact
		 * line "TEST.SCRIPT c6l3" (bare
		 * TEST.SCRIPT is ERR UNKNOWN -- that
		 * mistake failed every iteration at the
		 * C6L-5 second run). */
		if (uproto_cmd1("TEST.SCRIPT", "c6l3", buf,
			       sizeof(buf)) != 0)
			bad = 1;
		else {
			struct uproto_inb inb;
			int ev = uproto_poll_inbound(&inb, 5000u);

			if (ev != 1 || inb.kind != UPROTO_INB_EVT ||
			    inb.len != 12u ||
			    !streq((const char *)inb.payload,
				   "WIFI.DROPPED"))
				bad = 1;
		}
		u_put("ULINK5D: iter ");
		putu(n);
		if (bad) {
			u_put(" FAILED\n");
		} else {
			u_put(" ok\n");
		}
		sleep_ms(1000u);
	}
}
