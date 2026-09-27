/*
 * blk.c - Driver virtio-blk via virtio-mmio LEGACY (versi 1), Fase 12d.
 *
 * Pola yang sama dengan net.c (driver ditulis mandiri agar net.c yang
 * sudah terverifikasi tak tersentuh): probe 32 slot MMIO untuk DeviceID
 * 2 (block), negosiasi feature kosong, 1 virtqueue, I/O sinkron per
 * sektor 512B dengan polling used ring.
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
static uint8_t vq_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));

static struct bq_desc *bq_desc;
static struct bq_avail *bq_avail;
static struct bq_used *bq_used;
static uint16_t bq_avail_idx, bq_used_idx;

/* Header request 16B + 1 sektor data + status. */
static uint8_t req_hdr[16] __attribute__((aligned(16)));
static uint8_t req_data[BLK_SECTOR] __attribute__((aligned(16)));
static volatile uint8_t req_status;

static volatile uint32_t *vmm;

/* Kapasitas (sektor) dari config device; alokasi bump kernel. */
static uint32_t total_sectors;
static uint64_t next_free;   /* sektor 0 = superblock */

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

/* Jalankan 1 request (sinkron, polling). type = BLK_T_IN/BLK_T_OUT.
 * Untuk WRITE, data 512B disalin dulu ke req_data. Return 0 bila
 * status device = BLK_S_OK. */
static int blk_request(uint32_t type, uint64_t sector, const uint8_t *wdata)
{
    unsigned i, spin;

    if (!vmm || sector >= total_sectors)
        return -1;

    /* Header: type, ioprio=0, sector (little-endian, ARM sudah LE). */
    req_hdr[0] = (uint8_t)type;
    req_hdr[1] = req_hdr[2] = req_hdr[3] = 0u;
    req_hdr[4] = req_hdr[5] = req_hdr[6] = req_hdr[7] = 0u;
    for (i = 0; i < 8; i++)
        req_hdr[8 + i] = (uint8_t)(sector >> (i * 8));
    if (type == BLK_T_OUT && wdata)
        for (i = 0; i < BLK_SECTOR; i++)
            req_data[i] = wdata[i];
    req_status = 0xFFu;

    /* Rantai deskriptor: hdr -> data -> status. */
    bq_desc[0].addr_lo = (uint32_t)(uintptr_t)req_hdr;
    bq_desc[0].addr_hi = 0u;
    bq_desc[0].len = 16u;
    bq_desc[0].flags = VD_NEXT;
    bq_desc[0].next = 1u;
    bq_desc[1].addr_lo = (uint32_t)(uintptr_t)req_data;
    bq_desc[1].addr_hi = 0u;
    bq_desc[1].len = BLK_SECTOR;
    bq_desc[1].flags = (type == BLK_T_IN) ? (VD_NEXT | VD_WRITE) : VD_NEXT;
    bq_desc[1].next = 2u;
    bq_desc[2].addr_lo = (uint32_t)(uintptr_t)&req_status;
    bq_desc[2].addr_hi = 0u;
    bq_desc[2].len = 1u;
    bq_desc[2].flags = VD_WRITE;
    bq_desc[2].next = 0u;

    bq_avail->ring[bq_avail_idx % VQ_SIZE] = 0u;
    bq_avail_idx++;
    mem_barrier();
    bq_avail->idx = bq_avail_idx;
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 0u);
    __asm__ volatile("dsb ish" ::: "memory");

    /* Tunggu completion (polling; QEMU cepat untuk file raw).
     * Baca used->idx via pointer volatile agar compiler tidak hoist
     * read keluar loop (device menulis via DMA). */
    {
        volatile uint16_t *uidx = (volatile uint16_t *)&bq_used->idx;
        spin = 0u;
        while (*uidx == bq_used_idx && spin < 10000000u)
            spin++;
        if (*uidx == bq_used_idx)
            return -2;              /* timeout */
        bq_used_idx = *uidx;
    }
    mem_barrier();
    if (req_status != BLK_S_OK) {
        blk_log("[blk] DBG status=");
        blk_loghex(req_status);
        blk_log("\n");
    }
    return (req_status == BLK_S_OK) ? 0 : -3;
}

int blk_write(uint64_t sector, const uint8_t *data)
{
    return blk_request(BLK_T_OUT, sector, data);
}

int blk_read(uint64_t sector, uint8_t *data)
{
    unsigned i;
    int r = blk_request(BLK_T_IN, sector, 0);
    if (r == 0 && data)
        for (i = 0; i < BLK_SECTOR; i++)
            data[i] = req_data[i];
    return r;
}

