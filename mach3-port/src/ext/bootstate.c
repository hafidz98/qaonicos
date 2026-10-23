/*
 * mach3-port/src/ext/bootstate.c -- B2 resilience core.
 *
 * Persistent boot state + boot log ring on blk dev 0:
 *   sector [total-2]        : struct bs_state
 *   sectors [total-2-16 .. total-2) : 16 boot log records (ring)
 * (The last sector is the blk selftest scratch; do not touch.)
 *
 * Flow: bootstate_init() increments boot_count and fail_count and
 * persists immediately. If fail_count >= BS_FAIL_THRESHOLD, safe
 * mode is armed. bootstate_healthy() (called when all one-shot
 * tests passed) resets fail_count. On hardware the same logic
 * moves to the NAND env partition unchanged.
 */

extern int printf(const char *, ...);
extern int blk_read_sector(unsigned int sector, unsigned char *data);
extern int blk_write_sector(unsigned int sector,
    const unsigned char *data);

/* B2.8: blk requests can time out transiently (observed under
 * host CPU contention: the first state read of a boot failed,
 * silently disabling bootstate for that whole boot -- caught by
 * soak run 2 as an intermittent UBLOGTEST count=0). All
 * bootstate I/O goes through these 3-attempt wrappers. */
static int
bs_read(unsigned int sector, unsigned char *buf)
{
	int i;

	for (i = 0; i < 3; i++)
		if (blk_read_sector(sector, buf) == 0)
			return 0;
	return -1;
}

static int
bs_write(unsigned int sector, const unsigned char *buf)
{
	int i;

	for (i = 0; i < 3; i++)
		if (blk_write_sector(sector, buf) == 0)
			return 0;
	return -1;
}
extern unsigned int blk_total_sectors(void);

#define BS_MAGIC	0x424f4f54u	/* "BOOT" */
#define BLOG_MAGIC	0x424c4f47u	/* "BLOG" */
#define BS_FAIL_THRESHOLD 3u
#define BLOG_SLOTS	16u

/* last_reason values */
#define BSR_UNKNOWN	0u
#define BSR_POWER	1u
#define BSR_CRASH	2u
#define BSR_WATCHDOG	3u
#define BSR_SOFTWARE	4u	/* FND-1: SYS_RESET / SYS_POWEROFF */

/* boot log events */
#define BLE_BOOT	1u
#define BLE_HEALTHY	2u
#define BLE_SAFEMODE	3u
#define BLE_CRASHMARK	4u
#define BLE_PROVISIONED	5u
#define BLE_PROV_RESET	6u
#define BLE_ROLLBACK	7u
#define BLE_UPD_STAGE	8u
#define BLE_UPD_TRIAL	9u
#define BLE_UPD_CONFIRM	10u
#define BLE_UPD_CANCEL	11u
#define BLE_RESET	12u	/* FND-1: software reset/poweroff */
#define BLE_WD_WARN	13u	/* FND-3: supervisor warn (no progress) */
#define BLE_WD_HANG	14u	/* FND-3: supervisor hang declared */
#define BLE_WD_RESPAWN	15u	/* FND-3: supervisor respawned daemon */
#define BLE_WD_RESET	16u	/* FND-3: supervisor watchdog reset */

struct bs_state {
	unsigned int magic;
	unsigned int boot_count;
	unsigned int fail_count;
	unsigned int last_reason;
	unsigned int log_seq;	/* next boot-log sequence number */
	unsigned int flags;	/* B3: bit0 = provisioned;
				 * B4: bit1 = sabotage armed */
	unsigned int cur_version;	/* B4: version of active slot */
	unsigned int active_slot;	/* B4: 0=A, 1=B */
	unsigned int upd_state;		/* B4: 0 idle, 1 staged, 2 trial */
	unsigned int pending_slot;	/* B4 */
	unsigned int pending_version;	/* B4 */
	unsigned int cksum;
};

struct bs_rec {
	unsigned int magic;
	unsigned int seq;
	unsigned int boot_count;
	unsigned int event;
	unsigned int fail_count;
	unsigned int uptime_ms;
	char note[36];
	unsigned int cksum;	/* B2.7: over the first 60 bytes;
				 * a power cut mid-write tears the
				 * record and this catches it */
};

