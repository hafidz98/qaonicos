/*
 * mach3/kernel/arm/net.c -- Driver virtio-net via virtio-mmio LEGACY
 * (Fase D).
 *
 * Port dari archive/kernel-scratch/kernel/src/net.c (Fase 11, terbukti).
 * Perbedaan vs kernel lama:
 *  - POLLING MURNI: tanpa IRQ (net_isr/sched_wakeup_net dibuang);
 *    net_poll() dipanggil dari server loop.
 *  - D-cache ON di port Mach 3 ini: coherency eksplisit (clean
 *    deskriptor/avail/tx sebelum notify; invalidate used ring +
 *    buffer RX; pola blk.c).
 *  - Logging via printf kernel (bukan UART mentah).
 *
 * Register legacy:
 *   0x010 HostFeatures (32-bit), 0x020 GuestFeatures,
 *   0x024 DriverFeaturesSel, 0x028 GuestPageSize, 0x030 QueueSel,
 *   0x038 QueueNum, 0x03c QueueAlign, 0x040 QueuePFN,
 *   0x050 QueueNotify, 0x060 InterruptStatus, 0x064 InterruptACK,
 *   0x070 Status, 0x100 Config.
 *
 * Queue (legacy): satu blok kontinu, align QueueAlign (4096):
 *   [desc 16*QN][avail 6+2*QN][pad -> align][used 6+8*QN]
 * GuestPageSize=4096, QueuePFN = alamat_fisik_queue / 4096.
 *
 * virtio-mmio slots: 0x0a000000 + i*0x200 (32 slot).
 */

/* D-cache maintenance (pmap.c / blk.c). */
extern void	dcache_clean_range(unsigned int va, unsigned int len);
extern void	dcache_inval_range(unsigned int va, unsigned int len);
extern int	printf(const char *, ...);

/* --- Register virtio-mmio legacy --- */
#define VMM_MAGIC       0x000u
#define VMM_VERSION     0x004u
#define VMM_DEVID       0x008u
#define VMM_HOSTFEAT    0x010u
#define VMM_HOSTFEATSEL 0x014u
#define VMM_GUESTFEAT   0x020u
#define VMM_GUESTFEATSEL 0x024u
#define VMM_GUESTPAGESZ 0x028u
#define VMM_QSEL        0x030u
#define VMM_QNUMMAX     0x034u
#define VMM_QNUM        0x038u
#define VMM_QALIGN      0x03cu
#define VMM_QPFN        0x040u
#define VMM_QNOTIFY     0x050u
#define VMM_INTSTAT     0x060u
#define VMM_INTACK      0x064u
#define VMM_STATUS      0x070u
#define VMM_CONFIG      0x100u

#define VMM_BASE        0x0a000000u
#define VMM_STRIDE      0x200u
#define VMM_SLOTS       32u
#define VMM_MAGIC_VAL   0x74726976u
#define VMM_DEV_NET     1u

/* Status bits. */
#define VST_ACK         1u
#define VST_DRIVER      2u
#define VST_DRIVER_OK   4u
#define VST_FEAT_OK     8u

/* Feature bits (legacy, 32-bit). */
#define VF_NET_MAC      (1u << 5)

/* Descriptor flags. */
#define VD_NEXT         1u
#define VD_WRITE        2u

/* Interrupt bits. */
#define VINT_VRING      1u
#define VINT_CONFIG     2u

#define PAGE_SIZE       4096u

/* --- Split virtqueue (layout legacy) --- */
#define VQ_SIZE         128u

struct vq_desc {
    unsigned int addr_lo, addr_hi;
    unsigned int len;
    unsigned short flags, next;
} __attribute__((packed));

struct vq_avail {
    unsigned short flags, idx;
    unsigned short ring[VQ_SIZE];
    unsigned short used_event;
} __attribute__((packed));

struct vq_used_elem {
    unsigned int id, len;
} __attribute__((packed));

struct vq_used {
    unsigned short flags, idx;
    struct vq_used_elem ring[VQ_SIZE];
    unsigned short avail_event;
} __attribute__((packed));

/* Satu blok queue per arah, 4096-aligned. Ukuran: desc(2048) + avail(262)
 * -> pad ke 4096 -> used(1030). Total 8192 aman. */
