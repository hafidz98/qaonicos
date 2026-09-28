/*
 * mach3/kernel/arm/blk.c -- virtio-blk driver via virtio-mmio LEGACY.
 *
 * M4 item 3: storage for QEMU -M virt.  Single device, synchronous
 * polled I/O (interrupts not needed yet).  Adapted from the proven
 * rv1103-bringup/blk.c (QaonicOS Fase 12d/15/16); rewritten for the
 * Mach3 MD tree.
 *
 * DMA coherency: this port runs with D-cache enabled (locore.s sets
 * SCTLR.C, RAM sections are write-back).  QEMU's virtio device reads
 * and writes guest RAM directly, bypassing the CPU cache, so every
 * request does explicit cache maintenance:
 *   - clean (DCCMVAC) descriptors/avail/request before kick,
 *   - invalidate (DCIMVAC) the used ring while polling and the data/
 *     status buffers after completion.
 *
 * Request format (virtio-blk):
 *   desc[0]: 16B header (type u32, ioprio u32, sector u64), device-read
 *   desc[1]: 512B data (device-write for READ, device-read for WRITE)
 *   desc[2]: 1B status (device-write), 0 = VIRTIO_BLK_S_OK
 */
#include <mach/machine/vm_types.h>	/* unsigned int etc via vm_types */

/* --- virtio-mmio legacy registers --- */
#define	VMM_MAGIC	0x000u
#define	VMM_DEVID	0x008u
#define	VMM_HOSTFEAT	0x010u
#define	VMM_HOSTFEATSEL	0x014u
#define	VMM_GUESTFEAT	0x020u
#define	VMM_GUESTFEATSEL 0x024u
#define	VMM_GUESTPAGESZ	0x028u	/* NOTE: 0x028, not 0x024 (QaonicOS lesson) */
#define	VMM_QSEL	0x030u
#define	VMM_QNUMMAX	0x034u
#define	VMM_QNUM	0x038u
#define	VMM_QALIGN	0x03cu
#define	VMM_QPFN	0x040u
#define	VMM_QNOTIFY	0x050u
#define	VMM_STATUS	0x070u
#define	VMM_CONFIG	0x100u

#define	VMM_BASE	0x0a000000u
#define	VMM_STRIDE	0x200u
#define	VMM_SLOTS	32u
#define	VMM_MAGIC_VAL	0x74726976u
#define	VMM_DEV_BLK	2u

#define	VST_ACK		1u
#define	VST_DRIVER	2u
#define	VST_DRIVER_OK	4u
#define	VST_FEAT_OK	8u

#define	VD_NEXT		1u
#define	VD_WRITE	2u

#define	BLK_T_IN	0u
#define	BLK_T_OUT	1u
#define	BLK_S_OK	0u

#define	BLK_SECTOR	512u
#define	PAGE_SIZE	4096u

/* --- split virtqueue (legacy layout) --- */
#define	VQ_SIZE		128u

struct vq_desc {
	unsigned int addr_lo, addr_hi;
	unsigned int len;
	unsigned short flags, next;
} __attribute__((packed));

struct vq_avail {
	unsigned short flags, idx;
	unsigned short ring[VQ_SIZE];
	unsigned short used_event;
};

struct vq_used_elem {
	unsigned int id, len;
} __attribute__((packed));

struct vq_used {
	unsigned short flags, idx;
	struct vq_used_elem ring[VQ_SIZE];
	unsigned short avail_event;
};

#define	VQ_BLOCK_SIZE	8192u

extern void	panic(const char *, ...);
extern int	printf(const char *, ...);

/* --- cache maintenance (Cortex-A7, 64B lines) --- */
#define	CACHE_LINE	64u

