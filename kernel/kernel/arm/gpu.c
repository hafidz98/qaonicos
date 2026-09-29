/*
 * mach3/kernel/arm/gpu.c -- Driver virtio-gpu 2D via virtio-mmio LEGACY
 * (App A1: backend display QEMU untuk Qabot).
 *
 * Pola transport disalin dari net.c (Fase D, terbukti): probe slot
 * MMIO, init legacy (ACK/DRIVER/GUESTPAGESZ/feature/FEAT_OK/queue/
 * DRIVER_OK), satu split virtqueue, D-cache maintenance eksplisit
 * (D-cache ON di port Mach 3 ini).
 *
 * Protokol virtio-gpu (2D saja, tanpa 3D/virgl):
 *   RESOURCE_CREATE_2D(id=1, fmt=B8G8R8X8_UNORM, 240x240)
 *   RESOURCE_ATTACH_BACKING(id=1, 57 x halaman 4K)
 *   SET_SCANOUT(scanout=0, rect 240x240, id=1)
 *   per flush: TRANSFER_TO_HOST_2D(rect) + RESOURCE_FLUSH(rect)
 *
 * Backing guest = 240*240*4 byte (XRGB8888) di memori kernel;
 * program user mengirim strip RGB565 via SYS_DISPLAY_FLUSH, kernel
 * mengkonversi ke backing lalu transfer+flush rect strip tersebut.
 * Perintah setup memakai fence (tunggu respons OK); perintah
 * per-frame juga sinkron (fence) demi kesederhanaan — biayanya
 * kecil dibanding render.
 *
 * Register legacy: sama seperti net.c (0x000 magic ... 0x070 status).
 * Device ID GPU = 16.  Queue 0 = controlq (cursorq tidak dipakai).
 */

/* D-cache maintenance (pmap.c / blk.c). */
extern void	dcache_clean_range(unsigned int va, unsigned int len);
extern void	dcache_inval_range(unsigned int va, unsigned int len);
extern int	printf(const char *, ...);

/* --- Register virtio-mmio legacy (sama dengan net.c) --- */
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
#define VMM_STATUS      0x070u

#define VMM_BASE        0x0a000000u
#define VMM_STRIDE      0x200u
#define VMM_SLOTS       32u
#define VMM_MAGIC_VAL   0x74726976u
#define VMM_DEV_GPU     16u

#define VST_ACK         1u
#define VST_DRIVER      2u
#define VST_DRIVER_OK   4u
#define VST_FEAT_OK     8u

#define VD_NEXT         1u
#define VD_WRITE        2u

#define PAGE_SIZE       4096u

/* --- Split virtqueue (layout legacy, pola net.c) --- */
#define VQ_SIZE         64u

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

/* Blok queue: desc(1024) + avail(134) -> pad 4096 -> used(518). */
#define VQ_BLOCK_SIZE 8192u
static unsigned char vq_ctl_block[VQ_BLOCK_SIZE] __attribute__((aligned(4096)));

struct vqueue {
    struct vq_desc  *desc;
    struct vq_avail *avail;
    struct vq_used  *used;
    unsigned short avail_idx, used_idx;
};

/* --- Protokol virtio-gpu --- */
#define GPU_CMD_GET_DISPLAY_INFO      0x0100u
#define GPU_CMD_RESOURCE_CREATE_2D   0x0101u
#define GPU_CMD_SET_SCANOUT          0x0103u
#define GPU_CMD_RESOURCE_FLUSH       0x0104u
#define GPU_CMD_TRANSFER_TO_HOST_2D  0x0105u
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u

#define GPU_RESP_OK_NODATA           0x1100u
#define GPU_FLAG_FENCE               0x00000001u

#define GPU_FORMAT_B8G8R8X8_UNORM    2u

struct gpu_ctrl_hdr {
    unsigned int type;
    unsigned int flags;
    unsigned int fence_lo, fence_hi;
    unsigned int ctx_id;
    unsigned char ring_idx;
    unsigned char pad[3];
} __attribute__((packed));

struct gpu_rect {
    unsigned int x, y, w, h;
} __attribute__((packed));

/* Panel Qabot: 240x240 (sama dengan renderer face). */
#define GPU_W 240u
#define GPU_H 240u
#define GPU_RES_ID 1u

/* Backing XRGB8888 penuh di memori kernel (VA==PA, pola net.c).
 * 240*240*4 = 230400 byte = 57 halaman. */
#define GPU_BACKING_BYTES (GPU_W * GPU_H * 4u)
#define GPU_BACKING_PAGES ((GPU_BACKING_BYTES + PAGE_SIZE - 1u) / PAGE_SIZE)
static unsigned int gpu_backing[GPU_W * GPU_H] __attribute__((aligned(4096)));