#define VQ_BLOCK_SIZE 8192u
static unsigned char vq_rx_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));
static unsigned char vq_tx_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));

struct vqueue {
    struct vq_desc  *desc;
    struct vq_avail *avail;
    struct vq_used  *used;
    unsigned short avail_idx, used_idx;
};

/* --- State --- */
static volatile unsigned int *vmm;
static struct vqueue vq_rx, vq_tx;

#define RX_NBUF 32u
#define RX_BUFLEN 2048u
static unsigned char rx_buf[RX_NBUF][RX_BUFLEN] __attribute__((aligned(16)));
static unsigned char tx_buf[RX_BUFLEN] __attribute__((aligned(16)));

static unsigned char our_mac[6];
static void (*rx_cb)(const unsigned char *, unsigned);

/* Fase 12d: counter byte untuk bandwidth real (emulasi). Diakses hanya
 * dari konteks thread (net_send/net_poll), bukan IRQ, jadi aman. */
static unsigned long long net_rx_bytes;
static unsigned long long net_tx_bytes;

unsigned long long net_rx_bytes_get(void) { return net_rx_bytes; }
unsigned long long net_tx_bytes_get(void) { return net_tx_bytes; }

/* Logging UART langsung (PL011); kernel_main.c tidak mengekspor puts. */

static inline unsigned int mmio_r(unsigned int off)
{
    return vmm[off / 4u];
}
static inline void mmio_w(unsigned int off, unsigned int v)
{
    vmm[off / 4u] = v;
}
static void mem_barrier(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}
static void io_barrier(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
}

/* Inisialisasi satu vqueue legacy di blok yang disediakan. */
static int vq_setup(unsigned qsel, struct vqueue *vq, unsigned char *block)
{
    unsigned int qmax, qalign;
    unsigned long base = (unsigned long)block;
    unsigned long avail_end, used_base;
    unsigned i;

    /* Zero seluruh block (desc+avail+used). */
    for (i = 0; i < VQ_BLOCK_SIZE; i++)
        block[i] = 0u;

    mmio_w(VMM_QSEL, qsel);
    qmax = mmio_r(VMM_QNUMMAX);
    printf("[net] qmax=");
    printf("0x%x", qmax);
    printf("\n");
    if (qmax == 0u)
        return -1;
    if (qmax > VQ_SIZE)
        qmax = VQ_SIZE;
    qalign = mmio_r(VMM_QALIGN);
    printf("[net] qalign=");
    printf("0x%x", qalign);
    printf("\n");
    if (qalign == 0u)
        qalign = PAGE_SIZE;

    for (i = 0; i < VQ_BLOCK_SIZE / 4u; i++)
        ((unsigned int *)block)[i] = 0u;

    vq->desc = (struct vq_desc *)base;
    vq->avail = (struct vq_avail *)(base + 16u * VQ_SIZE);
    avail_end = base + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
    used_base = (avail_end + qalign - 1u) & ~(unsigned long)(qalign - 1u);
    vq->used = (struct vq_used *)used_base;
    vq->avail_idx = 0u;
    vq->used_idx = 0u;

    /* D-cache ON: dorong queue yang baru di-nol-kan ke RAM. */
    dcache_clean_range((unsigned int)base, VQ_BLOCK_SIZE);

    mmio_w(VMM_QNUM, qmax);
    mmio_w(VMM_QALIGN, qalign);
    mem_barrier();
    mmio_w(VMM_QPFN, (unsigned int)(base / PAGE_SIZE));
    return 0;
}

static void rx_post(unsigned bi)
{
    struct vqueue *vq = &vq_rx;
    unsigned short d = (unsigned short)bi;

    vq->desc[d].addr_lo = (unsigned int)(unsigned long)rx_buf[bi];
    vq->desc[d].addr_hi = 0u;
    vq->desc[d].len = RX_BUFLEN;
    vq->desc[d].flags = VD_WRITE;
    vq->desc[d].next = 0u;

    vq->avail->ring[vq->avail_idx % VQ_SIZE] = d;
    vq->avail_idx++;
    mem_barrier();
    vq->avail->idx = vq->avail_idx;
    /* D-cache ON: dorong deskriptor ke RAM sebelum notify; buffer RX
     * di-invalidate agar baca device tak kena cache basi. */
    dcache_clean_range((unsigned int)(unsigned long)&vq->desc[d],
                       sizeof(struct vq_desc));
    dcache_clean_range((unsigned int)(unsigned long)vq->avail,
                       sizeof(struct vq_avail));
    dcache_inval_range((unsigned int)(unsigned long)rx_buf[bi], RX_BUFLEN);
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 0u);
}