static void
dcache_clean_range(unsigned int va, unsigned int len)
{
	unsigned int a, end;

	end = (va + len + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
	for (a = va & ~(CACHE_LINE - 1u); a < end; a += CACHE_LINE)
		__asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r" (a));
	__asm__ volatile ("dsb ish" ::: "memory");
}

static void
dcache_inval_range(unsigned int va, unsigned int len)
{
	unsigned int a, end;

	end = (va + len + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
	for (a = va & ~(CACHE_LINE - 1u); a < end; a += CACHE_LINE)
		__asm__ volatile ("mcr p15, 0, %0, c7, c6, 1" :: "r" (a));
	__asm__ volatile ("dsb ish" ::: "memory");
}

static void
mem_barrier(void)
{
	__asm__ volatile ("dmb ish" ::: "memory");
}

/* --- device state --- */
static volatile unsigned int *blk_vmm;
static struct vq_desc *blk_desc;
static struct vq_avail *blk_avail;
static struct vq_used *blk_used;
static unsigned int blk_avail_idx, blk_used_idx;
static unsigned int blk_nsectors;

/* One virtqueue block (4KB-aligned) + request buffers.  Identity-mapped
 * kernel: VA == PA, so the device can use these addresses directly. */
static unsigned char vq_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));
static unsigned char req_hdr[16] __attribute__((aligned(16)));
static unsigned char req_data[BLK_SECTOR] __attribute__((aligned(16)));
static volatile unsigned char req_status;

static inline unsigned int
mmio_r(unsigned int off)
{
	return blk_vmm[off / 4u];
}

static inline void
mmio_w(unsigned int off, unsigned int v)
{
	blk_vmm[off / 4u] = v;
}

/*
 * blk_request: run one synchronous request.  type = BLK_T_IN/BLK_T_OUT.
 * For WRITE, 512B are copied from wdata.  For READ, data lands in
 * req_data (caller must copy out).  Returns 0 on VIRTIO_BLK_S_OK.
 */
static int
blk_request(unsigned int type, unsigned int sector, const unsigned char *wdata)
{
	unsigned int i, spin;
	volatile unsigned short *uidx;

	if (!blk_vmm || sector >= blk_nsectors)
		return -1;

	/* Header: type, ioprio=0, sector (LE; ARM is LE). */
	req_hdr[0] = (unsigned char)type;
	req_hdr[1] = req_hdr[2] = req_hdr[3] = 0;
	req_hdr[4] = req_hdr[5] = req_hdr[6] = req_hdr[7] = 0;
	for (i = 0; i < 8; i++)
		req_hdr[8 + i] = (unsigned char)(sector >> (i * 8));
	if (type == BLK_T_OUT && wdata)
		for (i = 0; i < BLK_SECTOR; i++)
			req_data[i] = wdata[i];
	req_status = 0xff;

	/* Descriptor chain: hdr -> data -> status. */
	blk_desc[0].addr_lo = (unsigned int)(unsigned long)req_hdr;
	blk_desc[0].addr_hi = 0;
	blk_desc[0].len = 16;
	blk_desc[0].flags = VD_NEXT;
	blk_desc[0].next = 1;
	blk_desc[1].addr_lo = (unsigned int)(unsigned long)req_data;
	blk_desc[1].addr_hi = 0;
	blk_desc[1].len = BLK_SECTOR;
	blk_desc[1].flags = (type == BLK_T_IN) ? (VD_NEXT | VD_WRITE) : VD_NEXT;
	blk_desc[1].next = 2;
	blk_desc[2].addr_lo = (unsigned int)(unsigned long)&req_status;
	blk_desc[2].addr_hi = 0;
	blk_desc[2].len = 1;
	blk_desc[2].flags = VD_WRITE;
	blk_desc[2].next = 0;

	blk_avail->ring[blk_avail_idx % VQ_SIZE] = 0;
	blk_avail_idx++;
	mem_barrier();
	blk_avail->idx = (unsigned short)blk_avail_idx;

	/* Push our writes to RAM so the device sees them. */
	dcache_clean_range((unsigned int)(unsigned long)blk_desc,
			   3 * sizeof(struct vq_desc));
	dcache_clean_range((unsigned int)(unsigned long)blk_avail,
			   sizeof(struct vq_avail));
	dcache_clean_range((unsigned int)(unsigned long)req_hdr, 16);
	if (type == BLK_T_OUT)
		dcache_clean_range((unsigned int)(unsigned long)req_data,
				   BLK_SECTOR);
	/* req_status written by device: drop any stale cached copy. */
	dcache_inval_range((unsigned int)(unsigned long)&req_status, 1);
	if (type == BLK_T_IN)
		dcache_inval_range((unsigned int)(unsigned long)req_data,
				   BLK_SECTOR);

	mem_barrier();
	mmio_w(VMM_QNOTIFY, 0);
	__asm__ volatile ("dsb ish" ::: "memory");

	/* Poll for completion.  used->idx is written by the device;
	 * invalidate its line each iteration so we never read a stale
	 * cached copy (the Fase 12d volatile-hoisting lesson, now with
	 * D-cache on). */
	uidx = (volatile unsigned short *)&blk_used->idx;
	spin = 0;
	while (*uidx == (unsigned short)blk_used_idx && spin < 100000000u) {
		dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
		spin++;
	}
	if (*uidx == (unsigned short)blk_used_idx)
		return -2;		/* timeout */
	blk_used_idx = *uidx;

	/* Pull device writes into the cache. */
	mem_barrier();
	dcache_inval_range((unsigned int)(unsigned long)blk_used,
			   sizeof(struct vq_used));
	dcache_inval_range((unsigned int)(unsigned long)&req_status, 1);
	if (type == BLK_T_IN)
		dcache_inval_range((unsigned int)(unsigned long)req_data,
				   BLK_SECTOR);

	return (req_status == BLK_S_OK) ? 0 : -3;
}

