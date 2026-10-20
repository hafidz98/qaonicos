/*
 * kernel/kernel/arm/bootstate.c -- B2 resilience core.
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

/* boot log events */
#define BLE_BOOT	1u
#define BLE_HEALTHY	2u
#define BLE_SAFEMODE	3u
#define BLE_CRASHMARK	4u
#define BLE_PROVISIONED	5u
#define BLE_PROV_RESET	6u

struct bs_state {
	unsigned int magic;
	unsigned int boot_count;
	unsigned int fail_count;
	unsigned int last_reason;
	unsigned int log_seq;	/* next boot-log sequence number */
	unsigned int flags;	/* B3: bit0 = provisioned */
	unsigned int cksum;
};

struct bs_rec {
	unsigned int magic;
	unsigned int seq;
	unsigned int boot_count;
	unsigned int event;
	unsigned int fail_count;
	unsigned int uptime_ms;
	char note[40];
};

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
	    s->flags * 977u ^ 0xa5a5a5a5u;
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
	blk_write_sector(bs_state_sector(), buf);
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
	slot = bs_cur.log_seq % BLOG_SLOTS;
	blk_write_sector(bs_log_sector(slot), buf);
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
	if (blk_read_sector(bs_state_sector(), buf) != 0)
		return -1;
	bs_cur = *s;
	if (bs_cur.magic != BS_MAGIC ||
	    bs_cur.cksum != bs_cksum(&bs_cur)) {
		bs_cur.magic = BS_MAGIC;
		bs_cur.boot_count = 0;
		bs_cur.fail_count = 0;
		bs_cur.last_reason = BSR_POWER;
		bs_cur.log_seq = 0;
		bs_cur.flags = 0;
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
	printf("bootstate: boot #%u fail=%u reason=%u safe=%d\n",
	    bs_cur.boot_count, bs_cur.fail_count, bs_cur.last_reason,
	    bs_safe_mode);
	return bs_safe_mode;
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

/* B3: provisioning flag (bit0 of flags). */
unsigned int
bootstate_flags(void)
{
	return bs_cur.flags;
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

/* Copy log record idx (0..count-1, oldest first) into out.
 * Returns 0 ok, -1 if absent. */
int
bootstate_log_get(unsigned int idx, struct bs_rec *out)
{
	unsigned char *buf = bs_sector_buf;
	struct bs_rec *r = (struct bs_rec *)buf;
	unsigned int count, target;

	if (!bs_ready)
		return -1;
	count = bootstate_log_count();
	if (idx >= count)
		return -1;
	target = bs_cur.log_seq - count + idx;
	if (blk_read_sector(bs_log_sector(target % BLOG_SLOTS), buf) != 0)
		return -1;
	if (r->magic != BLOG_MAGIC || r->seq != target)
		return -1;
	/* Byte copy: the user destination may be unaligned (a word
	 * struct copy faults with an alignment abort, seen as a hang
	 * after panic in the syscall path). */
	{
		unsigned char *d = (unsigned char *)out;
		unsigned char *src = (unsigned char *)r;
		unsigned int i;

		for (i = 0; i < 64u; i++)
			d[i] = src[i];
	}
	return 0;
}

unsigned int
bootstate_log_count(void)
{
	return bs_cur.log_seq >= BLOG_SLOTS ? BLOG_SLOTS : bs_cur.log_seq;
}