uint32_t blk_total_sectors(void) { return total_sectors; }

/* Sektor 0..next_free-1 dipakai kernel. */
uint32_t blk_used_sectors(void)
{
    uint64_t u = next_free;
    if (u > 0xFFFFFFFFu)
        u = 0xFFFFFFFFu;
    return (uint32_t)u;
}

/* Bump allocator: 1 sektor. Sektor 0 = superblock (dipesan di init). */
uint64_t blk_alloc(void)
{
    uint64_t s = next_free;
    if (s < total_sectors)
        next_free++;
    return s;
}

/* Superblock QaonicOS di sektor 0: magic + kapasitas + versi. */
#define SB_MAGIC0 0x51414f4eu   /* "QAON" */
#define SB_MAGIC1 0x424c4b31u   /* "BLK1" */

static void sb_write(uint8_t *sb)
{
    unsigned i;
    for (i = 0; i < BLK_SECTOR; i++)
        sb[i] = 0u;
    sb[0] = 'Q'; sb[1] = 'A'; sb[2] = 'O'; sb[3] = 'N';
    sb[4] = 'B'; sb[5] = 'L'; sb[6] = 'K'; sb[7] = '1';
    for (i = 0; i < 4; i++) {
        sb[8 + i] = (uint8_t)(total_sectors >> (i * 8));
        sb[12 + i] = (uint8_t)(SB_MAGIC0 >> (i * 8));
        sb[16 + i] = (uint8_t)(SB_MAGIC1 >> (i * 8));
    }
}

static int sb_check(const uint8_t *sb)
{
    uint32_t m0 = 0u, m1 = 0u, cap = 0u;
    unsigned i;
    if (sb[0] != 'Q' || sb[1] != 'A' || sb[2] != 'O' || sb[3] != 'N' ||
        sb[4] != 'B' || sb[5] != 'L' || sb[6] != 'K' || sb[7] != '1')
        return -1;
    for (i = 0; i < 4; i++) {
        cap |= (uint32_t)sb[8 + i] << (i * 8);
        m0 |= (uint32_t)sb[12 + i] << (i * 8);
        m1 |= (uint32_t)sb[16 + i] << (i * 8);
    }
    if (m0 != SB_MAGIC0 || m1 != SB_MAGIC1 || cap != total_sectors)
        return -1;
    return 0;
}

