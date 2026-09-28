/*
 * blk.c - Driver virtio-blk via virtio-mmio LEGACY (versi 1).
 *
 * Fase 12d: satu device (pico128.img, storage internal emulasi SPI NAND).
 * Fase 15: abstraksi blkdev multi-device —
 *   dev 0 = storage internal (pico128.img, superblock "QAONBLK1",
 *           access-log HTTP append-only sektor 1-127 — perilaku Fase 12d
 *           UTUH, semua API blk_* tetap melayani dev 0);
 *   dev 1 = kartu SD (sd128.img, superblock "QAONSD01", raw sector I/O
 *           via sd_read/sd_write).
 *
 * Pola yang sama dengan net.c (driver ditulis mandiri agar net.c yang
 * sudah terverifikasi tak tersentuh): probe 32 slot MMIO untuk DeviceID
 * 2 (block), negosiasi feature kosong, 1 virtqueue per device, I/O
 * sinkron per sektor 512B dengan polling used ring.
 *
 * Urutan slot = urutan -device di command line QEMU: run-qemu.sh
 * memasang hd0 (pico128.img) SEBELUM sd0 (sd128.img), sehingga probe
 * slot-naik memberi dev 0 = pico128.img, dev 1 = sd128.img.
 *
 * Format request virtio-blk:
 *   desc[0]: header 16B (type u32, ioprio u32, sector u64), device-read
 *   desc[1]: data 512B (device-write utk READ, device-read utk WRITE)
 *   desc[2]: status 1B (device-write), 0 = VIRTIO_BLK_S_OK
 */
#include "blk.h"

/* --- Register virtio-mmio legacy (sama dengan net.c) --- */
#define VMM_MAGIC       0x000u
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
#define VMM_VERSION     0x004u
#define VMM_DEV_BLK     2u

#define VST_ACK         1u
#define VST_DRIVER      2u
#define VST_DRIVER_OK   4u
#define VST_FEAT_OK     8u

#define VD_NEXT         1u
#define VD_WRITE        2u

#define PAGE_SIZE       4096u
#define BLK_SECTOR      512u

/* Tipe request virtio-blk. */
#define BLK_T_IN        0u
#define BLK_T_OUT       1u
#define BLK_S_OK        0u

/* --- Split virtqueue (layout legacy, sama dengan net.c) --- */
#define VQ_SIZE         128u

struct bq_desc {
    uint32_t addr_lo, addr_hi;
    uint32_t len;
    uint16_t flags, next;
} __attribute__((packed));

/* Avail & used ring (layout natural sudah pas; tanpa packed agar
 * &used->idx boleh diambil untuk polling volatile). */
struct bq_avail {
    uint16_t flags, idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
};

struct bq_used_elem {
    uint32_t id, len;
} __attribute__((packed));

struct bq_used {
    uint16_t flags, idx;
    struct bq_used_elem ring[VQ_SIZE];
    uint16_t avail_event;
};

#define VQ_BLOCK_SIZE 8192u

/* --- Fase 15: state per-device (blkdev). --- */
struct blk_dev {
    volatile uint32_t *vmm;
    struct bq_desc *desc;
    struct bq_avail *avail;
    struct bq_used *used;
    uint16_t avail_idx, used_idx;
    /* Header request 16B + 1 sektor data + status. */
    uint8_t req_hdr[16] __attribute__((aligned(16)));
    uint8_t req_data[BLK_SECTOR] __attribute__((aligned(16)));
    volatile uint8_t req_status;
    /* Kapasitas (sektor) dari config device; alokasi bump kernel. */
    uint32_t total_sectors;
    uint64_t next_free;   /* sektor 0 = superblock */
    /* Peran blkdev (diisi blk_init): 0 = storage internal,
     * 1 = SD card, -1 = belum teridentifikasi. */
    int role;
    /* Access log HTTP (Fase 12d): hanya dipakai dev 0. */
    uint8_t alog_buf[BLK_SECTOR];
    unsigned alog_pos;
    uint64_t alog_sec;    /* 0 = belum dialokasi */
};

static uint8_t vq_block[BLK_MAXDEV][VQ_BLOCK_SIZE]
    __attribute__((aligned(4096)));
static struct blk_dev devs[BLK_MAXDEV];
static unsigned ndevs;

