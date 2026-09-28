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

void
dcache_clean_range(unsigned int va, unsigned int len)
{
	unsigned int a, end;

	end = (va + len + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
	for (a = va & ~(CACHE_LINE - 1u); a < end; a += CACHE_LINE)
		__asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r" (a));
	__asm__ volatile ("dsb ish" ::: "memory");
}

void
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

/* --- device state (Fase D: multi-device) --- */
struct blkdev {
	volatile unsigned int	*vmm;
	struct vq_desc		*desc;
	struct vq_avail		*avail;
	struct vq_used		*used;
	unsigned int		avail_idx, used_idx;
	unsigned int		nsectors;
	/* One virtqueue block (4KB-aligned) + request buffers.
	 * Identity-mapped kernel: VA == PA, so the device can use
	 * these addresses directly. */
	unsigned char		vq_block[VQ_BLOCK_SIZE]
				__attribute__((aligned(4096)));
	unsigned char		req_hdr[16] __attribute__((aligned(16)));
	unsigned char		req_data[BLK_SECTOR] __attribute__((aligned(16)));
	volatile unsigned char	req_status;
};

#define	BLKDEV_MAX	2u
static struct blkdev	blkdevs[BLKDEV_MAX]
			__attribute__((aligned(4096)));
static unsigned int	nblkdevs;	/* 1 = dev 0 saja; 2 = + SD (dev 1) */
/* Urutan slot probe QEMU tak dapat diandalkan (pelajaran Fase 15);
 * petakan: idev = index device internal, sdev = index kartu SD. */
static unsigned int	idev, sdev;

#define	BLK0	(&blkdevs[idev])	/* storage internal */
#define	BLK1	(&blkdevs[sdev])	/* kartu SD */

static inline unsigned int
mmio_r(struct blkdev *d, unsigned int off)
{
	return d->vmm[off / 4u];
}

static inline void
mmio_w(struct blkdev *d, unsigned int off, unsigned int v)
{
	d->vmm[off / 4u] = v;
}

/*
 * blk_request: run one synchronous request on device d.
 * type = BLK_T_IN/BLK_T_OUT.  For WRITE, 512B are copied from wdata.
 * For READ, data lands in d->req_data (caller must copy out).
 * Returns 0 on VIRTIO_BLK_S_OK.
 */
static int
blk_request(struct blkdev *d, unsigned int type, unsigned int sector,
	    const unsigned char *wdata)
{
	unsigned int i, spin;
	volatile unsigned short *uidx;

	if (!d->vmm || sector >= d->nsectors)
		return -1;

	/* Header: type, ioprio=0, sector (LE; ARM is LE). */
	d->req_hdr[0] = (unsigned char)type;
	d->req_hdr[1] = d->req_hdr[2] = d->req_hdr[3] = 0;
	d->req_hdr[4] = d->req_hdr[5] = d->req_hdr[6] = d->req_hdr[7] = 0;
	for (i = 0; i < 8; i++)
		d->req_hdr[8 + i] = (unsigned char)(sector >> (i * 8));
	if (type == BLK_T_OUT && wdata)
		for (i = 0; i < BLK_SECTOR; i++)
			d->req_data[i] = wdata[i];
	d->req_status = 0xff;

	/* Descriptor chain: hdr -> data -> status. */
	d->desc[0].addr_lo = (unsigned int)(unsigned long)d->req_hdr;
	d->desc[0].addr_hi = 0;
	d->desc[0].len = 16;
	d->desc[0].flags = VD_NEXT;
	d->desc[0].next = 1;
	d->desc[1].addr_lo = (unsigned int)(unsigned long)d->req_data;
	d->desc[1].addr_hi = 0;
	d->desc[1].len = BLK_SECTOR;
	d->desc[1].flags = (type == BLK_T_IN) ? (VD_NEXT | VD_WRITE) : VD_NEXT;
	d->desc[1].next = 2;
	d->desc[2].addr_lo = (unsigned int)(unsigned long)&d->req_status;
	d->desc[2].addr_hi = 0;
	d->desc[2].len = 1;
	d->desc[2].flags = VD_WRITE;
	d->desc[2].next = 0;

	d->avail->ring[d->avail_idx % VQ_SIZE] = 0;
	d->avail_idx++;
	mem_barrier();
	d->avail->idx = (unsigned short)d->avail_idx;

	/* Push our writes to RAM so the device sees them. */
	dcache_clean_range((unsigned int)(unsigned long)d->desc,
			   3 * sizeof(struct vq_desc));
	dcache_clean_range((unsigned int)(unsigned long)d->avail,
			   sizeof(struct vq_avail));
	dcache_clean_range((unsigned int)(unsigned long)d->req_hdr, 16);
	if (type == BLK_T_OUT)
		dcache_clean_range((unsigned int)(unsigned long)d->req_data,
				   BLK_SECTOR);
	/* req_status written by device: drop any stale cached copy. */
	dcache_inval_range((unsigned int)(unsigned long)&d->req_status, 1);
	if (type == BLK_T_IN)
		dcache_inval_range((unsigned int)(unsigned long)d->req_data,
				   BLK_SECTOR);

	mem_barrier();
	mmio_w(d, VMM_QNOTIFY, 0);
	__asm__ volatile ("dsb ish" ::: "memory");

	/* Poll for completion.  used->idx is written by the device;
	 * invalidate its line each iteration so we never read a stale
	 * cached copy (the Fase 12d volatile-hoisting lesson, now with
	 * D-cache on). */
	uidx = (volatile unsigned short *)&d->used->idx;
	spin = 0;
	while (*uidx == (unsigned short)d->used_idx && spin < 100000000u) {
		dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
		spin++;
	}
	if (*uidx == (unsigned short)d->used_idx)
		return -2;		/* timeout */
	d->used_idx = *uidx;

	/* Pull device writes into the cache. */
	mem_barrier();
	dcache_inval_range((unsigned int)(unsigned long)d->used,
			   sizeof(struct vq_used));
	dcache_inval_range((unsigned int)(unsigned long)&d->req_status, 1);
	if (type == BLK_T_IN)
		dcache_inval_range((unsigned int)(unsigned long)d->req_data,
				   BLK_SECTOR);

	return (d->req_status == BLK_S_OK) ? 0 : -3;
}

/*
 * blk_read_sector / blk_write_sector: public MD API, dev 0 (512B).
 */
int
blk_read_sector(unsigned int sector, unsigned char *data)
{
	unsigned int i;
	int r;

	r = blk_request(BLK0, BLK_T_IN, sector, 0);
	if (r == 0 && data)
		for (i = 0; i < BLK_SECTOR; i++)
			data[i] = BLK0->req_data[i];
	return r;
}

int
blk_write_sector(unsigned int sector, const unsigned char *data)
{
	return blk_request(BLK0, BLK_T_OUT, sector, data);
}

unsigned int
blk_total_sectors(void)
{
	return BLK0->nsectors;
}

/* Fase D: kapasitas per device (dev 0 = internal, dev 1 = SD). */
unsigned int
blk_nsectors_dev(unsigned int dev)
{
	if (dev >= nblkdevs)
		return 0;
	return (dev == 0 ? BLK0 : BLK1)->nsectors;
}

/* Fase D: SD = dev 1. */
int
sd_present(void)
{
	return nblkdevs > 1;
}

int
sd_read(unsigned int sector, unsigned char *data)
{
	unsigned int i;
	int r;

	if (nblkdevs < 2)
		return -1;
	r = blk_request(BLK1, BLK_T_IN, sector, 0);
	if (r == 0 && data)
		for (i = 0; i < BLK_SECTOR; i++)
			data[i] = BLK1->req_data[i];
	return r;
}

int
sd_write(unsigned int sector, const unsigned char *data)
{
	if (nblkdevs < 2)
		return -1;
	return blk_request(BLK1, BLK_T_OUT, sector, data);
}

/*
 * blkdev_init_one: negotiate + set up one queue + read capacity for
 * the virtio-blk device at `base`.  Returns 0 on success.
 */
static int
blkdev_init_one(struct blkdev *d, volatile unsigned int *base)
{
	unsigned int i, qmax, qalign, status;
	unsigned int cap_lo, cap_hi;
	unsigned long basea, avail_end, used_base;

	d->vmm = base;
	mmio_w(d, VMM_STATUS, 0);
	mmio_w(d, VMM_STATUS, VST_ACK);
	status = mmio_r(d, VMM_STATUS);
	mmio_w(d, VMM_STATUS, status | VST_DRIVER);
	mmio_w(d, VMM_GUESTPAGESZ, PAGE_SIZE);

	/* No features needed, but do the FEATURES_OK dance. */
	mmio_w(d, VMM_HOSTFEATSEL, 0);
	(void)mmio_r(d, VMM_HOSTFEAT);
	mmio_w(d, VMM_GUESTFEATSEL, 0);
	mmio_w(d, VMM_GUESTFEAT, 0);
	status = mmio_r(d, VMM_STATUS);
	mmio_w(d, VMM_STATUS, status | VST_FEAT_OK);
	mem_barrier();

	/* One queue (queue 0). */
	mmio_w(d, VMM_QSEL, 0);
	qmax = mmio_r(d, VMM_QNUMMAX);
	if (qmax == 0)
		return -1;
	if (qmax > VQ_SIZE)
		qmax = VQ_SIZE;
	qalign = mmio_r(d, VMM_QALIGN);
	if (qalign == 0)
		qalign = PAGE_SIZE;

	basea = (unsigned long)d->vq_block;
	for (i = 0; i < VQ_BLOCK_SIZE / 4u; i++)
		((unsigned int *)basea)[i] = 0;
	d->desc = (struct vq_desc *)basea;
	d->avail = (struct vq_avail *)(basea + 16u * VQ_SIZE);
	avail_end = basea + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
	used_base = (avail_end + qalign - 1u) & ~(unsigned long)(qalign - 1u);
	d->used = (struct vq_used *)used_base;
	d->avail_idx = 0;
	d->used_idx = 0;

	mmio_w(d, VMM_QNUM, qmax);
	mmio_w(d, VMM_QALIGN, qalign);
	mem_barrier();
	mmio_w(d, VMM_QPFN, (unsigned int)(basea / PAGE_SIZE));

	/* Real capacity from device config (u64, 512B sectors). */
	cap_lo = ((volatile unsigned int *)
		  ((unsigned long)d->vmm + VMM_CONFIG))[0];
	cap_hi = ((volatile unsigned int *)
		  ((unsigned long)d->vmm + VMM_CONFIG))[1];
	if (cap_hi != 0 || cap_lo == 0)
		return -1;
	d->nsectors = cap_lo;

	status |= VST_DRIVER_OK;
	mmio_w(d, VMM_STATUS, status);
	mem_barrier();
	if (!(mmio_r(d, VMM_STATUS) & VST_DRIVER_OK))
		return -1;
	return 0;
}

/*
 * blkdev_is_fat32: sector 0 has a FAT32 boot signature
 * (0x55AA at 510, "FAT32   " at 0x52).  Used to tell the SD card
 * (dev 1) apart from internal storage (dev 0): QEMU slot order is
 * NOT reliable (Fase 15 lesson), the on-disk signature is.
 */
static int
blkdev_is_fat32(struct blkdev *d)
{
	static unsigned char sec[BLK_SECTOR] __attribute__((aligned(16)));
	unsigned int i;

	if (blk_request(d, BLK_T_IN, 0, 0) != 0)
		return 0;
	for (i = 0; i < BLK_SECTOR; i++)
		sec[i] = d->req_data[i];
	if (sec[510] != 0x55u || sec[511] != 0xaau)
		return 0;
	return sec[0x52] == 'F' && sec[0x53] == 'A' &&
	       sec[0x54] == 'T' && sec[0x55] == '3' &&
	       sec[0x56] == '2';
}

/*
 * blk_init: probe virtio-mmio slots for block devices (up to 2),
 * negotiate each, then order them: dev 0 = internal storage,
 * dev 1 = SD card (identified by FAT32 boot signature, not slot).
 * Returns 0 if at least dev 0 is up.
 */
int
blk_init(void)
{
	unsigned int i, found;
	volatile unsigned int *base;

	found = 0;
	for (i = 0; i < VMM_SLOTS && found < BLKDEV_MAX; i++) {
		base = (volatile unsigned int *)(VMM_BASE + i * VMM_STRIDE);
		if (base[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
			continue;
		if (base[VMM_DEVID / 4u] != VMM_DEV_BLK)
			continue;
		if (blkdev_init_one(&blkdevs[found], base) != 0) {
			printf("blk: slot %u init failed\n", i);
			continue;
		}
		printf("blk: dev %u: slot %u, %u sectors (%u MB)\n",
		       found, i, blkdevs[found].nsectors,
		       blkdevs[found].nsectors / 2048u);
		found++;
	}
	if (found == 0) {
		printf("blk: no virtio-blk device found\n");
		return -1;
	}
	nblkdevs = found;
	idev = 0;
	sdev = 1;

	/* Petakan: device bertanda FAT32 = kartu SD (dev 1).
	 * Urutan slot QEMU tak dapat diandalkan (pelajaran Fase 15). */
	if (found == 2) {
		int f0 = blkdev_is_fat32(&blkdevs[0]);
		int f1 = blkdev_is_fat32(&blkdevs[1]);
		if (f0 && !f1) {
			idev = 1;
			sdev = 0;
			printf("blk: dev 0 = internal (probed 2nd), "
			       "dev 1 = SD (probed 1st)\n");
		} else {
			printf("blk: dev 0 = internal, dev 1 = SD\n");
		}
	}
	printf("blk: ready, %u device(s); SD %s\n",
	       nblkdevs, nblkdevs > 1 ? "present (dev 1)" : "absent");
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

	sec = blk_total_sectors() - 1;	/* scratch: last sector */
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
	       blk_total_sectors());
}