/* Buffer perintah + respons (satu outstanding; semua pemanggil dari
 * konteks thread, tak ada IRQ driver ini). */
#define GPU_REQ_SIZE 2048u
static unsigned char gpu_req[GPU_REQ_SIZE] __attribute__((aligned(16)));
static unsigned char gpu_resp[64] __attribute__((aligned(16)));

static volatile unsigned int *vmm;
static struct vqueue vq_ctl;
static unsigned int gpu_fence = 1u;
static int gpu_available = 0;

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

int gpu_available_p(void) { return gpu_available; }

/* Tunggu hingga used_idx mengejar avail_idx (dengan batas spin agar
 * tak hang selamanya bila device tak merespons). */
static int vq_wait_used(void)
{
    unsigned long spin = 0;
    volatile unsigned short *uidx =
        (volatile unsigned short *)&vq_ctl.used->idx;

    for (;;) {
        dcache_inval_range((unsigned int)(unsigned long)uidx, 2);
        if (*uidx == vq_ctl.avail_idx)
            return 0;
        if (++spin > 20000000ul)
            return -1;
    }
}

/* Kirim satu perintah controlq. req_len byte dari gpu_req (device
 * read-only); bila want_resp, sediakan deskriptor tulis 24 byte ke
 * gpu_resp dan kembalikan tipe respons (atau <0 bila gagal). */
static int gpu_cmd(unsigned int req_len, int want_resp)
{
    struct vqueue *vq = &vq_ctl;
    unsigned short d0 = 0u, d1 = 1u;

    vq->desc[d0].addr_lo = (unsigned int)(unsigned long)gpu_req;
    vq->desc[d0].addr_hi = 0u;
    vq->desc[d0].len = req_len;
    vq->desc[d0].flags = want_resp ? VD_NEXT : 0u;
    vq->desc[d0].next = d1;

    if (want_resp) {
        vq->desc[d1].addr_lo = (unsigned int)(unsigned long)gpu_resp;
        vq->desc[d1].addr_hi = 0u;
        vq->desc[d1].len = sizeof(gpu_resp);
        vq->desc[d1].flags = VD_WRITE;
        vq->desc[d1].next = 0u;
        dcache_inval_range((unsigned int)(unsigned long)gpu_resp,
                           sizeof(gpu_resp));
    }

    vq->avail->ring[vq->avail_idx % VQ_SIZE] = d0;
    vq->avail_idx++;
    mem_barrier();
    vq->avail->idx = vq->avail_idx;

    dcache_clean_range((unsigned int)(unsigned long)vq->desc,
                       2u * sizeof(struct vq_desc));
    dcache_clean_range((unsigned int)(unsigned long)vq->avail,
                       sizeof(struct vq_avail));
    dcache_clean_range((unsigned int)(unsigned long)gpu_req, req_len);
    mem_barrier();
    mmio_w(VMM_QNOTIFY, 0u);
    io_barrier();

    if (vq_wait_used() != 0)
        return -1;
    vq->used_idx = vq->avail_idx;

    if (want_resp) {
        struct gpu_ctrl_hdr *rh =
            (struct gpu_ctrl_hdr *)(unsigned long)gpu_resp;
        dcache_inval_range((unsigned int)(unsigned long)gpu_resp,
                           sizeof(gpu_resp));
        return (int)rh->type;
    }
    return 0;
}

static void hdr_init(struct gpu_ctrl_hdr *h, unsigned int type)
{
    h->type = type;
    h->flags = GPU_FLAG_FENCE;
    h->fence_lo = gpu_fence++;
    h->fence_hi = 0u;
    h->ctx_id = 0u;
    h->ring_idx = 0u;
    h->pad[0] = h->pad[1] = h->pad[2] = 0u;
}

static int vq_setup(struct vqueue *vq, unsigned char *block)
{
    unsigned int qmax, qalign;
    unsigned long base = (unsigned long)block;
    unsigned long avail_end, used_base;
    unsigned i;

    for (i = 0; i < VQ_BLOCK_SIZE; i++)
        block[i] = 0u;

    mmio_w(VMM_QSEL, 0u);
    qmax = mmio_r(VMM_QNUMMAX);
    if (qmax == 0u)
        return -1;
    if (qmax > VQ_SIZE)
        qmax = VQ_SIZE;
    qalign = mmio_r(VMM_QALIGN);
    if (qalign == 0u)
        qalign = PAGE_SIZE;

    vq->desc = (struct vq_desc *)base;
    vq->avail = (struct vq_avail *)(base + 16u * VQ_SIZE);
    avail_end = base + 16u * VQ_SIZE + 6u + 2u * VQ_SIZE;
    used_base = (avail_end + qalign - 1u) & ~(unsigned long)(qalign - 1u);
    vq->used = (struct vq_used *)used_base;
    vq->avail_idx = 0u;
    vq->used_idx = 0u;

    dcache_clean_range((unsigned int)base, VQ_BLOCK_SIZE);

    mmio_w(VMM_QNUM, qmax);
    mmio_w(VMM_QALIGN, qalign);
    mem_barrier();
    mmio_w(VMM_QPFN, (unsigned int)(base / PAGE_SIZE));
    return 0;
}