/*
 * blk_read_sector / blk_write_sector: public MD API (512B sectors).
 */
int
blk_read_sector(unsigned int sector, unsigned char *data)
{
	unsigned int i;
	int r;

	r = blk_request(BLK_T_IN, sector, 0);
	if (r == 0 && data)
		for (i = 0; i < BLK_SECTOR; i++)
			data[i] = req_data[i];
	return r;
}

int
blk_write_sector(unsigned int sector, const unsigned char *data)
{
	return blk_request(BLK_T_OUT, sector, data);
}

unsigned int
blk_total_sectors(void)
{
	return blk_nsectors;
}

/*
 * blk_init: probe virtio-mmio slots for a block device, negotiate,
 * set up one virtqueue, read capacity.  Returns 0 on success.
 */
int
blk_init(void)
{
	unsigned int i, qmax, qalign, status;
	unsigned int cap_lo, cap_hi;
	unsigned long basea, avail_end, used_base;
	volatile unsigned int *base;
	int found;

	found = 0;
	for (i = 0; i < VMM_SLOTS; i++) {
		base = (volatile unsigned int *)(VMM_BASE + i * VMM_STRIDE);
		if (base[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
			continue;
		if (base[VMM_DEVID / 4u] != VMM_DEV_BLK)
			continue;
		blk_vmm = base;
		found = 1;
		break;
	}
	if (!found) {
		printf("blk: no virtio-blk device found\n");
		return -1;
	}

	mmio_w(VMM_STATUS, 0);
	mmio_w(VMM_STATUS, VST_ACK);
	status = mmio_r(VMM_STATUS);
	mmio_w(VMM_STATUS, status | VST_DRIVER);
	mmio_w(VMM_GUESTPAGESZ, PAGE_SIZE);

	/* No features needed, but do the FEATURES_OK dance. */
	mmio_w(VMM_HOSTFEATSEL, 0);
	(void)mmio_r(VMM_HOSTFEAT);
	mmio_w(VMM_GUESTFEATSEL, 0);
	mmio_w(VMM_GUESTFEAT, 0);
	status = mmio_r(VMM_STATUS);
	mmio_w(VMM_STATUS, status | VST_FEAT_OK);
	mem_barrier();

	/* One queue (queue 0). */
	mmio_w(VMM_QSEL, 0);
	qmax = mmio_r(VMM_QNUMMAX);
	if (qmax == 0) {
		printf("blk: queue 0 not available\n");
		return -1;
	}
	if (qmax > VQ_SIZE)
		qmax = VQ_SIZE;
	qalign = mmio_r(VMM_QALIGN);
	if (qalign == 0)
		qalign = PAGE_SIZE;

	basea = (unsigned long)vq_block;
	for (i = 0; i < VQ_BLOCK_SIZE / 4u; i++)
		((unsigned int *)basea)[i] = 0;
	blk_desc = (struct vq_desc *)basea;
	blk_avail = (struct vq_avail *)(basea + 16u * VQ_SIZE);
	avail_end = basea + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
	used_base = (avail_end + qalign - 1u) & ~(unsigned long)(qalign - 1u);
	blk_used = (struct vq_used *)used_base;
	blk_avail_idx = 0;
	blk_used_idx = 0;

	mmio_w(VMM_QNUM, qmax);
	mmio_w(VMM_QALIGN, qalign);
	mem_barrier();
	mmio_w(VMM_QPFN, (unsigned int)(basea / PAGE_SIZE));

	/* Real capacity from device config (u64, 512B sectors). */
	cap_lo = ((volatile unsigned int *)
		  ((unsigned long)blk_vmm + VMM_CONFIG))[0];
	cap_hi = ((volatile unsigned int *)
		  ((unsigned long)blk_vmm + VMM_CONFIG))[1];
	if (cap_hi != 0 || cap_lo == 0) {
		printf("blk: strange capacity hi=%u lo=%u\n", cap_hi, cap_lo);
		return -1;
	}
	blk_nsectors = cap_lo;

	status |= VST_DRIVER_OK;
	mmio_w(VMM_STATUS, status);
	mem_barrier();
	if (!(mmio_r(VMM_STATUS) & VST_DRIVER_OK)) {
		printf("blk: DRIVER_OK rejected\n");
		return -1;
	}

	printf("blk: virtio-blk ready, %u sectors (%u MB)\n",
	       blk_nsectors, blk_nsectors / 2048u);
	return 0;
}

/*
 * blk_selftest: write a pattern to a scratch sector, read it back,
 * verify byte-exact.  Called from machine_init().  Uses the last
 * sector so a real filesystem image stays untouched (mostly).
 */
void
blk_selftest(void)
{
	static unsigned char wbuf[BLK_SECTOR] __attribute__((aligned(16)));
	static unsigned char rbuf[BLK_SECTOR] __attribute__((aligned(16)));
	unsigned int i, sec;
	int r;

	printf("blk_selftest: init...\n");
	if (blk_init() != 0) {
		printf("blk_selftest: FAIL (no device)\n");
		return;
	}

	sec = blk_nsectors - 1;	/* scratch: last sector */
	for (i = 0; i < BLK_SECTOR; i++)
		wbuf[i] = (unsigned char)(i ^ 0xa5);

	printf("blk_selftest: write sector %u...\n", sec);
	r = blk_write_sector(sec, wbuf);
	if (r != 0) {
		printf("blk_selftest: FAIL (write rc=%d)\n", r);
		return;
	}

	for (i = 0; i < BLK_SECTOR; i++)
		rbuf[i] = 0;
	printf("blk_selftest: read back...\n");
	r = blk_read_sector(sec, rbuf);
	if (r != 0) {
		printf("blk_selftest: FAIL (read rc=%d)\n", r);
		return;
	}

	for (i = 0; i < BLK_SECTOR; i++) {
		if (rbuf[i] != wbuf[i]) {
			printf("blk_selftest: FAIL (mismatch at %u: %u != %u)\n",
			       i, rbuf[i], wbuf[i]);
			return;
		}
	}

	printf("blk_selftest: PASS (write+read verify, %u sectors)\n",
	       blk_nsectors);
}