static void net_debug_queues(void);

int net_init(void)
{
    unsigned i;
    unsigned int feat;
    unsigned char status;

    vmm = 0;
    for (i = 0; i < VMM_SLOTS; i++) {
        volatile unsigned int *b =
            (volatile unsigned int *)(VMM_BASE + i * VMM_STRIDE);
        if (b[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
            continue;
        if (b[VMM_DEVID / 4u] != VMM_DEV_NET)
            continue;
        vmm = b;
        break;
    }
    if (!vmm) {
        printf("[net] virtio-net tidak ditemukan\n");
        return -1;
    }
    printf("[net] virtio-net @slot ");
    printf("0x%x", i);
    
    
    printf(" (legacy)\n");

    /* Reset, ACKNOWLEDGE, lalu DRIVER (baca-modify-write). */
    mmio_w(VMM_STATUS, 0u);
    mmio_w(VMM_STATUS, VST_ACK);
    status = (unsigned char)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_DRIVER);

    /* Guest page size (legacy). */
    mmio_w(VMM_GUESTPAGESZ, PAGE_SIZE);

    /* Negosiasi feature 32-bit: pilih set 0, baca host, tulis MAC saja. */
    mmio_w(VMM_HOSTFEATSEL, 0u);
    feat = mmio_r(VMM_HOSTFEAT);
    printf("[net] hostfeat=");
    printf("0x%x", feat);
    printf("\n");
    mmio_w(VMM_GUESTFEATSEL, 0u);
    mmio_w(VMM_GUESTFEAT, feat & VF_NET_MAC);

    status = (unsigned char)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_FEAT_OK);
    mem_barrier();

    /* Queue 0 = RX, 1 = TX. */
    if (vq_setup(0u, &vq_rx, vq_rx_block) ||
        vq_setup(1u, &vq_tx, vq_tx_block)) {
        printf("[net] queue setup gagal\n");
        return -3;
    }

    /* Baca MAC dari config space + link status. */
    for (i = 0; i < 6u; i++)
        our_mac[i] = ((volatile unsigned char *)((unsigned long)vmm + VMM_CONFIG))[i];
    {
        unsigned short link = ((volatile unsigned short *)((unsigned long)vmm + VMM_CONFIG + 6))[0];
        printf("[net] link status=");
        printf("0x%x", link);
        printf("\n");
    }
    printf("[net] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           our_mac[0], our_mac[1], our_mac[2],
           our_mac[3], our_mac[4], our_mac[5]);

    status |= VST_DRIVER_OK;
    mmio_w(VMM_STATUS, status);
    mem_barrier();
    if (!(mmio_r(VMM_STATUS) & VST_DRIVER_OK)) {
        printf("[net] DRIVER_OK gagal\n");
        return -4;
    }

    /* Post buffer RX awal, notify sekali. */
    for (i = 0; i < RX_NBUF; i++)
        rx_post(i);
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 0u);
    io_barrier();

    printf("[net] siap\n");
    net_debug_queues();
    return 0;
}

const unsigned char *net_mac(void)
{
    return our_mac;
}


static void net_debug_queues(void)
{
    printf("[dbg] rx desc=0x%x avail=0x%x used=0x%x\n",
           (unsigned int)(unsigned long)vq_rx.desc,
           (unsigned int)(unsigned long)vq_rx.avail,
           (unsigned int)(unsigned long)vq_rx.used);
    printf("[dbg] tx desc=0x%x avail=0x%x used=0x%x aidx=0x%x uidx=0x%x\n",
           (unsigned int)(unsigned long)vq_tx.desc,
           (unsigned int)(unsigned long)vq_tx.avail,
           (unsigned int)(unsigned long)vq_tx.used,
           vq_tx.avail->idx, vq_tx.used->idx);
}

void net_on_rx(void (*cb)(const unsigned char *, unsigned))
{
    rx_cb = cb;
}

/* Header virtio-net legacy: 10 byte nol untuk paket sederhana. */
#define VNET_HDR_LEN 10u

