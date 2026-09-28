/*
 * syscall.c - SVC dispatch. Tiap thread pakai ipc_space milik task-nya.
 *
 * Fase 8: pemanggil dari USR mode dilayani syscall terbatas
 * (SYS_WRITE/YIELD/EXIT/RPC_USER/SBRK); primitif IPC mentah
 * (SYS_SEND/RECV/RPC) ditolak dari user (-1).
 */
#include "syscall.h"
#include "sched.h"
#include "user.h"
#include "vm.h"
#include "fs.h"
#include "lib.h"
#include "gpio.h"   /* Fase 14 */
#include "blk.h"    /* Fase 15: sd_read/sd_write */
#include "fat32.h"  /* Fase 16: filesystem FAT32 di /sd */
#include "net.h"    /* Fase 17: net_rx/tx_bytes_get untuk SYS_STAT */

static struct task *kern_task;

void syscall_init(struct task *kt)
{
    kern_task = kt;
}

static struct task *dispatch_task(void)
{
    struct task *t = sched_current_task();
    if (t)
        return t;
    /* Sebelum scheduler jalan (selftest single-threaded). */
    return kern_task;
}

/* 1 bila SVC datang dari USR mode (spsr mode field == 0x10). */
static int caller_is_user(struct trap_regs *regs)
{
    return regs && ((regs->spsr & 0x1Fu) == 0x10u);
}

/* ------------------------------------------------------------------ */
/* SYS_WRITE (user only): fd 1/2 -> UART; fd >= 3 -> tulis ke file   */
/* ramfs (Fase 9). Pointer user divalidasi terhadap USER range.      */
/* ------------------------------------------------------------------ */
static int sys_write_user(struct task *t, struct trap_regs *regs)
{
    uint32_t fd = regs->r[0];
    uint32_t va = regs->r[1];
    uint32_t len = regs->r[2];
    const unsigned char *p;
    uint32_t i;
    unsigned cpsr;

    if (!user_range_ok(va, len))
        return -1;
    if (fd == 1u || fd == 2u) {
        p = (const unsigned char *)va;
        /* Mask IRQ selama loop pendek ini agar baris tidak ter-interleave
         * dengan cetakan thread lain (pola irq_save Fase 6). */
        __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
        __asm__ volatile("cpsid i" ::: "memory");
        for (i = 0; i < len; i++)
            uputc((char)p[i]);
        __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
        return (int)len;
    }
    if (fd >= 3u)
        return fs_write(t, fd, (const uint8_t *)va, len);
    return -1;  /* fd 0 (stdin) / fd liar */
}

