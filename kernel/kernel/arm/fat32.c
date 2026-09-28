/*
 * mach3/kernel/arm/fat32.c -- Filesystem FAT32 read+write di atas
 * kartu SD (Fase D).
 *
 * Port dari archive/kernel-scratch/kernel/src/fat32.c (terbukti di
 * kernel lama).  Backend: sd_read/sd_write (blkdev dev 1, sektor
 * 512B).  Image dibuat di host oleh tools/mkfat32.py: 128MB,
 * 8 sektor/cluster, 2 FAT, root cluster 2, cluster terakhir BAD.
 *
 * Struktur kode:
 *   - BPB parse saat mount (fat32_mount).
 *   - Cache 1 sektor FAT (fat_get/fat_set + flush).
 *   - Path "/sd/..." -> parent cluster + nama 8.3 (uppercase).
 *   - Operasi direktori: find_in_dir, create_entry (extend chain
 *     bila penuh), alloc/free cluster chain.
 *   - File: write = create/truncate + alokasi seperlunya;
 *     read = ikuti chain; delete = free chain + tandai 0xE5.
 *
 * Batasan: 8.3 saja (LFN diabaikan), tanpa timestamp/atribut.
 */

/* Backend SD (blk.c). */
extern int	sd_present(void);
extern int	sd_read(unsigned int sector, unsigned char *data);
extern int	sd_write(unsigned int sector, const unsigned char *data);

/* --- Offset BPB (boot sector) --- */
#define BS_BYTS_PER_SEC 11u
#define BS_SEC_PER_CLUS 13u
#define BS_RSVD_CNT     14u
#define BS_NUM_FATS     16u
#define BS_TOT_SEC16    19u
#define BS_FATSZ16      22u
#define BS_TOT_SEC32    32u
#define BS_FATSZ32      36u
#define BS_ROOT_CLUS    44u
#define BS_FSINFO       48u
#define BS_SIG          510u

/* --- Entri direktori 32B --- */
#define DE_NAME   0u
#define DE_ATTR   11u
#define DE_CLHI   20u
#define DE_CLLO   26u
#define DE_SIZE   28u

#define ATTR_DIR  0x10u
#define ATTR_VOL  0x08u
#define ATTR_LFN  0x0Fu

#define FAT_EOC   0x0FFFFFFFu
#define FAT_BAD   0x0FFFFFF7u
#define FAT_FREE  0x00000000u

/* --- State mount --- */
static int      m_mounted;
static unsigned int m_spc;          /* sektor per cluster */
static unsigned int m_reserved;
static unsigned int m_nfats;
static unsigned int m_fatsz;        /* sektor per FAT */
static unsigned int m_root;         /* cluster root dir */
static unsigned int m_data;         /* sektor pertama area data */
static unsigned int m_nclus;        /* jumlah cluster data */
static unsigned int m_fsinfo;       /* sektor FSInfo */
static unsigned int m_nfree = 0xFFFFFFFFu;
static unsigned int m_hint = 2u;    /* hint alokasi cluster */

/* --- Buffer statis (aman: syscall tak preemptible) --- */
static unsigned char  sec_scratch[512] __attribute__((aligned(16)));
static unsigned char  fat_sec[512]     __attribute__((aligned(16)));
static unsigned int fat_cached = 0xFFFFFFFFu;
static int      fat_dirty;

static unsigned short rd16(const unsigned char *p)
{
    return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}
static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static void wr16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static unsigned int clus_first_sec(unsigned int cl)
{
    return m_data + (cl - 2u) * m_spc;
}
static int clus_valid(unsigned int cl)
{
    return cl >= 2u && cl < 2u + m_nclus;
}

/* --- Cache FAT --- */
static void fat_flush(void)
{
    unsigned int i, base;
    if (!fat_dirty || fat_cached == 0xFFFFFFFFu)
        return;
    base = m_reserved;          /* FAT pertama */
    for (i = 0u; i < m_nfats; i++)
        if (sd_write(base + i * m_fatsz + fat_cached, fat_sec) != 0)
            break;              /* best-effort */
    fat_dirty = 0;
}

