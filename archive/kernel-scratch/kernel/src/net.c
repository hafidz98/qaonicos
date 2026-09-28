/*
 * net.c - Driver virtio-net via virtio-mmio LEGACY (versi 1), Fase 11.
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
 * virtio-mmio slots: 0x0a000000 + i*0x200 (32 slot), GIC SPI 16+i
 *   -> ID interrupt 48+i. QEMU menaruh device di slot 31.
 */
#include "net.h"
#include "board.h"
#include "sched.h"   /* Fase 12d: sched_wakeup_net() dari net_isr */

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
    uint32_t addr_lo, addr_hi;
    uint32_t len;
    uint16_t flags, next;
} __attribute__((packed));

struct vq_avail {
    uint16_t flags, idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
} __attribute__((packed));

struct vq_used_elem {
    uint32_t id, len;
} __attribute__((packed));

struct vq_used {
    uint16_t flags, idx;
    struct vq_used_elem ring[VQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed));

/* Satu blok queue per arah, 4096-aligned. Ukuran: desc(2048) + avail(262)
 * -> pad ke 4096 -> used(1030). Total 8192 aman. */
#define VQ_BLOCK_SIZE 8192u
static uint8_t vq_rx_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));
static uint8_t vq_tx_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));

struct vqueue {
    struct vq_desc  *desc;
    struct vq_avail *avail;
    struct vq_used  *used;
    uint16_t avail_idx, used_idx;
};

/* --- State --- */
static volatile uint32_t *vmm;
static unsigned vmm_irq;
static struct vqueue vq_rx, vq_tx;

#define RX_NBUF 32u
#define RX_BUFLEN 2048u
static uint8_t rx_buf[RX_NBUF][RX_BUFLEN] __attribute__((aligned(16)));
static uint8_t tx_buf[RX_BUFLEN] __attribute__((aligned(16)));

static uint8_t our_mac[6];
static void (*rx_cb)(const uint8_t *, unsigned);
static volatile unsigned rx_pending;

/* Fase 12d: counter byte untuk bandwidth real (emulasi). Diakses hanya
 * dari konteks thread (net_send/net_poll), bukan IRQ, jadi aman. */
static uint64_t net_rx_bytes;
static uint64_t net_tx_bytes;

uint64_t net_rx_bytes_get(void) { return net_rx_bytes; }
uint64_t net_tx_bytes_get(void) { return net_tx_bytes; }

/* Logging UART langsung (PL011); kernel_main.c tidak mengekspor puts. */
#define N_UARTDR (*(volatile uint32_t *)0x09000000u)
#define N_UARTFR (*(volatile uint32_t *)0x09000018u)
static void nputc(char c)
{
    while (N_UARTFR & (1u << 5)) { }
    N_UARTDR = (uint32_t)(unsigned char)c;
}
void net_log(const char *str)
{
    while (*str) {
        if (*str == '\n') nputc('\r');
        nputc(*str++);
    }
}
void net_loghex(uint32_t v)
{
    int i;
    static const char hd[] = "0123456789abcdef";
    net_log("0x");
    for (i = 7; i >= 0; i--)
        nputc(hd[(v >> (i * 4)) & 0xfu]);
}

static inline uint32_t mmio_r(uint32_t off)
{
    return vmm[off / 4u];
}
static inline void mmio_w(uint32_t off, uint32_t v)
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
static int vq_setup(unsigned qsel, struct vqueue *vq, uint8_t *block)
{
    uint32_t qmax, qalign;
    uintptr_t base = (uintptr_t)block;
    uintptr_t avail_end, used_base;
    unsigned i;

    /* Zero seluruh block (desc+avail+used). */
    for (i = 0; i < VQ_BLOCK_SIZE; i++)
        block[i] = 0u;

    mmio_w(VMM_QSEL, qsel);
    qmax = mmio_r(VMM_QNUMMAX);
    net_log("[net] qmax=");
    net_loghex(qmax);
    net_log("\n");
    if (qmax == 0u)
        return -1;
    if (qmax > VQ_SIZE)
        qmax = VQ_SIZE;
    qalign = mmio_r(VMM_QALIGN);
    net_log("[net] qalign=");
    net_loghex(qalign);
    net_log("\n");
    if (qalign == 0u)
        qalign = PAGE_SIZE;

    for (i = 0; i < VQ_BLOCK_SIZE / 4u; i++)
        ((uint32_t *)block)[i] = 0u;

    vq->desc = (struct vq_desc *)base;
    vq->avail = (struct vq_avail *)(base + 16u * VQ_SIZE);
    avail_end = base + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
    used_base = (avail_end + qalign - 1u) & ~(uintptr_t)(qalign - 1u);
    vq->used = (struct vq_used *)used_base;
    vq->avail_idx = 0u;
    vq->used_idx = 0u;

    mmio_w(VMM_QNUM, qmax);
    mmio_w(VMM_QALIGN, qalign);
    mem_barrier();
    mmio_w(VMM_QPFN, (uint32_t)(base / PAGE_SIZE));
    return 0;
}