/* ------------------------------------------------------------------ */
/* SYS_YIELD: serahkan sisa slice; tunggu minimal satu tick dengan    */
/* IRQ hidup (SVC entry mem-mask IRQ -> cpsie dulu, restore sesudah,  */
/* pola Fase 6).                                                      */
/* ------------------------------------------------------------------ */
static int sys_yield_user(void)
{
    unsigned cpsr, t0;

    /*
     * Menunggu tick timer berikutnya. IRQ diaktifkan sementara agar
     * tick bisa masuk; CPU di-halt dengan WFI (bukan busy-spin) sampai
     * IRQ tiba. Busy-spin memanggil sched_ticks() berulang kali terbukti
     * memicu korupsi register di sched_on_tick pada QEMU (r6=0x80000093).
     */
    t0 = sched_ticks();
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("cpsie i" ::: "memory");
    while (sched_ticks() == t0) {
        __asm__ volatile("wfi" ::: "memory");
    }
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_EXIT: akhiri thread pemanggil. Tandai THREAD_DEAD, hidupkan    */
/* IRQ, spin; tick berikutnya menjadwalkan thread lain. TIDAK KEMBALI */
/* (jangan panic: thread lain harus tetap hidup).                     */
/* ------------------------------------------------------------------ */
static void sys_exit_user(void)
{
    struct sched_thread *t = sched_current_thread();

    uputs("[user] thread exit\n");
    if (t)
        t->state = THREAD_DEAD;
    user_done = 1u;
    __asm__ volatile("cpsie i" ::: "memory");
    for (;;) { }
    /* tak tercapai */
}

/* ------------------------------------------------------------------ */
/* SYS_RPC_USER (user only): RPC sinkron dengan bounce buffer kernel. */
/* Pointer user TIDAK dioper mentah ke ipc_* (halamannya bisa berubah */
/* di bawah kernel); request disalin ke buffer kernel statis, reply   */
/* disalin kembali ke buffer user (dibatasi kapasitasnya).             */
/* ------------------------------------------------------------------ */
static int sys_rpc_user(struct task *t, struct trap_regs *regs)
{
    /* r0=sname r1=req_ptr r2=req_len r3=rname r4=rep_ptr r5=rep_len */
    static struct ipc_wire kw_req, kw_rep;
    uint32_t req_va = regs->r[1], rep_va = regs->r[4];
    uint32_t req_len = regs->r[2], rep_len = regs->r[5];
    uint32_t n, total;
    int sz;

    if (!t)
        return -1;
    if (!user_range_ok(req_va, req_len) || !user_range_ok(rep_va, rep_len))
        return -1;
    if (req_len > sizeof(kw_req) || rep_len > sizeof(kw_rep))
        return -1;
    memcpy(&kw_req, (const void *)req_va, req_len);
    if (kw_req.size > IPC_MSG_DATA)
        kw_req.size = IPC_MSG_DATA;
    sz = ipc_rpc(&t->ipc, regs->r[0], &kw_req, sizeof(kw_req),
                 regs->r[3], &kw_rep, sizeof(kw_rep));
    if (sz < 0)
        return -1;
    /* Salin header (bits/id/size = 12 byte) + payload reply ke user,
     * dibatasi kapasitas buffer user. */
    n = (uint32_t)sz;
    total = 12u + n;
    if (total > rep_len)
        total = rep_len;
    memcpy((void *)rep_va, &kw_rep, total);
    return sz;
}

/* ------------------------------------------------------------------ */
/* Syscall file Fase 9 (semuanya user-only).                          */
/* ------------------------------------------------------------------ */

/* Salin path NUL-terminated dari user VA ke buffer kernel lokal
 * (di stack SVC thread ini -> per-thread, aman dari preempt).
 * -> 0 ok, -1 bila range tak valid / tak ada NUL dalam FS_PATH_MAX. */
static int copy_path_user(uint32_t va, char *kpath)
{
    const char *up;
    unsigned i;

    if (!user_range_ok(va, FS_PATH_MAX))
        return -1;
    up = (const char *)va;
    for (i = 0u; i < FS_PATH_MAX; i++) {
        kpath[i] = up[i];   /* fault -> pager selesaikan (pola write) */
        if (up[i] == 0)
            return 0;
    }
    return -1;
}

/* SYS_OPEN: r0=path_va r1=flags -> fd / -1. */
static int sys_open_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];

    if (!t)
        return -1;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fs_open(t, kpath, regs->r[1]);
}

/* SYS_READ: r0=fd r1=buf_va r2=len -> byte / -1.
 * fd 0 (stdin): tidak ada input di bring-up -> 0 (EOF). */
static int sys_read_user(struct task *t, struct trap_regs *regs)
{
    uint32_t fd = regs->r[0];
    uint32_t va = regs->r[1];
    uint32_t len = regs->r[2];

    if (fd == 0u)
        return 0;
    if (fd == 1u || fd == 2u)
        return -1;
    if (!user_range_ok(va, len))
        return -1;
    return fs_read(t, fd, (uint8_t *)va, len);
}

/* SYS_CLOSE: r0=fd -> 0 / -1. */
static int sys_close_user(struct task *t, struct trap_regs *regs)
{
    return fs_close(t, regs->r[0]);
}

/* SYS_LS: r0=buf_va r1=max -> jumlah file (daftar "nama\n"). */
static int sys_ls_user(struct task *t, struct trap_regs *regs)
{
    uint32_t va = regs->r[0];
    uint32_t max = regs->r[1];

    (void)t;
    if (!user_range_ok(va, max))
        return -1;
    return fs_list((char *)va, max);
}

/* SYS_DELETE: r0=path_va -> 0 / -1. */
static int sys_delete_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];

    (void)t;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fs_delete(kpath);
}
/* ------------------------------------------------------------------ */
/* Fase 15: SYS_SD_READ / SYS_SD_WRITE (user only): raw sector I/O    */
/* ke kartu SD (dev 1). Bounce buffer kernel statis 512B (pola       */
/* SYS_RPC_USER): pointer user tidak dioper mentah ke driver;        */
/* buffer user divalidasi user_range_ok dulu.                         */
/* ------------------------------------------------------------------ */
static uint8_t sd_kbuf[512] __attribute__((aligned(16)));

static int sys_sd_read_user(struct trap_regs *regs)
{
    uint32_t sector = regs->r[0];
    uint32_t va = regs->r[1];
    unsigned i;

    if (!user_range_ok(va, 512u))
        return -1;
    if (!sd_present())
        return -1;
    if (sd_read((uint64_t)sector, sd_kbuf) != 0)
        return -1;
    for (i = 0; i < 512u; i++)
        ((uint8_t *)va)[i] = sd_kbuf[i];
    return 0;
}