static unsigned int fat_get(unsigned int cl)
{
    unsigned int sec = cl / 128u;   /* 128 entri u32 per sektor */
    if (sec != fat_cached) {
        fat_flush();
        if (sd_read(m_reserved + sec, fat_sec) != 0)
            return FAT_BAD;     /* gagal baca -> anggap tak bisa dipakai */
        fat_cached = sec;
    }
    return rd32(fat_sec + (cl % 128u) * 4u) & 0x0FFFFFFFu;
}

static void fat_set(unsigned int cl, unsigned int v)
{
    unsigned int sec = cl / 128u;
    unsigned int cur;
    if (sec != fat_cached) {
        fat_flush();
        if (sd_read(m_reserved + sec, fat_sec) != 0)
            return;
        fat_cached = sec;
    }
    cur = rd32(fat_sec + (cl % 128u) * 4u);
    wr32(fat_sec + (cl % 128u) * 4u, (cur & 0xF0000000u) | (v & 0x0FFFFFFFu));
    fat_dirty = 1;
}

/* --- FSInfo: catat nfree (best-effort) --- */
static void fsinfo_flush(void)
{
    if (m_nfree == 0xFFFFFFFFu || m_fsinfo == 0u)
        return;
    if (sd_read(m_fsinfo, sec_scratch) != 0)
        return;
    if (rd32(sec_scratch) != 0x41615252u ||
        rd32(sec_scratch + 484u) != 0x61417272u)
        return;
    wr32(sec_scratch + 488u, m_nfree);
    wr32(sec_scratch + 492u, m_hint);
    sd_write(m_fsinfo, sec_scratch);
}

int fat32_mounted(void)
{
    return m_mounted;
}

int fat32_mount(void)
{
    unsigned int bps, fatsz16, tot16, tot32;
    m_mounted = 0;
    if (!sd_present())
        return -1;
    if (sd_read(0u, sec_scratch) != 0)
        return -1;
    /* Validasi boot sector FAT32. */
    if (sec_scratch[BS_SIG] != 0x55u || sec_scratch[BS_SIG + 1] != 0xAAu)
        return -1;
    if (sec_scratch[82] != 'F' || sec_scratch[83] != 'A' ||
        sec_scratch[84] != 'T' || sec_scratch[85] != '3' ||
        sec_scratch[86] != '2')
        return -1;
    bps = rd16(sec_scratch + BS_BYTS_PER_SEC);
    if (bps != 512u)
        return -1;
    m_spc = sec_scratch[BS_SEC_PER_CLUS];
    if (m_spc == 0u || m_spc > 128u || (m_spc & (m_spc - 1u)) != 0u)
        return -1;
    m_reserved = rd16(sec_scratch + BS_RSVD_CNT);
    m_nfats = sec_scratch[BS_NUM_FATS];
    if (m_nfats == 0u || m_reserved == 0u)
        return -1;
    fatsz16 = rd16(sec_scratch + BS_FATSZ16);
    m_fatsz = rd32(sec_scratch + BS_FATSZ32);
    if (m_fatsz == 0u)
        m_fatsz = fatsz16;
    if (m_fatsz == 0u)
        return -1;
    m_root = rd32(sec_scratch + BS_ROOT_CLUS);
    tot16 = rd16(sec_scratch + BS_TOT_SEC16);
    tot32 = rd32(sec_scratch + BS_TOT_SEC32);
    m_nclus = ( (tot32 ? tot32 : tot16) - m_reserved -
                m_nfats * m_fatsz ) / m_spc;
    if (m_root < 2u || !clus_valid(m_root))
        return -1;
    m_data = m_reserved + m_nfats * m_fatsz;
    m_fsinfo = rd16(sec_scratch + BS_FSINFO);
    /* FSInfo: ambil free count bila signature valid. */
    m_nfree = 0xFFFFFFFFu;
    if (m_fsinfo != 0u && m_fsinfo < m_reserved &&
        sd_read(m_fsinfo, sec_scratch) == 0 &&
        rd32(sec_scratch) == 0x41615252u &&
        rd32(sec_scratch + 484u) == 0x61417272u)
        m_nfree = rd32(sec_scratch + 488u);
    m_hint = 2u;
    fat_cached = 0xFFFFFFFFu;
    fat_dirty = 0;
    m_mounted = 1;
    return 0;
}

/* --- Nama 8.3 ---
 * comp[0..len): komponen path. -> out[11] uppercase padded spasi.
 * 0 ok, -1 nama jelek. "." dan ".." ditolak (tak didukung di path). */