static void rx_post(unsigned bi)
{
    struct vqueue *vq = &vq_rx;
    uint16_t d = (uint16_t)bi;

    vq->desc[d].addr_lo = (uint32_t)(uintptr_t)rx_buf[bi];
    vq->desc[d].addr_hi = 0u;
    vq->desc[d].len = RX_BUFLEN;
    vq->desc[d].flags = VD_WRITE;
    vq->desc[d].next = 0u;

    vq->avail->ring[vq->avail_idx % VQ_SIZE] = d;
    vq->avail_idx++;
    mem_barrier();
    vq->avail->idx = vq->avail_idx;
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 0u);
}

static void net_debug_queues(void);

int net_init(void)
{
    unsigned i;
    uint32_t feat;
    uint8_t status;

    vmm = 0;
    for (i = 0; i < VMM_SLOTS; i++) {
        volatile uint32_t *b =
            (volatile uint32_t *)(VMM_BASE + i * VMM_STRIDE);
        if (b[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
            continue;
        if (b[VMM_DEVID / 4u] != VMM_DEV_NET)
            continue;
        vmm = b;
        vmm_irq = 48u + i;          /* GIC SPI 16+i -> ID 32+16+i */
        break;
    }
    if (!vmm) {
        net_log("[net] virtio-net tidak ditemukan\n");
        return -1;
    }
    net_log("[net] virtio-net @slot ");
    net_loghex(i);
    net_log(" irq ");
    net_loghex(vmm_irq);
    net_log(" (legacy)\n");

    /* Reset, ACKNOWLEDGE, lalu DRIVER (baca-modify-write). */
    mmio_w(VMM_STATUS, 0u);
    mmio_w(VMM_STATUS, VST_ACK);
    status = (uint8_t)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_DRIVER);

    /* Guest page size (legacy). */
    mmio_w(VMM_GUESTPAGESZ, PAGE_SIZE);

    /* Negosiasi feature 32-bit: pilih set 0, baca host, tulis MAC saja. */
    mmio_w(VMM_HOSTFEATSEL, 0u);
    feat = mmio_r(VMM_HOSTFEAT);
    net_log("[net] hostfeat=");
    net_loghex(feat);
    net_log("\n");
    mmio_w(VMM_GUESTFEATSEL, 0u);
    mmio_w(VMM_GUESTFEAT, feat & VF_NET_MAC);

    status = (uint8_t)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_FEAT_OK);
    mem_barrier();

    /* Queue 0 = RX, 1 = TX. */
    if (vq_setup(0u, &vq_rx, vq_rx_block) ||
        vq_setup(1u, &vq_tx, vq_tx_block)) {
        net_log("[net] queue setup gagal\n");
        return -3;
    }

    /* Baca MAC dari config space + link status. */
    for (i = 0; i < 6u; i++)
        our_mac[i] = ((volatile uint8_t *)((uintptr_t)vmm + VMM_CONFIG))[i];
    {
        uint16_t link = ((volatile uint16_t *)((uintptr_t)vmm + VMM_CONFIG + 6))[0];
        net_log("[net] link status=");
        net_loghex(link);
        net_log("\n");
    }
    net_log("[net] MAC ");
    for (i = 0; i < 6u; i++) {
        uint8_t b = our_mac[i];
        nputc("0123456789abcdef"[b >> 4]);
        nputc("0123456789abcdef"[b & 0xfu]);
        if (i < 5u)
            nputc(':');
    }
    net_log("\n");

    status |= VST_DRIVER_OK;
    mmio_w(VMM_STATUS, status);
    mem_barrier();
    if (!(mmio_r(VMM_STATUS) & VST_DRIVER_OK)) {
        net_log("[net] DRIVER_OK gagal\n");
        return -4;
    }

    /* Post buffer RX awal, notify sekali. */
    for (i = 0; i < RX_NBUF; i++)
        rx_post(i);
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 0u);
    io_barrier();

    net_log("[net] siap\n");
    net_debug_queues();
    return 0;
}

const uint8_t *net_mac(void)
{
    return our_mac;
}

unsigned net_irq(void)
{
    return vmm_irq;
}