static int sys_sd_write_user(struct trap_regs *regs)
{
    uint32_t sector = regs->r[0];
    uint32_t va = regs->r[1];
    unsigned i;

    if (!user_range_ok(va, 512u))
        return -1;
    if (!sd_present())
        return -1;
    for (i = 0; i < 512u; i++)
        sd_kbuf[i] = ((const uint8_t *)va)[i];
    return sd_write((uint64_t)sector, sd_kbuf) == 0 ? 0 : -1;
}
/* ------------------------------------------------------------------ */
/* Fase 16: syscall filesystem FAT32 (user only). Path absolut        */
/* "/sd/..." disalin dari user via copy_path_user (maks 63+NUL).      */
/* Buffer data divalidasi user_range_ok lalu dioper langsung sebagai   */
/* pointer ke fat32_* (halaman user ter-map; pola sys_sd_read_user).  */
/* ------------------------------------------------------------------ */

/* Batas ukuran file per syscall (1MB; transfer per sektor, tanpa
 * bounce buffer raksasa). */
#define FAT_XFER_MAX (1024u * 1024u)

static int sys_mkdir_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];

    (void)t;
    if (!fat32_mounted())
        return -1;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fat32_mkdir(kpath);
}

static int sys_fat_write_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];
    uint32_t va = regs->r[1];
    uint32_t len = regs->r[2];

    (void)t;
    if (!fat32_mounted())
        return -1;
    if (len > FAT_XFER_MAX)
        return -1;
    if (!user_range_ok(va, len))
        return -1;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fat32_write_file(kpath, (const uint8_t *)va, len);
}

static int sys_fat_read_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];
    uint32_t va = regs->r[1];
    uint32_t max = regs->r[2];

    (void)t;
    if (!fat32_mounted())
        return -1;
    if (max > FAT_XFER_MAX)
        return -1;
    if (!user_range_ok(va, max))
        return -1;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fat32_read_file(kpath, (uint8_t *)va, max);
}

static int sys_fat_delete_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];

    (void)t;
    if (!fat32_mounted())
        return -1;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fat32_delete(kpath);
}

static int sys_readdir_user(struct task *t, struct trap_regs *regs)
{
    char kpath[FS_PATH_MAX];
    uint32_t va = regs->r[1];
    uint32_t max = regs->r[2];

    (void)t;
    if (!fat32_mounted())
        return -1;
    if (max > FAT_XFER_MAX)
        return -1;
    if (!user_range_ok(va, max))
        return -1;
    if (copy_path_user(regs->r[0], kpath) != 0)
        return -1;
    return fat32_listdir(kpath, (char *)va, max);
}

/* ------------------------------------------------------------------ */
/* Fase 17: SYS_STAT / SYS_TLIST / SYS_READ_CONSOLE (user only).     */
/* Statistik REAL emulasi Fase 12d, diekspos ke umon.                 */
/* ------------------------------------------------------------------ */

static int sys_stat_user(struct trap_regs *regs)
{
    uint32_t va = regs->r[0];
    uint32_t len = regs->r[1];
    struct qaon_stat *s;
    struct vm_stats vs;

    if (len < sizeof(struct qaon_stat))
        return -1;
    if (!user_range_ok(va, sizeof(struct qaon_stat)))
        return -1;
    s = (struct qaon_stat *)va;
    s->uptime_ms = sched_ticks();
    s->cpu_pct = sched_cpu_pct();
    vm_get_stats(&vs);
    s->mem_used_kb = vs.pages_used * 4u;
    s->mem_total_kb = vs.pages_total * 4u;
    s->blk_total_sec = blk_total_sectors();
    s->blk_used_sec = blk_used_sectors();
    s->net_rx_kb = (uint32_t)(net_rx_bytes_get() >> 10);
    s->net_tx_kb = (uint32_t)(net_tx_bytes_get() >> 10);
    s->nthreads = sched_thread_count();
    return 0;
}

static int sys_tlist_user(struct trap_regs *regs)
{
    uint32_t va = regs->r[0];
    uint32_t max = regs->r[1];
    struct qaon_tentry *e;
    unsigned n, i, cnt;

    if (max == 0u || max > 64u)
        return -1;
    if (!user_range_ok(va, max * (uint32_t)sizeof(struct qaon_tentry)))
        return -1;
    e = (struct qaon_tentry *)va;
    n = sched_thread_count();
    cnt = 0u;
    for (i = 0u; i < n && cnt < max; i++) {
        const struct sched_thread *t = sched_thread_at(i);
        if (!t)
            continue;
        e[cnt].id = (uint32_t)t->id;
        e[cnt].state = t->state;
        e[cnt].user = t->user_sp ? 1u : 0u;
        cnt++;
    }
    return (int)cnt;
}