static int name_to_83(const char *comp, unsigned len, unsigned char out[11])
{
    unsigned i, bi = 0u, ei = 0u, dot = 0u;
    unsigned char base[8], ext[3];

    if (len == 0u || len > 12u)
        return -1;
    for (i = 0u; i < len; i++) {
        char c = comp[i];
        if (c == '.') {
            if (dot) return -1;         /* dua titik */
            dot = i + 1u;
            continue;
        }
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '$' || c == '%' || c == '\'' ||
              c == '-' || c == '~' || c == '!' || c == '#' ||
              c == '(' || c == ')'))
            return -1;
        if (!dot) {
            if (bi >= 8u) return -1;
            base[bi++] = (unsigned char)c;
        } else {
            if (ei >= 3u) return -1;
            ext[ei++] = (unsigned char)c;
        }
    }
    if (bi == 0u)
        return -1;
    for (i = 0u; i < 8u; i++)
        out[i] = i < bi ? base[i] : ' ';
    for (i = 0u; i < 3u; i++)
        out[8u + i] = i < ei ? ext[i] : ' ';
    return 0;
}

static int name83_eq(const unsigned char *a, const unsigned char *b)
{
    unsigned i;
    for (i = 0u; i < 11u; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static unsigned int entry_cluster(const unsigned char *e)
{
    return ((unsigned int)rd16(e + DE_CLHI) << 16) | rd16(e + DE_CLLO);
}

/* Cari entri `name` di direktori `dir_cl`. Bila ketemu: salin 32B ke
 * `out`, isi lokasi entri di `sec`/`off`. 0 ketemu, -1 tidak. */
static int find_in_dir(unsigned int dir_cl, const unsigned char name[11],
                       unsigned char out[32], unsigned int *sec, unsigned int *off)
{
    unsigned int cl = dir_cl, s, o;
    unsigned guard = 0u;

    while (clus_valid(cl) && guard < m_nclus + 8u) {
        guard++;
        for (s = 0u; s < m_spc; s++) {
            unsigned int secno = clus_first_sec(cl) + s;
            if (sd_read(secno, sec_scratch) != 0)
                return -1;
            for (o = 0u; o < 512u; o += 32u) {
                const unsigned char *e = sec_scratch + o;
                unsigned char attr;
                unsigned i;
                if (e[0] == 0x00u)
                    return -1;          /* akhir direktori */
                if (e[0] == 0xE5u)
                    continue;           /* terhapus */
                attr = e[DE_ATTR];
                if (attr == ATTR_LFN || attr == ATTR_VOL)
                    continue;
                if (name83_eq(e, name)) {
                    for (i = 0u; i < 32u; i++)
                        out[i] = e[i];
                    *sec = secno;
                    *off = o;
                    return 0;
                }
            }
        }
        cl = fat_get(cl);
        if (cl >= FAT_EOC)
            break;
    }
    return -1;
}

/* Alokasi 1 cluster: tandai EOC, nol-kan isinya. 0 = habis/gagal. */
static unsigned int alloc_cluster(void)
{
    unsigned int c, s, i;

    c = m_hint;
    for (i = 0u; i < m_nclus; i++) {
        if (c < 2u || c >= 2u + m_nclus)
            c = 2u;
        if (fat_get(c) == FAT_FREE)
            break;
        c++;
    }
    if (i >= m_nclus || fat_get(c) != FAT_FREE)
        return 0u;
    fat_set(c, FAT_EOC);
    for (s = 0u; s < m_spc; s++) {
        for (i = 0u; i < 512u; i++)
            sec_scratch[i] = 0u;
        if (sd_write(clus_first_sec(c) + s, sec_scratch) != 0) {
            fat_set(c, FAT_FREE);
            fat_flush();
            return 0u;
        }
    }
    if (m_nfree != 0xFFFFFFFFu && m_nfree > 0u)
        m_nfree--;
    m_hint = c + 1u;
    fat_flush();
    return c;
}

/* Bebaskan chain cluster (tandai 0 semua). */
static void free_chain(unsigned int cl)
{
    unsigned int nx;
    unsigned guard = 0u;
    while (clus_valid(cl) && guard < m_nclus + 8u) {
        guard++;
        nx = fat_get(cl);
        fat_set(cl, FAT_FREE);
        if (m_nfree != 0xFFFFFFFFu)
            m_nfree++;
        if (nx >= FAT_EOC || nx < 2u)
            break;
        cl = nx;
    }
    fat_flush();
}

/* Cari slot entri kosong di direktori; perluas chain bila penuh.
 * Tulis entri 32B `e` ke slot. 0 ok / -1 gagal. */
static int create_entry(unsigned int dir_cl, const unsigned char e[32])
{
    unsigned int cl = dir_cl, s, o, secno, prev = 0u, nc;
    unsigned guard = 0u, i;

    while (clus_valid(cl) && guard < m_nclus + 8u) {
        guard++;
        for (s = 0u; s < m_spc; s++) {
            secno = clus_first_sec(cl) + s;
            if (sd_read(secno, sec_scratch) != 0)
                return -1;
            for (o = 0u; o < 512u; o += 32u) {
                unsigned char c0 = sec_scratch[o];
                if (c0 == 0x00u || c0 == 0xE5u) {
                    for (i = 0u; i < 32u; i++)
                        sec_scratch[o + i] = e[i];
                    if (sd_write(secno, sec_scratch) != 0)
                        return -1;
                    return 0;
                }
            }
        }
        prev = cl;
        cl = fat_get(cl);
        if (cl >= FAT_EOC)
            break;
    }
    /* Direktori penuh: tambah 1 cluster. */
    nc = alloc_cluster();
    if (nc == 0u)
        return -1;
    fat_set(prev, nc);
    fat_flush();
    secno = clus_first_sec(nc);
    if (sd_read(secno, sec_scratch) != 0)
        return -1;
    for (i = 0u; i < 32u; i++)
        sec_scratch[i] = e[i];
    if (sd_write(secno, sec_scratch) != 0)
        return -1;
    return 0;
}

/* Pecah path "/sd/..." -> cluster direktori induk + nama 8.3 leaf.
 * Path "/sd" (root) -> *is_root=1. 0 ok / -1 path jelek. */
static int split_path(const char *path, unsigned int *parent,
                      unsigned char leaf[11], int *is_root)
{
    const char *p = path;
    unsigned int cl;
    unsigned char e[32];
    unsigned int sec, off;

    *is_root = 0;
    if (p[0] != '/' || p[1] != 's' || p[2] != 'd')
        return -1;
    p += 3;
    if (*p == '\0') {                    /* "/sd" */
        *is_root = 1;
        *parent = m_root;
        return 0;
    }
    if (*p != '/')
        return -1;
    p++;
    if (*p == '\0') {                    /* "/sd/" */
        *is_root = 1;
        *parent = m_root;
        return 0;
    }
    cl = m_root;
    for (;;) {
        const char *comp = p;
        unsigned len = 0u;
        while (p[len] && p[len] != '/')
            len++;
        if (len == 0u)
            return -1;
        if (p[len] == '/') {
            /* Komponen tengah: harus subdirektori yang ada. */
            unsigned char n83[11];
            if (name_to_83(comp, len, n83) != 0)
                return -1;
            if (find_in_dir(cl, n83, e, &sec, &off) != 0)
                return -1;
            if (!(e[DE_ATTR] & ATTR_DIR))
                return -1;
            cl = entry_cluster(e);
            if (!clus_valid(cl))
                return -1;
            p += len + 1u;
            if (*p == '\0')
                return -1;              /* trailing '/' */
        } else {
            /* Leaf. */
            if (name_to_83(comp, len, leaf) != 0)
                return -1;
            *parent = cl;
            return 0;
        }
    }
}

static void make_entry(unsigned char e[32], const unsigned char name[11],
                       unsigned char attr, unsigned int cl, unsigned int size)
{
    unsigned i;
    for (i = 0u; i < 11u; i++)
        e[i] = name[i];
    e[DE_ATTR] = attr;
    for (i = 12u; i < 32u; i++)
        e[i] = 0u;
    wr16(e + DE_CLHI, (unsigned short)(cl >> 16));
    wr16(e + DE_CLLO, (unsigned short)cl);
    wr32(e + DE_SIZE, size);
}

int fat32_mkdir(const char *path)
{
    unsigned int parent, cl, s;
    unsigned char leaf[11], e[32], dot[32], dotdot[32];
    int is_root;
    unsigned int sec, off;
    unsigned i;

    if (!m_mounted)
        return -1;
    if (split_path(path, &parent, leaf, &is_root) != 0 || is_root)
        return -1;
    if (find_in_dir(parent, leaf, e, &sec, &off) == 0)
        return -1;                      /* sudah ada */
    cl = alloc_cluster();
    if (cl == 0u)
        return -1;
    /* Entri "." dan ".." di cluster baru (konvensi FAT). */
    if (sd_read(clus_first_sec(cl), sec_scratch) != 0) {
        fat_set(cl, FAT_FREE);
        fat_flush();
        return -1;
    }
    for (i = 0u; i < 11u; i++) {
        dot[i] = dotdot[i] = ' ';
    }
    dot[0] = '.';
    dotdot[0] = '.'; dotdot[1] = '.';
    make_entry(sec_scratch, dot, ATTR_DIR, cl, 0u);
    make_entry(sec_scratch + 32u, dotdot, ATTR_DIR, parent, 0u);
    if (sd_write(clus_first_sec(cl), sec_scratch) != 0) {
        fat_set(cl, FAT_FREE);
        fat_flush();
        return -1;
    }
    make_entry(e, leaf, ATTR_DIR, cl, 0u);
    if (create_entry(parent, e) != 0) {
        fat_set(cl, FAT_FREE);
        fat_flush();
        return -1;
    }
    /* Sisa sektor cluster baru sudah nol dari alloc_cluster. */
    for (s = 1u; s < m_spc; s++) {
        for (i = 0u; i < 512u; i++)
            sec_scratch[i] = 0u;
        sd_write(clus_first_sec(cl) + s, sec_scratch);
    }
    fsinfo_flush();
    return 0;
}

int fat32_write_file(const char *path, const unsigned char *data, unsigned int len)
{
    unsigned int parent, first = 0u, cl = 0u, prev = 0u;
    unsigned int sec, off, written = 0u, size;
    unsigned char leaf[11], e[32];
    int is_root;
    unsigned i;

    if (!m_mounted)
        return -1;
    if (split_path(path, &parent, leaf, &is_root) != 0 || is_root)
        return -1;
    if (find_in_dir(parent, leaf, e, &sec, &off) == 0) {
        if (e[DE_ATTR] & ATTR_DIR)
            return -1;                  /* direktori: tolak */
        cl = entry_cluster(e);
        if (clus_valid(cl))
            free_chain(cl);
        first = 0u;
        /* Entri dipakai ulang di bawah (update cluster+size). */
    } else {
        make_entry(e, leaf, 0x20u, 0u, 0u);
        if (create_entry(parent, e) != 0)
            return -1;
        /* Baca ulang lokasi entri untuk update. */
        if (find_in_dir(parent, leaf, e, &sec, &off) != 0)
            return -1;
    }

    /* Alokasi + tulis per sektor. */
    while (written < len) {
        unsigned int nc = alloc_cluster();
        unsigned int s;
        if (nc == 0u)
            break;                      /* habis: tulis sebagian */
        if (first == 0u)
            first = nc;
        else
            fat_set(prev, nc);
        fat_flush();
        prev = nc;
        for (s = 0u; s < m_spc && written < len; s++) {
            unsigned int n = len - written;
            if (n > 512u)
                n = 512u;
            for (i = 0u; i < n; i++)
                sec_scratch[i] = data[written + i];
            for (; i < 512u; i++)
                sec_scratch[i] = 0u;
            if (sd_write(clus_first_sec(nc) + s, sec_scratch) != 0)
                break;
            written += n;
        }
    }

    /* Update entri: cluster awal + ukuran. */
    size = written;
    wr16(e + DE_CLHI, (unsigned short)(first >> 16));
    wr16(e + DE_CLLO, (unsigned short)first);
    wr32(e + DE_SIZE, size);
    if (sd_read(sec, sec_scratch) == 0) {
        for (i = 0u; i < 32u; i++)
            sec_scratch[off + i] = e[i];
        sd_write(sec, sec_scratch);
    }
    fsinfo_flush();
    return (written == len) ? (int)len : -1;
}

int fat32_read_file(const char *path, unsigned char *dst, unsigned int max)
{
    unsigned int parent, cl, size, got = 0u, sec, off;
    unsigned char leaf[11], e[32];
    int is_root;
    unsigned guard = 0u, i;

    if (!m_mounted)
        return -1;
    if (split_path(path, &parent, leaf, &is_root) != 0 || is_root)
        return -1;
    if (find_in_dir(parent, leaf, e, &sec, &off) != 0)
        return -1;
    if (e[DE_ATTR] & ATTR_DIR)
        return -1;
    size = rd32(e + DE_SIZE);
    if (size > max)
        size = max;
    cl = entry_cluster(e);
    while (got < size && clus_valid(cl) && guard < m_nclus + 8u) {
        unsigned int s;
        guard++;
        for (s = 0u; s < m_spc && got < size; s++) {
            unsigned int n = size - got;
            if (n > 512u)
                n = 512u;
            if (sd_read(clus_first_sec(cl) + s, sec_scratch) != 0)
                return -1;
            for (i = 0u; i < n; i++)
                dst[got + i] = sec_scratch[i];
            got += n;
        }
        cl = fat_get(cl);
        if (cl >= FAT_EOC)
            break;
    }
    return (int)got;
}

int fat32_delete(const char *path)
{
    unsigned int parent, cl, sec, off;
    unsigned char leaf[11], e[32];
    int is_root;

    if (!m_mounted)
        return -1;
    if (split_path(path, &parent, leaf, &is_root) != 0 || is_root)
        return -1;
    if (find_in_dir(parent, leaf, e, &sec, &off) != 0)
        return -1;
    if (e[DE_ATTR] & ATTR_DIR)
        return -1;                      /* direktori tak dihapus di sini */
    cl = entry_cluster(e);
    if (clus_valid(cl))
        free_chain(cl);
    if (sd_read(sec, sec_scratch) != 0)
        return -1;
    sec_scratch[off] = 0xE5u;
    if (sd_write(sec, sec_scratch) != 0)
        return -1;
    fsinfo_flush();
    return 0;
}

int fat32_listdir(const char *path, char *dst, unsigned int max)
{
    unsigned int dir_cl, cl, s, o, n = 0u, pos = 0u;
    unsigned int parent, sec, off;
    unsigned char leaf[11], e[32];
    int is_root;
    unsigned guard = 0u, i;

    if (!m_mounted)
        return -1;
    if (split_path(path, &parent, leaf, &is_root) != 0)
        return -1;
    if (is_root) {
        dir_cl = m_root;
    } else {
        if (find_in_dir(parent, leaf, e, &sec, &off) != 0)
            return -1;
        if (!(e[DE_ATTR] & ATTR_DIR))
            return -1;
        dir_cl = entry_cluster(e);
        if (!clus_valid(dir_cl))
            return -1;
    }

    cl = dir_cl;
    while (clus_valid(cl) && guard < m_nclus + 8u) {
        guard++;
        for (s = 0u; s < m_spc; s++) {
            if (sd_read(clus_first_sec(cl) + s, sec_scratch) != 0)
                return -1;
            for (o = 0u; o < 512u; o += 32u) {
                const unsigned char *de = sec_scratch + o;
                unsigned char attr, c0 = de[0];
                unsigned nl;
                char nm[14];
                if (c0 == 0x00u)
                    goto done;          /* akhir direktori */
                if (c0 == 0xE5u)
                    continue;
                attr = de[DE_ATTR];
                if (attr == ATTR_LFN || attr == ATTR_VOL)
                    continue;
                /* Lewati "." dan "..". */
                if (de[0] == '.' &&
                    (de[1] == ' ' || (de[1] == '.' && de[2] == ' ')))
                    continue;
                /* Format "NAMA.EXT\n" atau "NAMA/\n" untuk direktori. */
                nl = 0u;
                for (i = 0u; i < 8u && de[i] != ' '; i++)
                    nm[nl++] = (char)de[i];
                if (!(attr & ATTR_DIR)) {
                    unsigned el = 0u;
                    for (i = 8u; i < 11u && de[i] != ' '; i++)
                        el++;
                    if (el > 0u) {
                        nm[nl++] = '.';
                        for (i = 8u; i < 8u + el; i++)
                            nm[nl++] = (char)de[i];
                    }
                } else {
                    nm[nl++] = '/';
                }
                nm[nl++] = '\n';
                if (pos + nl < max) {
                    for (i = 0u; i < nl; i++)
                        dst[pos + i] = nm[i];
                    pos += nl;
                }
                n++;
            }
        }
        cl = fat_get(cl);
        if (cl >= FAT_EOC)
            break;
    }
done:
    return (int)n;
}
