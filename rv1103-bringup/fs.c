/*
 * fs.c - ramfs flat dalam RAM (Fase 9, bring-up).
 *
 * Satu tabel file global (16 file x 64 KB pool BSS statis = 1 MB;
 * deterministik, tanpa fragmentasi, tanpa page allocator). Tiap file
 * punya generation counter: delete menaikkan gen sehingga fd yang
 * masih terbuka ke slot itu terdeteksi basi.
 *
 * Sinkronisasi: semua API publik me-mask IRQ selama seksi kritis
 * (pola ipc.c Fase 6). Aman karena tidak ada blocking di dalamnya:
 * hanya scan array + memcpy terbatas.
 *
 * Keterbatasan bring-up (disengaja): flat tanpa subdirektori, tanpa
 * truncate (O_CREAT pada file yang ada = open biasa), tanpa rename,
 * tanpa permission bits.
 *
 * C99, -ffreestanding, no libc.
 */
#include "fs.h"
#include "task.h"
#include "lib.h"

struct ramfs_file {
    char name[FS_NAME_MAX];
    uint8_t data[FS_FILE_MAX];
    uint32_t size;
    unsigned gen;
    int used;
};

static struct ramfs_file fs_files[FS_MAX_FILES];

static inline unsigned fs_irq_save(void)
{
    unsigned cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    return cpsr;
}