/* SYS_READ_CONSOLE: 1 byte dari UART polled, non-blocking.
 * 0-255 bila ada, -1 bila FIFO kosong. */
static int sys_console_read_user(void)
{
    return console_getc_nb();
}

/* SYS_SBRK (user only): naikkan program break (lihat aslinya). */static int sys_sbrk_user(struct task *t, struct trap_regs *regs)
{
    int inc;
    uint32_t old, n, i, pa;

    if (!t)
        return -1;
    if (t->brk == 0u)
        t->brk = USER_BRK_START;
    inc = (int)regs->r[0];
    if (inc <= 0)
        return (int)t->brk;
    old = t->brk;
    n = ((uint32_t)inc + 0xFFFu) >> 12;
    for (i = 0; i < n; i++) {
        pa = vm_page_alloc();
        if (pa == 0u)
            return -1;
        if (vm_map(&t->vm, t->brk, pa, VM_PROT_READ | VM_PROT_WRITE) != 0)
            return -1;
        t->brk += 0x1000u;
    }
    return (int)old;
}

void svc_dispatch(struct trap_regs *regs)
{
    unsigned num;
    struct task *t;
    struct ipc_space *sp;
    int ret = -2;   /* -2 = not a known syscall, leave r0 alone */
    int u;

    if (!regs)
        return;
    t = dispatch_task();
    sp = t ? &t->ipc : 0;
    if (!sp)
        return;
    num = regs->r[7];
    u = caller_is_user(regs);
    if (num == SYS_SEND) {
        ret = u ? -1 : ipc_send(sp, regs->r[0],
                               (const struct ipc_wire *)regs->r[1],
                               regs->r[2]);
    } else if (num == SYS_RECV) {
        ret = u ? -1 : ipc_recv(sp, regs->r[0],
                               (struct ipc_wire *)regs->r[1], regs->r[2]);
    } else if (num == SYS_RPC) {
        ret = u ? -1 : ipc_rpc(sp, regs->r[0],
                               (const struct ipc_wire *)regs->r[1],
                               regs->r[2], regs->r[3],
                               (struct ipc_wire *)regs->r[4], regs->r[5]);
    } else if (num == SYS_WRITE) {
        ret = u ? sys_write_user(t, regs) : -1;
    } else if (num == SYS_YIELD) {
        ret = sys_yield_user();
    } else if (num == SYS_EXIT) {
        sys_exit_user();    /* tidak kembali */
        ret = 0;            /* tak tercapai */
    } else if (num == SYS_RPC_USER) {
        ret = u ? sys_rpc_user(t, regs) : -1;
    } else if (num == SYS_SBRK) {
        ret = u ? sys_sbrk_user(t, regs) : -1;
    } else if (num == SYS_OPEN) {
        ret = u ? sys_open_user(t, regs) : -1;
    } else if (num == SYS_READ) {
        ret = u ? sys_read_user(t, regs) : -1;
    } else if (num == SYS_CLOSE) {
        ret = u ? sys_close_user(t, regs) : -1;
    } else if (num == SYS_LS) {
        ret = u ? sys_ls_user(t, regs) : -1;
    } else if (num == SYS_DELETE) {
        ret = u ? sys_delete_user(t, regs) : -1;
    } else if (num == SYS_GPIO_SET) {
        ret = u ? gpio_set(0u, regs->r[0], regs->r[1]) : -1;
    } else if (num == SYS_GPIO_GET) {
        ret = u ? gpio_get(0u, regs->r[0]) : -1;
    } else if (num == SYS_SD_READ) {
        ret = u ? sys_sd_read_user(regs) : -1;
    } else if (num == SYS_SD_WRITE) {
        ret = u ? sys_sd_write_user(regs) : -1;
    } else if (num == SYS_MKDIR) {
        ret = u ? sys_mkdir_user(t, regs) : -1;
    } else if (num == SYS_FAT_WRITE) {
        ret = u ? sys_fat_write_user(t, regs) : -1;
    } else if (num == SYS_FAT_READ) {
        ret = u ? sys_fat_read_user(t, regs) : -1;
    } else if (num == SYS_FAT_DELETE) {
        ret = u ? sys_fat_delete_user(t, regs) : -1;
    } else if (num == SYS_READDIR) {
        ret = u ? sys_readdir_user(t, regs) : -1;
    } else if (num == SYS_STAT) {
        ret = u ? sys_stat_user(regs) : -1;
    } else if (num == SYS_TLIST) {
        ret = u ? sys_tlist_user(regs) : -1;
    } else if (num == SYS_READ_CONSOLE) {
        ret = u ? sys_console_read_user() : -1;
    }
    if (ret != -2)
        regs->r[0] = (uint32_t)ret;
}