int net_send(const unsigned char *frame, unsigned len)
{
    struct vqueue *vq = &vq_tx;
    unsigned i;
    unsigned spin;
    volatile unsigned short *uidx;

    if (!vmm || !frame || len == 0u || len + VNET_HDR_LEN > RX_BUFLEN)
        return -1;

    /* TX sinkron 1 buffer: tunggu paket sebelumnya selesai dikirim
     * device sebelum menimpa tx_buf (kalau tidak, paket pertama
     * korup/hilang — balapan dengan device). */
    uidx = (volatile unsigned short *)&vq->used->idx;
    spin = 0u;
    while (vq->avail_idx != vq->used_idx && spin < 1000000u) {
        dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
        while (*uidx != vq->used_idx)
            vq->used_idx++;
        mem_barrier();
        spin++;
    }
    dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
    while (*uidx != vq->used_idx)
        vq->used_idx++;

    for (i = 0; i < VNET_HDR_LEN; i++)
        tx_buf[i] = 0u;
    for (i = 0; i < len; i++)
        tx_buf[VNET_HDR_LEN + i] = frame[i];

    vq->desc[0].addr_lo = (unsigned int)(unsigned long)tx_buf;
    vq->desc[0].addr_hi = 0u;
    vq->desc[0].len = len + VNET_HDR_LEN;
    vq->desc[0].flags = 0u;
    vq->desc[0].next = 0u;

    vq->avail->ring[vq->avail_idx % VQ_SIZE] = 0u;
    vq->avail_idx++;
    mem_barrier();
    vq->avail->idx = vq->avail_idx;

    /* D-cache ON: dorong deskriptor + data ke RAM sebelum notify. */
    dcache_clean_range((unsigned int)(unsigned long)vq->desc,
                       sizeof(struct vq_desc));
    dcache_clean_range((unsigned int)(unsigned long)vq->avail,
                       sizeof(struct vq_avail));
    dcache_clean_range((unsigned int)(unsigned long)tx_buf,
                       len + VNET_HDR_LEN);
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 1u);
    io_barrier();
    /* Fase 12d: hitung byte TX (frame Ethernet, tanpa header virtio). */
    net_tx_bytes += (unsigned long long)len;
    return 0;
}

/* Fase 12d: kembalikan jumlah paket yang diproses (RX + TX completion). */
unsigned net_poll(void)
{
    unsigned guard;
    unsigned work = 0u;
    volatile unsigned short *uidx;

    if (!vmm)
        return 0u;

    /* TX completions: invalidate used->idx tiap baca (device menulis). */
    uidx = (volatile unsigned short *)&vq_tx.used->idx;
    dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
    guard = 0u;
    while (*uidx != vq_tx.used_idx && guard < VQ_SIZE) {
        vq_tx.used_idx++;
        guard++;
        work++;
        dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
    }

    /* RX completions. */
    uidx = (volatile unsigned short *)&vq_rx.used->idx;
    dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
    guard = 0u;
    while (*uidx != vq_rx.used_idx && guard < VQ_SIZE) {
        struct vq_used_elem *e;
        unsigned bi, len;

        dcache_inval_range((unsigned int)(unsigned long)vq_rx.used,
                           sizeof(struct vq_used));
        e = &vq_rx.used->ring[vq_rx.used_idx % VQ_SIZE];
        bi = e->id;
        len = e->len;

        vq_rx.used_idx++;
        mem_barrier();
        if (bi < RX_NBUF && len <= RX_BUFLEN && rx_cb) {
            /* Tarik data yang ditulis device ke cache. */
            dcache_inval_range((unsigned int)(unsigned long)rx_buf[bi],
                               len);
            /* Lewati header virtio-net 10 byte. */
            if (len > VNET_HDR_LEN) {
                rx_cb(rx_buf[bi] + VNET_HDR_LEN, len - VNET_HDR_LEN);
                /* Fase 12d: hitung byte RX (frame Ethernet). */
                net_rx_bytes += (unsigned long long)(len - VNET_HDR_LEN);
            }
        }
        rx_post(bi);
        mem_barrier();
        mmio_w(VMM_QNOTIFY, 0u);
        io_barrier();
        guard++;
        work++;
        dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
    }
    return work;
}