static inline void fs_irq_restore(unsigned cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

void fs_init(void)
{
    /* BSS sudah nol: used=0, gen=0, size=0. Tidak ada yang dikerjakan. */
}

/* Lewati '/' di depan: "/a.txt" -> "a.txt". */
static const char *name_norm(const char *name)
{
    while (*name == '/')
        name++;
    return name;
}

/* 1 bila nama valid: 1..FS_NAME_MAX-1 char, tanpa '/' di tengah. */
static int name_ok(const char *name)
{
    unsigned i;

    if (!name || name[0] == 0)
        return 0;
    for (i = 0; i < FS_NAME_MAX; i++) {
        char c = name[i];
        if (c == 0)
            return 1;
        if (c == '/')
            return 0;
    }
    return 0;   /* tidak NUL-terminated dalam batas */
}

static int name_eq(const char *a, const char *b)
{
    unsigned i;

    for (i = 0; i < FS_NAME_MAX; i++) {
        if (a[i] != b[i])
            return 0;
        if (a[i] == 0)
            return 1;
    }
    return 1;
}

/* Versi tanpa lock (pemanggil sudah me-mask IRQ). */
static int find_nolock(const char *name)
{
    unsigned i;

    for (i = 0; i < FS_MAX_FILES; i++)
        if (fs_files[i].used && name_eq(fs_files[i].name, name))
            return (int)i;
    return -1;
}

int fs_find(const char *name)
{
    int r;
    unsigned s;

    if (!name)
        return -1;
    name = name_norm(name);
    if (!name_ok(name))
        return -1;
    s = fs_irq_save();
    r = find_nolock(name);
    fs_irq_restore(s);
    return r;
}

int fs_create(const char *name)
{
    unsigned i, s;
    int r = -1;

    if (!name)
        return -1;
    name = name_norm(name);
    if (!name_ok(name))
        return -1;
    s = fs_irq_save();
    if (find_nolock(name) < 0) {
        for (i = 0; i < FS_MAX_FILES; i++) {
            if (!fs_files[i].used) {
                struct ramfs_file *f = &fs_files[i];
                unsigned j;
                /* gen dibiarkan (naik tiap delete); slot baru gen=0. */
                for (j = 0; j < FS_NAME_MAX; j++) {
                    f->name[j] = name[j];
                    if (name[j] == 0)
                        break;
                }
                f->size = 0;
                f->used = 1;
                r = (int)i;
                break;
            }
        }
    }
    fs_irq_restore(s);
    return r;
}

int fs_open(struct task *t, const char *name, unsigned flags)
{
    unsigned i, s;
    int idx, fd = -1;

    if (!t || !name)
        return -1;
    name = name_norm(name);
    if (!name_ok(name))
        return -1;
    s = fs_irq_save();
    idx = find_nolock(name);
    if (idx < 0) {
        int ni;
        unsigned j;
        if (!(flags & FS_O_CREAT)) {
            fs_irq_restore(s);
            return -1;
        }
        ni = -1;
        for (i = 0; i < FS_MAX_FILES; i++) {
            if (!fs_files[i].used) {
                ni = (int)i;
                break;
            }
        }
        if (ni < 0) {
            fs_irq_restore(s);
            return -1;
        }
        for (j = 0; j < FS_NAME_MAX; j++) {
            fs_files[ni].name[j] = name[j];
            if (name[j] == 0)
                break;
        }
        fs_files[ni].size = 0;
        fs_files[ni].used = 1;
        idx = ni;
    }
    for (i = 3u; i < FS_MAX_FD; i++) {
        if (!t->fds[i].used) {
            struct fs_fd *f = &t->fds[i];
            f->used = 1;
            f->fidx = (unsigned)idx;
            f->gen = fs_files[idx].gen;
            f->off = 0u;
            f->flags = flags;
            fd = (int)i;
            break;
        }
    }
    fs_irq_restore(s);
    return fd;
}

/* Ambil file dari fd; 0 bila fd basi/tak valid. */
static struct ramfs_file *fd_file(struct task *t, unsigned fd,
                                  struct fs_fd **out)
{
    struct fs_fd *f;
    struct ramfs_file *rf;

    if (!t || fd >= FS_MAX_FD)
        return 0;
    f = &t->fds[fd];
    if (!f->used || f->fidx == FS_FD_CONSOLE)
        return 0;
    if (f->fidx >= FS_MAX_FILES)
        return 0;
    rf = &fs_files[f->fidx];
    if (!rf->used || rf->gen != f->gen)
        return 0;   /* fd basi: file di-delete sesudah open */
    if (out)
        *out = f;
    return rf;
}

int fs_read(struct task *t, unsigned fd, uint8_t *dst, uint32_t len)
{
    struct ramfs_file *rf;
    struct fs_fd *f;
    unsigned s, acc;
    uint32_t n;

    if (!dst)
        return -1;
    s = fs_irq_save();
    rf = fd_file(t, fd, &f);
    if (!rf) {
        fs_irq_restore(s);
        return -1;
    }
    acc = f->flags & FS_O_ACCMODE;
    if (acc != FS_O_RDONLY && acc != FS_O_RDWR) {
        fs_irq_restore(s);
        return -1;
    }
    n = (f->off < rf->size) ? rf->size - f->off : 0u;
    if (n > len)
        n = len;
    if (n > 0)
        memcpy(dst, rf->data + f->off, n);
    f->off += n;
    fs_irq_restore(s);
    return (int)n;
}

int fs_write(struct task *t, unsigned fd, const uint8_t *src,
             uint32_t len)
{
    struct ramfs_file *rf;
    struct fs_fd *f;
    unsigned s, acc;
    uint32_t room, n;

    if (!src)
        return -1;
    s = fs_irq_save();
    rf = fd_file(t, fd, &f);
    if (!rf) {
        fs_irq_restore(s);
        return -1;
    }
    acc = f->flags & FS_O_ACCMODE;
    if (acc != FS_O_WRONLY && acc != FS_O_RDWR) {
        fs_irq_restore(s);
        return -1;
    }
    /* Tulis parsial bila melebihi kapasitas (bukan error). */
    room = (f->off < FS_FILE_MAX) ? FS_FILE_MAX - f->off : 0u;
    n = (len < room) ? len : room;
    if (n > 0) {
        memcpy(rf->data + f->off, src, n);
        f->off += n;
        if (f->off > rf->size)
            rf->size = f->off;
    }
    fs_irq_restore(s);
    return (int)n;
}

int fs_close(struct task *t, unsigned fd)
{
    struct fs_fd *f;
    unsigned s;

    if (!t || fd >= FS_MAX_FD)
        return -1;
    s = fs_irq_save();
    f = &t->fds[fd];
    if (!f->used || f->fidx == FS_FD_CONSOLE) {
        fs_irq_restore(s);
        return -1;
    }
    f->used = 0;
    fs_irq_restore(s);
    return 0;
}

int fs_list(char *dst, uint32_t max)
{
    unsigned i, s;
    uint32_t pos = 0u;
    int cnt = 0;

    if (!dst)
        return -1;
    s = fs_irq_save();
    for (i = 0; i < FS_MAX_FILES; i++) {
        unsigned j = 0u;
        if (!fs_files[i].used)
            continue;
        cnt++;
        while (j < FS_NAME_MAX && fs_files[i].name[j])
            j++;
        /* Tulis "nama\n" hanya bila muat utuh. */
        if (pos + j + 1u <= max && max > 0u) {
            memcpy(dst + pos, fs_files[i].name, j);
            dst[pos + j] = '\n';
            pos += j + 1u;
        }
    }
    fs_irq_restore(s);
    return cnt;
}

int fs_delete(const char *name)
{
    unsigned s;
    int idx, r = -1;

    if (!name)
        return -1;
    name = name_norm(name);
    if (!name_ok(name))
        return -1;
    s = fs_irq_save();
    idx = find_nolock(name);
    if (idx >= 0) {
        fs_files[idx].used = 0;
        fs_files[idx].gen++;    /* fd terbuka ke file ini jadi basi */
        fs_files[idx].size = 0;
        fs_files[idx].name[0] = 0;
        r = 0;
    }
    fs_irq_restore(s);
    return r;
}