static void net_debug_queues(void)
{
    net_log("[dbg] rx desc=");
    net_loghex((uint32_t)(uintptr_t)vq_rx.desc);
    net_log(" avail=");
    net_loghex((uint32_t)(uintptr_t)vq_rx.avail);
    net_log(" used=");
    net_loghex((uint32_t)(uintptr_t)vq_rx.used);
    net_log("\n[dbg] tx desc=");
    net_loghex((uint32_t)(uintptr_t)vq_tx.desc);
    net_log(" avail=");
    net_loghex((uint32_t)(uintptr_t)vq_tx.avail);
    net_log(" used=");
    net_loghex((uint32_t)(uintptr_t)vq_tx.used);
    net_log(" aidx=");
    net_loghex(vq_tx.avail->idx);
    net_log(" uidx=");
    net_loghex(vq_tx.used->idx);
    net_log("\n");
}

void net_on_rx(void (*cb)(const uint8_t *, unsigned))
{
    rx_cb = cb;
}

void net_isr(void)
{
    uint32_t s = mmio_r(VMM_INTSTAT);

    mmio_w(VMM_INTACK, s);
    if (s & (VINT_VRING | VINT_CONFIG)) {
        rx_pending = 1u;
        /* Fase 12d: paket tiba -> bangunkan thread net bila ter-block. */
        sched_wakeup_net();
    }
}

/* Header virtio-net legacy: 10 byte nol untuk paket sederhana. */
#define VNET_HDR_LEN 10u

int net_send(const uint8_t *frame, unsigned len)
{
    struct vqueue *vq = &vq_tx;
    unsigned i;
    unsigned spin;

    if (!vmm || !frame || len == 0u || len + VNET_HDR_LEN > RX_BUFLEN)
        return -1;

    /* TX sinkron 1 buffer: tunggu paket sebelumnya selesai dikirim
     * device sebelum menimpa tx_buf (kalau tidak, paket pertama
     * korup/hilang — balapan dengan device). */
    spin = 0u;
    while (vq->avail_idx != vq->used_idx && spin < 1000000u) {
        while (vq->used->idx != vq->used_idx)
            vq->used_idx++;
        mem_barrier();
        spin++;
    }
    while (vq->used->idx != vq->used_idx)
        vq->used_idx++;

    for (i = 0; i < VNET_HDR_LEN; i++)
        tx_buf[i] = 0u;
    for (i = 0; i < len; i++)
        tx_buf[VNET_HDR_LEN + i] = frame[i];

    vq->desc[0].addr_lo = (uint32_t)(uintptr_t)tx_buf;
    vq->desc[0].addr_hi = 0u;
    vq->desc[0].len = len + VNET_HDR_LEN;
    vq->desc[0].flags = 0u;
    vq->desc[0].next = 0u;

    vq->avail->ring[vq->avail_idx % VQ_SIZE] = 0u;
    vq->avail_idx++;
    mem_barrier();
    vq->avail->idx = vq->avail_idx;
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 1u);
    io_barrier();
    /* Fase 12d: hitung byte TX (frame Ethernet, tanpa header virtio). */
    net_tx_bytes += (uint64_t)len;
    return 0;
}

/* Fase 12d: kembalikan jumlah paket yang diproses (RX + TX completion)
 * agar thread net tahu kapan boleh block (tak ada kerja). */
unsigned net_poll(void)
{
    unsigned guard;
    unsigned work = 0u;

    if (!vmm)
        return 0u;
    rx_pending = 0u;
    mem_barrier();

    /* TX completions. */
    guard = 0u;
    while (vq_tx.used->idx != vq_tx.used_idx && guard < VQ_SIZE) {
        vq_tx.used_idx++;
        guard++;
        work++;
    }

    /* RX completions. */
    guard = 0u;
    while (vq_rx.used->idx != vq_rx.used_idx && guard < VQ_SIZE) {
        struct vq_used_elem *e =
            &vq_rx.used->ring[vq_rx.used_idx % VQ_SIZE];
        unsigned bi = e->id;
        unsigned len = e->len;

        vq_rx.used_idx++;
        mem_barrier();
        if (bi < RX_NBUF && len <= RX_BUFLEN && rx_cb) {
            /* Lewati header virtio-net 10 byte. */
            if (len > VNET_HDR_LEN) {
                rx_cb(rx_buf[bi] + VNET_HDR_LEN, len - VNET_HDR_LEN);
                /* Fase 12d: hitung byte RX (frame Ethernet). */
                net_rx_bytes += (uint64_t)(len - VNET_HDR_LEN);
            }
        }
        rx_post(bi);
        mem_barrier();
        mmio_w(VMM_QNOTIFY, 0u);
        io_barrier();
        guard++;
        work++;
    }
    return work;
}