int gpu_init(void)
{
    unsigned i;
    unsigned int feat;
    unsigned char status;
    struct {
        struct gpu_ctrl_hdr hdr;
        unsigned int resource_id;
        unsigned int format;
        unsigned int width;
        unsigned int height;
    } __attribute__((packed)) *c2d;
    struct {
        struct gpu_ctrl_hdr hdr;
        unsigned int resource_id;
        unsigned int nr_entries;
    } __attribute__((packed)) *ab;
    struct {
        unsigned int addr_lo, addr_hi;
        unsigned int length;
        unsigned int pad;
    } __attribute__((packed)) *ent;
    struct {
        struct gpu_ctrl_hdr hdr;
        struct gpu_rect r;
        unsigned int scanout_id;
        unsigned int resource_id;
    } __attribute__((packed)) *sso;
    int rc;

    vmm = 0;
    for (i = 0; i < VMM_SLOTS; i++) {
        volatile unsigned int *b =
            (volatile unsigned int *)(VMM_BASE + i * VMM_STRIDE);
        if (b[VMM_MAGIC / 4u] != VMM_MAGIC_VAL)
            continue;
        if (b[VMM_DEVID / 4u] != VMM_DEV_GPU)
            continue;
        vmm = b;
        break;
    }
    if (!vmm) {
        printf("[gpu] virtio-gpu tidak ditemukan (jalan tanpa display)\n");
        return -1;
    }
    printf("[gpu] virtio-gpu @slot 0x%x (legacy)\n", i);

    mmio_w(VMM_STATUS, 0u);
    mmio_w(VMM_STATUS, VST_ACK);
    status = (unsigned char)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_DRIVER);
    mmio_w(VMM_GUESTPAGESZ, PAGE_SIZE);

    /* Feature: tak butuh apa pun (2D dasar). */
    mmio_w(VMM_HOSTFEATSEL, 0u);
    feat = mmio_r(VMM_HOSTFEAT);
    printf("[gpu] hostfeat=0x%x\n", feat);
    mmio_w(VMM_GUESTFEATSEL, 0u);
    mmio_w(VMM_GUESTFEAT, 0u);

    status = (unsigned char)mmio_r(VMM_STATUS);
    mmio_w(VMM_STATUS, status | VST_FEAT_OK);
    mem_barrier();

    if (vq_setup(&vq_ctl, vq_ctl_block)) {
        printf("[gpu] queue setup gagal\n");
        return -3;
    }

    status |= VST_DRIVER_OK;
    mmio_w(VMM_STATUS, status);
    mem_barrier();
    if (!(mmio_r(VMM_STATUS) & VST_DRIVER_OK)) {
        printf("[gpu] DRIVER_OK gagal\n");
        return -4;
    }

    /* RESOURCE_CREATE_2D(id=1, B8G8R8X8, 240x240). */
    c2d = (void *)gpu_req;
    hdr_init(&c2d->hdr, GPU_CMD_RESOURCE_CREATE_2D);
    c2d->resource_id = GPU_RES_ID;
    c2d->format = GPU_FORMAT_B8G8R8X8_UNORM;
    c2d->width = GPU_W;
    c2d->height = GPU_H;
    rc = gpu_cmd(sizeof(*c2d), 1);
    if (rc != (int)GPU_RESP_OK_NODATA) {
        printf("[gpu] CREATE_2D gagal (resp 0x%x)\n", rc);
        return -5;
    }

    /* RESOURCE_ATTACH_BACKING: 57 entri halaman 4K. */
    ab = (void *)gpu_req;
    hdr_init(&ab->hdr, GPU_CMD_RESOURCE_ATTACH_BACKING);
    ab->resource_id = GPU_RES_ID;
    ab->nr_entries = GPU_BACKING_PAGES;
    ent = (void *)(ab + 1);
    for (i = 0; i < GPU_BACKING_PAGES; i++) {
        unsigned int off = i * PAGE_SIZE;
        unsigned int len = PAGE_SIZE;
        if (off + len > GPU_BACKING_BYTES)
            len = GPU_BACKING_BYTES - off;
        ent[i].addr_lo = (unsigned int)(unsigned long)gpu_backing + off;
        ent[i].addr_hi = 0u;
        ent[i].length = len;
        ent[i].pad = 0u;
    }
    rc = gpu_cmd(sizeof(*ab) + GPU_BACKING_PAGES * sizeof(*ent), 1);
    if (rc != (int)GPU_RESP_OK_NODATA) {
        printf("[gpu] ATTACH_BACKING gagal (resp 0x%x)\n", rc);
        return -6;
    }

    /* SET_SCANOUT(0, 240x240, id=1). */
    sso = (void *)gpu_req;
    hdr_init(&sso->hdr, GPU_CMD_SET_SCANOUT);
    sso->r.x = 0u; sso->r.y = 0u;
    sso->r.w = GPU_W; sso->r.h = GPU_H;
    sso->scanout_id = 0u;
    sso->resource_id = GPU_RES_ID;
    rc = gpu_cmd(sizeof(*sso), 1);
    if (rc != (int)GPU_RESP_OK_NODATA) {
        printf("[gpu] SET_SCANOUT gagal (resp 0x%x)\n", rc);
        return -7;
    }

    gpu_available = 1;
    printf("[gpu] siap: 240x240 XRGB8888 (resource 1, scanout 0)\n");
    return 0;
}