/* Logging UART langsung (PL011). */
#define B_UARTDR (*(volatile uint32_t *)0x09000000u)
#define B_UARTFR (*(volatile uint32_t *)0x09000018u)
static void bputc(char c)
{
    while (B_UARTFR & (1u << 5)) { }
    B_UARTDR = (uint32_t)(unsigned char)c;
}
void blk_log(const char *s)
{
    while (*s) {
        if (*s == '\n') bputc('\r');
        bputc(*s++);
    }
}
void blk_loghex(uint32_t v)
{
    int i;
    static const char hd[] = "0123456789abcdef";
    blk_log("0x");
    for (i = 7; i >= 0; i--)
        bputc(hd[(v >> (i * 4)) & 0xfu]);
}

static inline uint32_t mmio_r(struct blk_dev *d, uint32_t off)
{
    return d->vmm[off / 4u];
}
static inline void mmio_w(struct blk_dev *d, uint32_t off, uint32_t v)
{
    d->vmm[off / 4u] = v;
}
static void mem_barrier(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

/* Jalankan 1 request (sinkron, polling). type = BLK_T_IN/BLK_T_OUT.
 * Untuk WRITE, data 512B disalin dulu ke req_data device. Return 0 bila
 * status device = BLK_S_OK. */
static int blk_request(struct blk_dev *d, uint32_t type, uint64_t sector,
                       const uint8_t *wdata)
{
    unsigned i, spin;

    if (!d->vmm || sector >= d->total_sectors)
        return -1;

    /* Header: type, ioprio=0, sector (little-endian, ARM sudah LE). */
    d->req_hdr[0] = (uint8_t)type;
    d->req_hdr[1] = d->req_hdr[2] = d->req_hdr[3] = 0u;
    d->req_hdr[4] = d->req_hdr[5] = d->req_hdr[6] = d->req_hdr[7] = 0u;
    for (i = 0; i < 8; i++)
        d->req_hdr[8 + i] = (uint8_t)(sector >> (i * 8));
    if (type == BLK_T_OUT && wdata)
        for (i = 0; i < BLK_SECTOR; i++)
            d->req_data[i] = wdata[i];
    d->req_status = 0xFFu;

    /* Rantai deskriptor: hdr -> data -> status. */
    d->desc[0].addr_lo = (uint32_t)(uintptr_t)d->req_hdr;
    d->desc[0].addr_hi = 0u;
    d->desc[0].len = 16u;
    d->desc[0].flags = VD_NEXT;
    d->desc[0].next = 1u;
    d->desc[1].addr_lo = (uint32_t)(uintptr_t)d->req_data;
    d->desc[1].addr_hi = 0u;
    d->desc[1].len = BLK_SECTOR;
    d->desc[1].flags = (type == BLK_T_IN) ? (VD_NEXT | VD_WRITE) : VD_NEXT;
    d->desc[1].next = 2u;
    d->desc[2].addr_lo = (uint32_t)(uintptr_t)&d->req_status;
    d->desc[2].addr_hi = 0u;
    d->desc[2].len = 1u;
    d->desc[2].flags = VD_WRITE;
    d->desc[2].next = 0u;

    d->avail->ring[d->avail_idx % VQ_SIZE] = 0u;
    d->avail_idx++;
    mem_barrier();
    d->avail->idx = d->avail_idx;
    mem_barrier();
    mmio_w(d, VMM_QNOTIFY, 0u);
    __asm__ volatile("dsb ish" ::: "memory");

    /* Tunggu completion (polling; QEMU cepat untuk file raw).
     * Baca used->idx via pointer volatile agar compiler tidak hoist
     * read keluar loop (device menulis via DMA). */
    {
        volatile uint16_t *uidx = (volatile uint16_t *)&d->used->idx;
        spin = 0u;
        while (*uidx == d->used_idx && spin < 10000000u)
            spin++;
        if (*uidx == d->used_idx)
            return -2;              /* timeout */
        d->used_idx = *uidx;
    }
    mem_barrier();
    if (d->req_status != BLK_S_OK) {
        blk_log("[blk] DBG status=");
        blk_loghex(d->req_status);
        blk_log("\n");
    }
    return (d->req_status == BLK_S_OK) ? 0 : -3;
}

static int dev_write(struct blk_dev *d, uint64_t sector, const uint8_t *data)
{
    return blk_request(d, BLK_T_OUT, sector, data);
}

static int dev_read(struct blk_dev *d, uint64_t sector, uint8_t *data)
{
    unsigned i;
    int r = blk_request(d, BLK_T_IN, sector, 0);
    if (r == 0 && data)
        for (i = 0; i < BLK_SECTOR; i++)
            data[i] = d->req_data[i];
    return r;
}

/* Cari device berdasarkan peran (0 = storage, 1 = SD). */
static struct blk_dev *dev_by_role(unsigned role)
{
    unsigned i;
    for (i = 0; i < ndevs; i++)
        if (devs[i].role == (int)role && devs[i].vmm)
            return &devs[i];
    return 0;
}

/* --- API dev 0 (storage internal): SEMANTIK SAMA DENGAN Fase 12d. --- */

int blk_write(uint64_t sector, const uint8_t *data)
{
    struct blk_dev *d = dev_by_role(0u);
    return d ? dev_write(d, sector, data) : -1;
}

int blk_read(uint64_t sector, uint8_t *data)
{
    struct blk_dev *d = dev_by_role(0u);
    return d ? dev_read(d, sector, data) : -1;
}

uint32_t blk_total_sectors(void)
{
    struct blk_dev *d = dev_by_role(0u);
    return d ? d->total_sectors : 0u;
}

/* Sektor 0..next_free-1 dipakai kernel. */
uint32_t blk_used_sectors(void)
{
    struct blk_dev *d = dev_by_role(0u);
    uint64_t u;
    if (!d)
        return 0u;
    u = d->next_free;
    if (u > 0xFFFFFFFFu)
        u = 0xFFFFFFFFu;
    return (uint32_t)u;
}

/* Bump allocator: 1 sektor. Sektor 0 = superblock (dipesan di init). */
uint64_t blk_alloc(void)
{
    struct blk_dev *d = dev_by_role(0u);
    uint64_t s;
    if (!d)
        return 0u;
    s = d->next_free;
    if (s < d->total_sectors)
        d->next_free++;
    return s;
}

/* --- Fase 15: API dev 1 (kartu SD, raw sector I/O). --- */

int sd_present(void)
{
    return dev_by_role(1u) ? 1 : 0;
}

uint32_t sd_total_sectors(void)
{
    struct blk_dev *d = dev_by_role(1u);
    return d ? d->total_sectors : 0u;
}

int sd_write(uint64_t sector, const uint8_t *data)
{
    struct blk_dev *d = dev_by_role(1u);
    return d ? dev_write(d, sector, data) : -1;
}

int sd_read(uint64_t sector, uint8_t *data)
{
    struct blk_dev *d = dev_by_role(1u);
    return d ? dev_read(d, sector, data) : -1;
}

/* --- Superblock ---
 * dev 0: "QAONBLK1" (storage internal, Fase 12d).
 * dev 1: "QAONSD01" (kartu SD, Fase 15).
 * Layout: byte 0..7 magic ASCII, 8..11 kapasitas (u32 LE),
 * 12..15 & 16..19 dua magic u32, 20..511 nol. */

static void sb_write(uint8_t *sb, const char *magic, uint32_t cap,
                     uint32_t m0, uint32_t m1)
{
    unsigned i;
    for (i = 0; i < BLK_SECTOR; i++)
        sb[i] = 0u;
    for (i = 0; i < 8; i++)
        sb[i] = (uint8_t)magic[i];
    for (i = 0; i < 4; i++) {
        sb[8 + i] = (uint8_t)(cap >> (i * 8));
        sb[12 + i] = (uint8_t)(m0 >> (i * 8));
        sb[16 + i] = (uint8_t)(m1 >> (i * 8));
    }
}

static int sb_check(const uint8_t *sb, const char *magic, uint32_t cap,
                    uint32_t m0, uint32_t m1)
{
    uint32_t c = 0u, a = 0u, b = 0u;
    unsigned i;
    for (i = 0; i < 8; i++)
        if (sb[i] != (uint8_t)magic[i])
            return -1;
    for (i = 0; i < 4; i++) {
        c |= (uint32_t)sb[8 + i] << (i * 8);
        a |= (uint32_t)sb[12 + i] << (i * 8);
        b |= (uint32_t)sb[16 + i] << (i * 8);
    }
    if (a != m0 || b != m1 || c != cap)
        return -1;
    return 0;
}

/* Inisialisasi satu transport virtio-blk (slot sudah ditemukan).
 * HANYA negosiasi + queue + baca kapasitas; superblock ditulis di
 * fase identifikasi (blk_identify) setelah peran device diketahui. */
static int dev_transport_init(struct blk_dev *d, volatile uint32_t *base,
                              const char *tag)
{
    unsigned i, qmax, qalign;
    uintptr_t basea, avail_end, used_base;
    uint8_t status;
    uint32_t cap_lo, cap_hi;
    uint64_t cap;

    d->vmm = base;
    d->avail_idx = 0u;
    d->used_idx = 0u;
    d->alog_pos = 0u;
    d->alog_sec = 0u;
    d->total_sectors = 0u;

    mmio_w(d, VMM_STATUS, 0u);
    mmio_w(d, VMM_STATUS, VST_ACK);
    status = (uint8_t)mmio_r(d, VMM_STATUS);
    mmio_w(d, VMM_STATUS, status | VST_DRIVER);
    mmio_w(d, VMM_GUESTPAGESZ, PAGE_SIZE);

    /* Feature: tak butuh apa-apa; tetap lakukan dance FEATURES_OK. */
    mmio_w(d, VMM_HOSTFEATSEL, 0u);
    (void)mmio_r(d, VMM_HOSTFEAT);
    mmio_w(d, VMM_GUESTFEATSEL, 0u);
    mmio_w(d, VMM_GUESTFEAT, 0u);
    status = (uint8_t)mmio_r(d, VMM_STATUS);
    mmio_w(d, VMM_STATUS, status | VST_FEAT_OK);
    mem_barrier();

    /* Satu queue (queue 0). */
    mmio_w(d, VMM_QSEL, 0u);
    qmax = mmio_r(d, VMM_QNUMMAX);
    if (qmax == 0u) {
        blk_log("[blk] ");
        blk_log(tag);
        blk_log(" qmax=0\n");
        return -3;
    }
    if (qmax > VQ_SIZE)
        qmax = VQ_SIZE;
    qalign = mmio_r(d, VMM_QALIGN);
    if (qalign == 0u)
        qalign = PAGE_SIZE;
    /* Blok antrian milik device ini (tiap device punya blok sendiri,
     * tidak sharing seperti Fase 12d — perlu untuk 2 device). */
    basea = (uintptr_t)&vq_block[(unsigned)(d - devs)][0];
    for (i = 0; i < VQ_BLOCK_SIZE / 4u; i++)
        ((uint32_t *)basea)[i] = 0u;
    d->desc = (struct bq_desc *)basea;
    d->avail = (struct bq_avail *)(basea + 16u * VQ_SIZE);
    avail_end = basea + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
    used_base = (avail_end + qalign - 1u) & ~(uintptr_t)(qalign - 1u);
    d->used = (struct bq_used *)used_base;
    mmio_w(d, VMM_QNUM, qmax);
    mmio_w(d, VMM_QALIGN, qalign);
    mem_barrier();
    mmio_w(d, VMM_QPFN, (uint32_t)(basea / PAGE_SIZE));

    /* Kapasitas REAL dari config device (u64, satuan sektor 512B). */
    cap_lo = ((volatile uint32_t *)((uintptr_t)d->vmm + VMM_CONFIG))[0];
    cap_hi = ((volatile uint32_t *)((uintptr_t)d->vmm + VMM_CONFIG))[1];
    cap = ((uint64_t)cap_hi << 32) | cap_lo;
    if (cap == 0u || cap > 0xFFFFFFFFu) {
        blk_log("[blk] ");
        blk_log(tag);
        blk_log(" kapasitas aneh\n");
        return -4;
    }
    d->total_sectors = (uint32_t)cap;
    blk_log("[blk] ");
    blk_log(tag);
    blk_log(" kapasitas ");
    blk_loghex(d->total_sectors);
    blk_log(" sektor (");
    blk_loghex(d->total_sectors / 2048u);
    blk_log(" MB)\n");

    status |= VST_DRIVER_OK;
    mmio_w(d, VMM_STATUS, status);
    mem_barrier();
    if (!(mmio_r(d, VMM_STATUS) & VST_DRIVER_OK)) {
        blk_log("[blk] ");
        blk_log(tag);
        blk_log(" DRIVER_OK gagal\n");
        return -5;
    }
    return 0;
}

/* Tulis + verifikasi read-back superblock sektor 0 device d. */
static int dev_write_sb(struct blk_dev *d, const char *tag,
                        const char *magic, uint32_t m0, uint32_t m1)
{
    unsigned i;
    static uint8_t sb[BLK_SECTOR] __attribute__((aligned(16)));

    d->next_free = 1u;          /* sektor 0 dipesan untuk superblock */
    sb_write(sb, magic, d->total_sectors, m0, m1);
    if (dev_write(d, 0u, sb) != 0) {
        blk_log("[blk] ");
        blk_log(tag);
        blk_log(" tulis superblock gagal\n");
        return -6;
    }
    for (i = 0; i < BLK_SECTOR; i++)
        sb[i] = 0u;
    if (dev_read(d, 0u, sb) != 0 ||
        sb_check(sb, magic, d->total_sectors, m0, m1) != 0) {
        blk_log("[blk] ");
        blk_log(tag);
        blk_log(" verifikasi superblock gagal\n");
        return -7;
    }
    blk_log("[blk] ");
    blk_log(tag);
    blk_log(" superblock OK (write+read verify)\n");
    return 0;
}

/* Identifikasi peran device dari magic superblock sektor 0 yang
 * SUDAH ADA (ditulis boot sebelumnya):
 *   "QAONBLK1" -> storage internal (dev 0)
 *   "QAONSD01" -> SD card (dev 1)
 * Kembalikan 0 = storage, 1 = SD, -1 = belum ada superblock valid. */
static int dev_identify(struct blk_dev *d)
{
    unsigned i;
    static uint8_t s0[BLK_SECTOR] __attribute__((aligned(16)));

    for (i = 0; i < BLK_SECTOR; i++)
        s0[i] = 0u;
    if (dev_read(d, 0u, s0) != 0)
        return -1;
    if (sb_check(s0, "QAONBLK1", d->total_sectors,
                 0x51414f4eu, 0x424c4b31u) == 0)
        return 0;
    if (sb_check(s0, "QAONSD01", d->total_sectors,
                 0x51414f4eu, 0x53443031u) == 0)
        return 1;
    return -1;
}

int blk_init(void)
{
    unsigned i;
    unsigned ntrans = 0u;
    unsigned slot_of[BLK_MAXDEV];
    unsigned n_unid, hi;

    ndevs = 0u;
    for (i = 0; i < BLK_MAXDEV; i++) {
        devs[i].vmm = 0;
        devs[i].total_sectors = 0u;
        devs[i].role = -1;
        slot_of[i] = 0u;
    }

    /* 1. Probe semua slot; transport yang hidup di-setup (tanpa
     * superblock dulu — peran belum diketahui). */
    for (i = 0; i < VMM_SLOTS && ntrans < BLK_MAXDEV; i++) {
        volatile uint32_t *b =
            (volatile uint32_t *)(VMM_BASE + i * VMM_STRIDE);
        if (b[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
            continue;
        if (b[VMM_DEVID / 4u] != VMM_DEV_BLK)
            continue;
        if (dev_transport_init(&devs[ntrans], b, "blk") != 0) {
            devs[ntrans].vmm = 0;
            continue;
        }
        slot_of[ntrans] = i;
        ntrans++;
    }
    if (ntrans == 0u) {
        blk_log("[blk] virtio-blk tidak ditemukan\n");
        return -1;
    }

    /* 2. Identifikasi peran dari superblock sektor 0 yang SUDAH ADA
     * (ditulis boot sebelumnya). */
    for (i = 0; i < ntrans; i++)
        devs[i].role = dev_identify(&devs[i]);

    /* 3. Transport yang belum teridentifikasi (image baru / satu disk
     * saja ala Fase 12d): */
    n_unid = 0u;
    for (i = 0; i < ntrans; i++)
        if (devs[i].role < 0)
            n_unid++;
    if (n_unid == 2u) {
        /* Boot pertama dengan dua image baru: transport di slot
         * TERTINGGI = storage (dev 0). Sesuai observasi QEMU 8.2.2:
         * "-device" TERAKHIR di command line mendapat slot MMIO
         * TERENDAH; run-qemu.sh memasang hd0 (pico128.img) SETELAH
         * hd1 (sd128.img). */
        hi = slot_of[0] > slot_of[1] ? 0u : 1u;
        devs[hi].role = 0;
        devs[1u - hi].role = 1;
    } else {
        for (i = 0; i < ntrans; i++) {
            unsigned r, taken;
            if (devs[i].role >= 0)
                continue;
            /* Peran yang belum dipakai transport lain. */
            taken = 0u;
            for (r = 0; r < ntrans; r++)
                if (r != i && devs[r].role >= 0)
                    taken |= 1u << (unsigned)devs[r].role;
            devs[i].role = (taken & 1u) ? 1 : 0;
        }
    }

    /* 4. Tulis superblock sesuai peran (bukti I/O + penanda untuk
     * boot berikutnya). */
    ndevs = ntrans;
    {
        struct blk_dev *st = dev_by_role(0u);
        struct blk_dev *sd = dev_by_role(1u);
        if (!st) {
            blk_log("[blk] storage (dev 0) tidak teridentifikasi\n");
            return -1;
        }
        if (dev_write_sb(st, "storage", "QAONBLK1",
                         0x51414f4eu, 0x424c4b31u) != 0)
            return -1;
        if (sd) {
            if (dev_write_sb(sd, "sd", "QAONSD01",
                             0x51414f4eu, 0x53443031u) != 0) {
                blk_log("[blk] SD gagal superblock; lanjut tanpa SD\n");
                sd->role = -1;
                sd->vmm = 0;
            }
        } else {
            blk_log("[blk] SD card (dev 1) tidak ditemukan; lanjut tanpa SD\n");
        }
    }
    return 0;
}

/* --- Fase 12d: access log HTTP (append-only, best-effort) ---
 * Dev 0, sektor 1..127, 16 entri x 32B per sektor. Tiap request HTTP
 * menambah 1 entri teks "t=<ticks> c=<code>", sehingga "used sectors"
 * di dashboard bergerak mengikuti aktivitas nyata (bukan angka statis).
 * Kegagalan tulis diabaikan (best-effort, jangan ganggu HTTP). */
#define ALOG_FIRST 1u
#define ALOG_LAST  127u
#define ALOG_ENT   16u
#define ALOG_ESZ   32u

static unsigned dec_app(uint8_t *d, unsigned i, uint32_t v)
{
    char tmp[10];
    int n = 0, k;
    if (v == 0u)
        tmp[n++] = '0';
    else
        while (v > 0u) {
            tmp[n++] = (char)('0' + v % 10u);
            v /= 10u;
        }
    for (k = n - 1; k >= 0; k--)
        d[i++] = (uint8_t)tmp[k];
    return i;
}

void blk_log_request(uint32_t ticks, unsigned code)
{
    struct blk_dev *d = dev_by_role(0u);
    uint8_t *e;
    unsigned i;

    if (!d)
        return;                     /* disk tak ada: no-op */
    if (!d->vmm || d->total_sectors == 0u)
        return;
    if (d->alog_sec == 0u) {
        d->alog_sec = blk_alloc();
        if (d->alog_sec < ALOG_FIRST || d->alog_sec > ALOG_LAST)
            d->alog_sec = ALOG_FIRST;   /* ring penuh: tulis ulang dari awal */
        for (i = 0; i < BLK_SECTOR; i++)
            d->alog_buf[i] = 0u;
        d->alog_pos = 0u;
    }
    e = d->alog_buf + d->alog_pos * ALOG_ESZ;
    i = 0;
    e[i++] = 't'; e[i++] = '=';
    i = dec_app(e, i, ticks);
    e[i++] = ' '; e[i++] = 'c'; e[i++] = '=';
    i = dec_app(e, i, (uint32_t)code);
    e[i++] = '\n';
    while (i < ALOG_ESZ)
        e[i++] = ' ';
    d->alog_pos++;
    if (d->alog_pos >= ALOG_ENT) {
        if (dev_write(d, d->alog_sec, d->alog_buf) == 0) {
            if (d->alog_sec < ALOG_LAST)
                d->alog_sec = blk_alloc();
            else
                d->alog_sec = ALOG_FIRST;   /* ring penuh: putar ulang */
        }
        for (i = 0; i < BLK_SECTOR; i++)
            d->alog_buf[i] = 0u;
        d->alog_pos = 0u;
    }
}