static unsigned int
bs_rec_cksum(const struct bs_rec *r)
{
	const unsigned char *b = (const unsigned char *)r;
	unsigned int h = 2166136261u;
	unsigned int i;

	for (i = 0; i < 60u; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

static unsigned char bs_sector_buf[512];	/* shared scratch (static:
						 * kernel stacks are
						 * too small for 512B
						 * locals) */
static struct bs_state bs_cur;
static int bs_ready;
static int bs_safe_mode;

unsigned int bootstate_log_count(void);

static unsigned int
bs_cksum(const struct bs_state *s)
{
	return s->magic ^ s->boot_count * 2654435761u ^
	    s->fail_count * 40503u ^ s->last_reason ^ s->log_seq ^
	    s->flags * 977u ^ s->cur_version * 31u ^ s->active_slot ^
	    s->upd_state * 7u ^ s->pending_slot ^ s->pending_version ^
	    0xa5a5a5a5u;
}

static unsigned int
bs_state_sector(void)
{
	return blk_total_sectors() - 2u;
}

static unsigned int
bs_log_sector(unsigned int slot)
{
	return blk_total_sectors() - 2u - BLOG_SLOTS + slot;
}

static void
bs_persist(void)
{
	unsigned char *buf = bs_sector_buf;
	unsigned int i;
	struct bs_state *s = (struct bs_state *)buf;

	for (i = 0; i < sizeof(bs_sector_buf); i++)
		buf[i] = 0;
	bs_cur.magic = BS_MAGIC;
	bs_cur.cksum = bs_cksum(&bs_cur);
	*s = bs_cur;
	bs_write(bs_state_sector(), buf);
}

static void
bs_log(unsigned int event, unsigned int uptime_ms, const char *note)
{
	unsigned char *buf = bs_sector_buf;
	unsigned int i, slot;
	struct bs_rec *r = (struct bs_rec *)buf;

	if (!bs_ready)
		return;
	for (i = 0; i < sizeof(bs_sector_buf); i++)
		buf[i] = 0;
	r->magic = BLOG_MAGIC;
	r->seq = bs_cur.log_seq;
	r->boot_count = bs_cur.boot_count;
	r->event = event;
	r->fail_count = bs_cur.fail_count;
	r->uptime_ms = uptime_ms;
	for (i = 0; note && note[i] && i < sizeof(r->note) - 1u; i++)
		r->note[i] = note[i];
	r->cksum = bs_rec_cksum(r);
	slot = bs_cur.log_seq % BLOG_SLOTS;
	bs_write(bs_log_sector(slot), buf);
	bs_cur.log_seq++;
	bs_persist();
}

int
bootstate_init(void)
{
	unsigned char *buf = bs_sector_buf;
	struct bs_state *s = (struct bs_state *)buf;

	bs_ready = 0;
	bs_safe_mode = 0;
	if (bs_read(bs_state_sector(), buf) != 0) {
		printf("bootstate: WARN state sector unreadable "
		    "(bootstate off this boot)\n");
		return -1;
	}
	bs_cur = *s;
	if (bs_cur.magic != BS_MAGIC ||
	    bs_cur.cksum != bs_cksum(&bs_cur)) {
		bs_cur.magic = BS_MAGIC;
		bs_cur.boot_count = 0;
		bs_cur.fail_count = 0;
		bs_cur.last_reason = BSR_POWER;
		bs_cur.log_seq = 0;
		bs_cur.flags = 0;
		bs_cur.cur_version = 1;
		bs_cur.active_slot = 0;
		bs_cur.upd_state = 0;
		bs_cur.pending_slot = 0;
		bs_cur.pending_version = 0;
	}
	bs_cur.boot_count++;
	bs_cur.fail_count++;
	bs_ready = 1;
	bs_persist();
	if (bs_cur.fail_count >= BS_FAIL_THRESHOLD) {
		bs_safe_mode = 1;
		printf("bootstate: SAFE MODE (fail_count=%u)\n",
		    bs_cur.fail_count);
		bs_log(BLE_SAFEMODE, 0, "safe mode armed");
	} else {
		bs_log(BLE_BOOT, 0, "boot");
	}
	/* B4: update state machine.
	 * state==trial here means the previous boot was a trial that
	 * never confirmed itself healthy -> roll back to the old slot.
	 * state==staged means a staged update gets its trial boot now. */
	if (bs_cur.upd_state == 2u) {
		bs_cur.active_slot ^= 1u;
		bs_cur.upd_state = 0;
		bs_cur.pending_version = 0;
		bs_persist();
		bs_log(BLE_ROLLBACK, 0, "update rolled back");
		printf("bootstate: update ROLLBACK (trial unconfirmed)\n");
	} else if (bs_cur.upd_state == 1u) {
		bs_cur.active_slot = bs_cur.pending_slot;
		bs_cur.upd_state = 2u;
		bs_persist();
		bs_log(BLE_UPD_TRIAL, 0, "update trial boot");
		printf("bootstate: update TRIAL slot=%u ver=%u\n",
		    bs_cur.active_slot, bs_cur.pending_version);
	}

	printf("bootstate: boot #%u fail=%u reason=%u safe=%d\n",
	    bs_cur.boot_count, bs_cur.fail_count, bs_cur.last_reason,
	    bs_safe_mode);
	return bs_safe_mode;
}

/* B4: stage an update for trial on the next boot.
 * 0 ok, -1 busy, -2 downgrade rejected (B5 anti-rollback:
 * a staged update may never move the device backwards). */
int
bootstate_update_stage(unsigned int version)
{
	if (!bs_ready || bs_cur.upd_state != 0u)
		return -1;
	if (version <= bs_cur.cur_version)
		return -2;
	bs_cur.pending_slot = bs_cur.active_slot ^ 1u;
	bs_cur.pending_version = version;
	bs_cur.upd_state = 1u;
	bs_persist();
	bs_log(BLE_UPD_STAGE, 0, "update staged");
	return 0;
}

void
bootstate_update_info(unsigned int *cur_version, unsigned int *slot,
    unsigned int *state, unsigned int *pending_version)
{
	*cur_version = bs_cur.cur_version;
	*slot = bs_cur.active_slot;
	*state = bs_cur.upd_state;
	*pending_version = bs_cur.pending_version;
}

/* B5.3: cancel a staged (not yet trialled) update. 0 ok, -1 if
 * nothing was staged. */
int
bootstate_update_cancel(void)
{
	if (!bs_ready || bs_cur.upd_state != 1u)
		return -1;
	bs_cur.upd_state = 0;
	bs_cur.pending_version = 0;
	bs_persist();
	bs_log(BLE_UPD_CANCEL, 0, "update stage cancelled");
	return 0;
}

/* B4 test hook: arm a one-shot sabotage (skip one healthy mark). */
void
bootstate_sabotage_arm(void)
{
	if (!bs_ready)
		return;
	bs_cur.flags |= 2u;
	bs_persist();
}

int
bootstate_sabotage_take(void)
{
	if (!bs_ready || !(bs_cur.flags & 2u))
		return 0;
	bs_cur.flags &= ~2u;
	bs_persist();
	return 1;
}

int
bootstate_safe_mode(void)
{
	return bs_safe_mode;
}

void
bootstate_healthy(unsigned int uptime_ms)
{
	if (!bs_ready)
		return;
	bs_cur.fail_count = 0;
	bs_persist();
	bs_log(BLE_HEALTHY, uptime_ms, "all one-shots passed");
	/* B4: a healthy trial boot confirms the update. */
	if (bs_cur.upd_state == 2u) {
		bs_cur.cur_version = bs_cur.pending_version;
		bs_cur.pending_version = 0;
		bs_cur.upd_state = 0;
		bs_persist();
		bs_log(BLE_UPD_CONFIRM, uptime_ms, "update confirmed");
		printf("bootstate: update CONFIRMED ver=%u slot=%u\n",
		    bs_cur.cur_version, bs_cur.active_slot);
	}
}

/* Arm a simulated crash: next boot will reach the threshold. */
void
bootstate_crash_sim(void)
{
	if (!bs_ready)
		return;
	bs_cur.fail_count = BS_FAIL_THRESHOLD - 1u;
	bs_cur.last_reason = BSR_CRASH;
	bs_persist();
	bs_log(BLE_CRASHMARK, 0, "crash simulated");
}

/* Best effort from the panic path: record the reason only. */
void
bootstate_panic_note(void)
{
	if (!bs_ready)
		return;
	bs_cur.last_reason = BSR_CRASH;
	bs_persist();
}

/* FND-1: a userspace program asked for a reset or poweroff
 * (SYS_RESET / SYS_POWEROFF). Record reason 4 (software
 * request) and a boot-log event BEFORE the platform acts, so
 * the next boot's bootlog tells the truth about why the
 * previous boot ended. bs_persist() inside bs_log() makes
 * both durable before this returns. */
void
bootstate_note_software_reset(unsigned int uptime_ms, int poweroff)
{
	if (!bs_ready)
		return;
	bs_cur.last_reason = BSR_SOFTWARE;
	bs_persist();
	bs_log(BLE_RESET, uptime_ms,
	    poweroff ? "software poweroff" : "software reset");
}

/* FND-3: generic boot-log writer for the health supervisor
 * (user.c). Event numbers 13..16 are the BLE_WD_* defines
 * above; the caller composes the note. bs_log() is already
 * a no-op when bootstate is off this boot. */
void
bootstate_log_event(unsigned int event, unsigned int uptime_ms,
    const char *note)
{
	bs_log(event, uptime_ms, note);
}

/* FND-3: the supervisor is resetting the device because a
 * daemon hung past every softer rung. Record it like a
 * crash the bootstate path already understands:
 * last_reason = BSR_WATCHDOG (3) and fail_count armed to
 * THRESHOLD-1 -- the same arithmetic bootstate_crash_sim()
 * uses -- so the NEXT boot's bootstate_init() increment
 * lands on the threshold and arms safe mode. Repeated
 * runtime hangs therefore end in the existing 3-strikes
 * safe mode (shell only, which then healthy-marks and
 * self-clears if its one-shots pass), never in an endless
 * reset loop. Both fields are durable before this returns:
 * bs_log() persists the state after writing the record. */
void
bootstate_note_watchdog(unsigned int uptime_ms, const char *note)
{
	if (!bs_ready)
		return;
	bs_cur.last_reason = BSR_WATCHDOG;
	bs_cur.fail_count = BS_FAIL_THRESHOLD - 1u;
	bs_log(BLE_WD_RESET, uptime_ms, note ? note : "watchdog reset");
}

/* B3: provisioning flag (bit0 of flags). */
unsigned int
bootstate_flags(void)
{
	return bs_cur.flags;
}

/* Boot UX: user (GUI) mode flag (bit2 of flags). */
void
bootstate_set_gui_mode(unsigned int on)
{
	if (!bs_ready)
		return;
	if (on)
		bs_cur.flags |= 4u;
	else
		bs_cur.flags &= ~4u;
	bs_persist();
}

unsigned int
bootstate_gui_mode(void)
{
	return (bs_ready && (bs_cur.flags & 4u)) ? 1u : 0u;
}

void
bootstate_set_provisioned(int on)
{
	if (!bs_ready)
		return;
	if (on)
		bs_cur.flags |= 1u;
	else
		bs_cur.flags &= ~1u;
	bs_persist();
	bs_log(on ? BLE_PROVISIONED : BLE_PROV_RESET, 0,
	    on ? "provisioned" : "provisioning reset");
}

void
bootstate_info(unsigned int *boot_count, unsigned int *fail_count,
    unsigned int *safe_mode, unsigned int *last_reason)
{
	*boot_count = bs_cur.boot_count;
	*fail_count = bs_cur.fail_count;
	*safe_mode = (unsigned int)bs_safe_mode;
	*last_reason = bs_cur.last_reason;
}

/* A record is valid iff magic + checksum + expected seq match.
 * B2.7: a record torn by a power cut mid-write fails the
 * checksum and is skipped by readers (the ring overwrites it
 * within BLOG_SLOTS boots -- self-healing). Found by the
 * 100-boot/30-cut soak test (2026-10-20). */
static int
bs_rec_valid(const struct bs_rec *r, unsigned int expect_seq)
{
	return r->magic == BLOG_MAGIC && r->seq == expect_seq &&
	    r->cksum == bs_rec_cksum(r);
}

static unsigned int
bs_window_start(void)
{
	return bs_cur.log_seq >= BLOG_SLOTS ?
	    bs_cur.log_seq - BLOG_SLOTS : 0;
}

/* Copy log record idx (0..count-1, oldest first, valid records
 * only) into out. Returns 0 ok, -1 if absent. */
int
bootstate_log_get(unsigned int idx, struct bs_rec *out)
{
	unsigned char *buf = bs_sector_buf;
	struct bs_rec *r = (struct bs_rec *)buf;
	unsigned int seq, seen = 0;

	if (!bs_ready)
		return -1;
	for (seq = bs_window_start(); seq < bs_cur.log_seq; seq++) {
		if (bs_read(bs_log_sector(seq % BLOG_SLOTS),
		    buf) != 0)
			continue;
		if (!bs_rec_valid(r, seq))
			continue;	/* torn: skip */
		if (seen == idx) {
			/* Byte copy: the user destination may be
			 * unaligned (a word struct copy faults). */
			unsigned char *d = (unsigned char *)out;
			unsigned char *src = (unsigned char *)r;
			unsigned int i;

			for (i = 0; i < 64u; i++)
				d[i] = src[i];
			return 0;
		}
		seen++;
	}
	return -1;
}

unsigned int
bootstate_log_count(void)
{
	unsigned char *buf = bs_sector_buf;
	struct bs_rec *r = (struct bs_rec *)buf;
	unsigned int seq, n = 0;

	if (!bs_ready)
		return 0;
	for (seq = bs_window_start(); seq < bs_cur.log_seq; seq++) {
		if (bs_read(bs_log_sector(seq % BLOG_SLOTS),
		    buf) != 0)
			continue;
		if (bs_rec_valid(r, seq))
			n++;
	}
	return n;
}