/* Konversi strip RGB565 (user) -> backing XRGB8888, lalu
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH untuk rect (x,y,w,h).
 * 0 = ok, -1 = gagal/tak tersedia. */
int gpu_flush_strip(unsigned int x, unsigned int y,
                    unsigned int w, unsigned int h,
                    const unsigned short *rgb565)
{
    struct {
        struct gpu_ctrl_hdr hdr;
        struct gpu_rect r;
        unsigned int off_lo, off_hi;
        unsigned int resource_id;
        unsigned int pad;
    } __attribute__((packed)) *t2h;
    struct {
        struct gpu_ctrl_hdr hdr;
        struct gpu_rect r;
        unsigned int resource_id;
        unsigned int pad;
    } __attribute__((packed)) *fl;
    unsigned int ix, iy;
    int rc;

    if (!gpu_available)
        return -1;
    if (x + w > GPU_W || y + h > GPU_H || w == 0u || h == 0u)
        return -1;

    /* RGB565 -> B8G8R8X8 (byte order little-endian: B,G,R,X). */
    for (iy = 0; iy < h; iy++) {
        unsigned int *dst = &gpu_backing[(y + iy) * GPU_W + x];
        const unsigned short *src = &rgb565[iy * w];
        for (ix = 0; ix < w; ix++) {
            unsigned int p = src[ix];
            unsigned int r = (p >> 11u) & 0x1fu;
            unsigned int g = (p >> 5u) & 0x3fu;
            unsigned int b = p & 0x1fu;
            /* Ekspansi ke 8 bit (replikasi bit atas). */
            r = (r << 3u) | (r >> 2u);
            g = (g << 2u) | (g >> 4u);
            b = (b << 3u) | (b >> 2u);
            dst[ix] = (r << 16u) | (g << 8u) | b;
        }
    }
    dcache_clean_range((unsigned int)(unsigned long)
                       &gpu_backing[y * GPU_W + x],
                       h * GPU_W * 4u);

    /* TRANSFER_TO_HOST_2D(rect): offset = posisi rect di backing. */
    t2h = (void *)gpu_req;
    hdr_init(&t2h->hdr, GPU_CMD_TRANSFER_TO_HOST_2D);
    t2h->r.x = x; t2h->r.y = y;
    t2h->r.w = w; t2h->r.h = h;
    t2h->off_lo = (y * GPU_W + x) * 4u;
    t2h->off_hi = 0u;
    t2h->resource_id = GPU_RES_ID;
    t2h->pad = 0u;
    rc = gpu_cmd(sizeof(*t2h), 1);
    if (rc != (int)GPU_RESP_OK_NODATA)
        return -1;

    /* RESOURCE_FLUSH(rect). */
    fl = (void *)gpu_req;
    hdr_init(&fl->hdr, GPU_CMD_RESOURCE_FLUSH);
    fl->r.x = x; fl->r.y = y;
    fl->r.w = w; fl->r.h = h;
    fl->resource_id = GPU_RES_ID;
    fl->pad = 0u;
    rc = gpu_cmd(sizeof(*fl), 1);
    return (rc == (int)GPU_RESP_OK_NODATA) ? 0 : -1;
}