int blk_init(void)
{
    unsigned i, qmax, qalign;
    uintptr_t base, avail_end, used_base;
    uint8_t status;
    uint32_t cap_lo, cap_hi;
    uint64_t cap;
    static uint8_t sb[BLK_SECTOR] __attribute__((aligned(16)));

    vmm = 0;
    for (i = 0; i < VMM_SLOTS; i++) {
        volatile uint32_t *b =
            (volatile uint32_t *)(VMM_BASE + i * VMM_STRIDE);
        if (b[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
            continue;
        if (b[VMM_DEVID / 4u] != VMM_DEV_BLK)
            continue;
        vmm = b;
        break;
    }
    if (!vmm) {
        blk_log("[blk] virtio-blk tidak ditemukan\n");
        return -1;
    }
    blk_log("[blk] virtio-blk @slot ");
    blk_loghex(i);
    blk_log("\n");

    mmio_w(VMM_STATUS, 0u);
    mmio_w(VMM_STATUS, VST_ACK);
    status = (uint8_t)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_DRIVER);
    mmio_w(VMM_GUESTPAGESZ, PAGE_SIZE);

    /* Feature: tak butuh apa-apa; tetap lakukan dance FEATURES_OK. */
    mmio_w(VMM_HOSTFEATSEL, 0u);
    (void)mmio_r(VMM_HOSTFEAT);
    mmio_w(VMM_GUESTFEATSEL, 0u);
    mmio_w(VMM_GUESTFEAT, 0u);
    status = (uint8_t)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_FEAT_OK);
    mem_barrier();

    /* Satu queue (queue 0). */
    mmio_w(VMM_QSEL, 0u);
    qmax = mmio_r(VMM_QNUMMAX);
    if (qmax == 0u) {
        blk_log("[blk] qmax=0\n");
        return -3;
    }
    if (qmax > VQ_SIZE)
        qmax = VQ_SIZE;
    qalign = mmio_r(VMM_QALIGN);
    if (qalign == 0u)
        qalign = PAGE_SIZE;
    for (i = 0; i < VQ_BLOCK_SIZE / 4u; i++)
        ((uint32_t *)vq_block)[i] = 0u;
    base = (uintptr_t)vq_block;
    bq_desc = (struct bq_desc *)base;
    bq_avail = (struct bq_avail *)(base + 16u * VQ_SIZE);
    avail_end = base + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
    used_base = (avail_end + qalign - 1u) & ~(uintptr_t)(qalign - 1u);
    bq_used = (struct bq_used *)used_base;
    bq_avail_idx = 0u;
    bq_used_idx = 0u;
    mmio_w(VMM_QNUM, qmax);
    mmio_w(VMM_QALIGN, qalign);
    mem_barrier();
    mmio_w(VMM_QPFN, (uint32_t)(base / PAGE_SIZE));

    /* Kapasitas REAL dari config device (u64, satuan sektor 512B). */
    cap_lo = ((volatile uint32_t *)((uintptr_t)vmm + VMM_CONFIG))[0];
    cap_hi = ((volatile uint32_t *)((uintptr_t)vmm + VMM_CONFIG))[1];
    cap = ((uint64_t)cap_hi << 32) | cap_lo;
    if (cap == 0u || cap > 0xFFFFFFFFu) {
        blk_log("[blk] kapasitas aneh\n");
        return -4;
    }
    total_sectors = (uint32_t)cap;
    blk_log("[blk] kapasitas ");
    blk_loghex(total_sectors);
    blk_log(" sektor (");
    blk_loghex(total_sectors / 2048u);
    blk_log(" MB)\n");

    status |= VST_DRIVER_OK;
    mmio_w(VMM_STATUS, status);
    mem_barrier();
    if (!(mmio_r(VMM_STATUS) & VST_DRIVER_OK)) {
        blk_log("[blk] DRIVER_OK gagal\n");
        return -5;
    }

    /* Superblock: tulis sektor 0, baca balik, verifikasi (bukti I/O). */
    next_free = 1u;             /* sektor 0 dipesan untuk superblock */
    sb_write(sb);
    if (blk_write(0u, sb) != 0) {
        blk_log("[blk] tulis superblock gagal\n");
        return -6;
    }
    for (i = 0; i < BLK_SECTOR; i++)
        sb[i] = 0u;
    if (blk_read(0u, sb) != 0 || sb_check(sb) != 0) {
        blk_log("[blk] verifikasi superblock gagal\n");
        return -7;
    }
    blk_log("[blk] superblock OK (write+read verify)\n");
    return 0;
}

/* --- Fase 12d: access log HTTP (append-only, best-effort) ---
 * Sektor 1..127, 16 entri x 32B per sektor. Tiap request HTTP menambah
 * 1 entri teks "t=<ticks> c=<code>", sehingga "used sectors" di
 * dashboard bergerak mengikuti aktivitas nyata (bukan angka statis).
 * Kegagalan tulis diabaikan (best-effort, jangan ganggu HTTP). */
#define ALOG_FIRST 1u
#define ALOG_LAST  127u
#define ALOG_ENT   16u
#define ALOG_ESZ   32u

static uint8_t alog_buf[BLK_SECTOR];
static unsigned alog_pos;
static uint64_t alog_sec;   /* 0 = belum dialokasi */

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
    uint8_t *e;
    unsigned i;

    if (!vmm || total_sectors == 0u)
        return;                     /* disk tak ada: no-op */
    if (alog_sec == 0u) {
        alog_sec = blk_alloc();
        if (alog_sec < ALOG_FIRST || alog_sec > ALOG_LAST)
            alog_sec = ALOG_FIRST;  /* ring penuh: tulis ulang dari awal */
        for (i = 0; i < BLK_SECTOR; i++)
            alog_buf[i] = 0u;
        alog_pos = 0u;
    }
    e = alog_buf + alog_pos * ALOG_ESZ;
    i = 0;
    e[i++] = 't'; e[i++] = '=';
    i = dec_app(e, i, ticks);
    e[i++] = ' '; e[i++] = 'c'; e[i++] = '=';
    i = dec_app(e, i, (uint32_t)code);
    e[i++] = '\n';
    while (i < ALOG_ESZ)
        e[i++] = ' ';
    alog_pos++;
    if (alog_pos >= ALOG_ENT) {
        if (blk_write(alog_sec, alog_buf) == 0) {
            if (alog_sec < ALOG_LAST)
                alog_sec = blk_alloc();
            else
                alog_sec = ALOG_FIRST;   /* ring penuh: putar ulang */
        }
        for (i = 0; i < BLK_SECTOR; i++)
            alog_buf[i] = 0u;
        alog_pos = 0u;
    }
}
