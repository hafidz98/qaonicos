/*
 * sched.c - Preemptive round-robin scheduler.
 *
 * BUILD WITH -mgeneral-regs-only (see sched.h for why).
 */
#include "sched.h"
#include "gic.h"
#include "timer.h"
#include "vm.h"
#include "user.h"

static struct sched_thread threads[SCHED_MAX_THREADS];
static unsigned nthreads;
static unsigned cur;
static volatile unsigned ticks;
static uint32_t slice_ticks;

/* Fase 12d: idle thread untuk CPU accounting. idle_idx = indeks thread
 * idle (0xFFFFFFFF = belum didaftarkan); idle_ticks = jumlah tick 1ms
 * yang dijalankan thread idle. net_idx = indeks thread net (untuk
 * wakeup dari net_isr saat paket tiba). */
static unsigned idle_idx = 0xFFFFFFFFu;
static unsigned net_idx = 0xFFFFFFFFu;
static volatile unsigned idle_ticks;

void sched_set_idle(unsigned idx)
{
    idle_idx = idx;
}

unsigned sched_idle_ticks(void)
{
    return idle_ticks;
}

/* Fase 17: CPU% = 100*(1 - idle/total) per window >= 500ms.
 * Dipindah dari http.c (static cpu_pct_update) agar bisa dipakai
 * syscall SYS_STAT juga; state window dipakai bersama. */
unsigned sched_cpu_pct(void)
{
    static unsigned last_t, last_idle, pct;
    unsigned t = ticks;
    unsigned it = idle_ticks;
    unsigned dt = t - last_t;

    if (dt >= 500u) {
        unsigned di = it - last_idle;
        pct = (di >= dt) ? 0u : (100u * (dt - di) / dt);
        last_t = t;
        last_idle = it;
    }
    return pct;
}

void sched_set_net_idx(unsigned idx)
{
    net_idx = idx;
}

/* Bangunkan thread net bila sedang BLOCKED. Aman dari konteks IRQ
 * (net_isr): hanya store satu word ke state. */
void sched_wakeup_net(void)
{
    if (net_idx < nthreads &&
        threads[net_idx].state == THREAD_BLOCKED)
        threads[net_idx].state = THREAD_RUNNABLE;
}

/*
 * Fase 10: stack ABT per-thread.
 *
 * Root cause bug scheduler Fase 10: abort handler (data/prefetch)
 * berjalan di mode ABT di atas SATU stack ABT global (_abt_stack_top).
 * Thread yang block di pager IPC dari dalam abort handler (mis.
 * pclient menunggu halaman via ipc_recv) lalu ter-preempt tick timer
 * menyimpan frame IRQ dengan CPSR=ABT; thread lain yang fault
 * (mis. init via syscall file) memakai stack ABT global yang sama
 * dan menimpa rantai C thread yang ter-suspend. Saat thread pertama
 * dijadwalkan lagi, resume via rfeia ke mode ABT memakai SP_abt
 * yang isinya sampah -> abort (gejala: r6=0x80000093 dkk di
 * sched_on_tick, padahal itu akibat, bukan penyebab).
 *
 * Dengan stack ABT per-thread + save/restore SP_abt (banked register
 * per-CPU, seperti SP_usr) di tiap context switch, frame ABT milik
 * thread tetap utuh walau ia ter-preempt saat block di pager.
 */
#define ABT_STACK_SIZE 8192u   /* samakan dgn stack ABT shared semula */
static uint8_t abt_stacks[SCHED_MAX_THREADS][ABT_STACK_SIZE]
    __attribute__((aligned(8)));
/* SP_abt tersimpan per-thread: di-save saat thread ter-preempt,
 * di-restore + divalidasi saat thread dijadwalkan masuk. */
static uint32_t abt_sp_save[SCHED_MAX_THREADS];

void sched_init(void)
{
    nthreads = 0;
    cur = 0;
    ticks = 0;
    slice_ticks = 0;
}

void sched_add(void (*entry)(void), uint8_t *stack_top,
               struct task *task)
{
    uint32_t *f;
    unsigned i;

    if (nthreads >= SCHED_MAX_THREADS || !entry || !stack_top)
        return;
    f = (uint32_t *)stack_top - FR_WORDS;
    for (i = 0; i <= 12; i++)
        f[i] = 0u;                          /* r0..r12 */
    f[FR_LR] = 0u;                          /* lr_svc (tak dipakai saat start) */
    f[FR_PC] = (uint32_t)entry;
    f[FR_CPSR] = 0x13u;                 /* SVC mode, IRQ enabled */

    threads[nthreads].sp = f;
    for (i = 0; i < 32; i++)
        threads[nthreads].vfp.d[i] = 0u;
    threads[nthreads].vfp.fpscr = 0u;
    threads[nthreads].id = (int)nthreads;
    threads[nthreads].task = task;
    threads[nthreads].state = THREAD_RUNNABLE;
    threads[nthreads].user_sp = 0u;     /* tak dipakai thread kernel */
    threads[nthreads].user_lr = 0u;
    /* Fase 10: stack ABT privat thread ini. */
    abt_sp_save[nthreads] =
        (uint32_t)(abt_stacks[nthreads] + ABT_STACK_SIZE);
    nthreads++;
}

/* Set SP_usr (banked) sekali, lewat mode SYS yang berbagi register
 * banked dengan USR. Dipanggil dari SVC (privileged) saat pembuatan
 * thread; nilai per-thread yang otoritatif ada di
 * threads[].user_sp (di-restore tiap switch-in, Fase 9). */
static void user_sp_set(uint32_t sp)
{
    __asm__ volatile(
        "cps #0x1f\n\t"   /* SYS: berbagi SP/LR banked dengan USR */
        "mov sp, %0\n\t"
        "cps #0x13"         /* kembali ke SVC */
        :: "r"(sp) : "memory");
}

/* Baca banked SP/LR (USR/SYS). Dipanggil dari sched_on_tick yang
 * berjalan di SVC mode (stub IRQ vectors.S: cps #0x13 sebelum
 * c_irq_handler). cps hanya mengganti bit mode; bit I tetap ter-mask
 * (masuk IRQ me-mask IRQ), jadi jendela mode SYS ini atomik terhadap
 * tick. WAJIB kembali ke SVC (#0x13), bukan IRQ: sp_irq tidak dipakai
 * stub ini. */
static void banked_get(uint32_t *sp, uint32_t *lr)
{
    uint32_t s, l;
    __asm__ volatile(
"cps #0x1f\n\t"      /* SYS: banked SP/LR sama dengan USR */
        "mov %0, sp\n\t"
        "mov %1, lr\n\t"
        "cps #0x13"           /* kembali ke SVC (I tetap mask) */
        : "=r"(s), "=r"(l) :: "memory");
    *sp = s;
    *lr = l;
}

static void banked_set(uint32_t sp, uint32_t lr)
{
    __asm__ volatile(
"cps #0x1f\n\t"
        "mov sp, %0\n\t"
        "mov lr, %1\n\t"
        "cps #0x13"
        :: "r"(sp), "r"(lr) : "memory");
}

/* Baca/tulis banked SP_abt. Dipanggil dari SVC (sched_on_tick berjalan
 * di SVC pasca stub IRQ; sched_start juga SVC). cps hanya mengganti bit
 * mode; bit I tetap ter-mask (masuk IRQ me-mask IRQ), jadi jendela mode
 * ABT ini atomik terhadap tick. Pola sama dengan banked_get/set. */
static uint32_t abt_sp_get(void)
{
    uint32_t s;
    __asm__ volatile(
        "cps #0x17\n\t"   /* ABT: SP banked per-mode */
        "mov %0, sp\n\t"
        "cps #0x13"         /* kembali ke SVC */
        : "=r"(s) :: "memory", "r4");
    return s;
}

static void abt_sp_set(uint32_t s)
{
    /* Catatan: `cps` pada QEMU Cortex-A7 tampak mengkorup r4 (callee-
     * saved); r4 dideklarasikan clobber agar compiler menyelamatkannya. */
    __asm__ volatile(
        "cps #0x17\n\t"
        "mov sp, %0\n\t"
        "cps #0x13"
        :: "r"(s) : "memory", "r4");
}

void sched_add_user(uint8_t *stack_top, struct task *task,
                    uint32_t user_pc, uint32_t user_sp_top)
{
    uint32_t *f;
    unsigned i;

    if (nthreads >= SCHED_MAX_THREADS || !stack_top || !task)
        return;
    f = (uint32_t *)stack_top - FR_WORDS;
    for (i = 0; i <= 12; i++)
        f[i] = 0u;                          /* r0..r12 */
    f[FR_LR] = 0u;                          /* lr_svc (tak dipakai thread user) */
    f[FR_PC] = user_pc;
    f[FR_CPSR] = 0x10u;                 /* USR mode, IRQ enabled */

    threads[nthreads].sp = f;
    for (i = 0; i < 32; i++)
        threads[nthreads].vfp.d[i] = 0u;
    threads[nthreads].vfp.fpscr = 0u;
    threads[nthreads].id = (int)nthreads;
    threads[nthreads].task = task;
    threads[nthreads].state = THREAD_RUNNABLE;
    /* Fase 9: SP_usr/LR_usr per-thread (di-restore tiap switch-in).
     * user_sp_set() di bawah hanya untuk nilai awal sebelum tick
     * pertama; yang otoritatif adalah field ini. */
    threads[nthreads].user_sp = user_sp_top;
    threads[nthreads].user_lr = 0u;
    /* Fase 10: stack ABT privat thread ini. */
    abt_sp_save[nthreads] =
        (uint32_t)(abt_stacks[nthreads] + ABT_STACK_SIZE);
    nthreads++;

    user_sp_set(user_sp_top);
}

void sched_set_slice(uint32_t ticks)
{
    slice_ticks = ticks;
}

unsigned sched_ticks(void)
{
    return ticks;
}

/* Fase 12: introspeksi thread untuk HTTP /metrics. */
unsigned sched_thread_count(void)
{
    return nthreads;
}

const struct sched_thread *sched_thread_at(unsigned i)
{
    if (i >= nthreads)
        return 0;
    return &threads[i];
}

struct sched_thread *sched_current_thread(void)
{
    if (nthreads == 0)
        return 0;
    return &threads[cur];
}

struct task *sched_current_task(void)
{
    struct sched_thread *t = sched_current_thread();
    return t ? t->task : 0;
}

void sched_block_current(void)
{
    struct sched_thread *t = sched_current_thread();
    unsigned cpsr;

    if (!t)
        return;
    /*
     * Tandai BLOCKED secara atomik terhadap tick timer, lalu spin.
     *
     * Kenapa spin, bukan switch langsung dari sini: kita dipanggil
     * dari dalam SVC (di bawah frame SVC masih ada call chain C
     * arm_trap -> svc_dispatch -> ipc_recv), dan frame SVC
     * (r0..r12, pad, lr_svc, spsr_svc) TIDAK kompatibel dengan frame
     * 16-word IRQ ([0]=pad, [1..13]=r0..r12, [14]=pc, [15]=cpsr) yang
     * dipakai scheduler. Jadi serahkan perpindahan ke tick preemptif:
     * tick berikutnya melihat status BLOCKED dan memilih thread lain.
     * Pengirim membangunkan kita via sched_wakeup(); saat dijadwalkan
     * lagi, eksekusi lanjut tepat setelah spin ini.
     */
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    t->state = THREAD_BLOCKED;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    while (t->state == THREAD_BLOCKED) {
        /* IRQ hidup: tick bisa menyela kapan saja. */
    }
}

void sched_wakeup(struct sched_thread *t)
{
    if (t)
        t->state = THREAD_RUNNABLE;
}

void sched_start(void)
{
    uint32_t *f;

    if (nthreads == 0)
        return;
    cur = 0;
    /*
     * Sinkronkan TTBR0 + vm_cur_space ke task thread pertama sebelum
     * rfeia. Mulai titik ini invariant "ruang alamat == vm milik task
     * thread berjalan" dipegang oleh sched_on_tick().
     */
    if (threads[0].task)
        vm_space_switch(&threads[0].task->vm);
    fpu_restore(&threads[0].vfp);   /* start with clean VFP */
    /* Fase 10: mulai sekarang SP_abt = stack ABT privat thread 0.
     * Invarian "SP_abt == stack ABT thread berjalan" dipegang oleh
     * sched_on_tick() mulai titik ini. */
    abt_sp_set(abt_sp_save[0]);
    f = threads[0].sp;
    __asm__ volatile(
        "mov sp, %0\n\t"
        "pop {r0-r12, lr}\n\t"         /* r0..r12, lr_svc (tanpa pad) */
        "rfeia sp!"                 /* pc=f[14], cpsr=f[15] */
        :: "r"(f) : "memory");
    __builtin_unreachable();
}

uint32_t *sched_on_tick(uint32_t *frame)
{
    /* No VFP use in this function (build flag); the save/restore below
     * captures the outgoing/incoming thread's real VFP registers. */
    struct sched_thread *out;
    struct sched_thread *in;
    unsigned nxt, i;

    /* Invariant: threads[cur] harus thread cur. Menangkap korupsi cur
     * (mis. stack menimpa BSS) sebelum out->sp=frame merambat. */
    if (cur >= nthreads || threads[cur].id != (int)cur) {
        uputs("\n[SCHED] invariant rusak: cur/id!\n");
        for (;;) { }
    }
    out = &threads[cur];
    nxt = cur;

    fpu_save(&out->vfp);
    out->sp = frame;
    /* Fase 10: selamatkan SP_abt thread keluar. */
    abt_sp_save[cur] = abt_sp_get();
    /* Fase 9: selamatkan banked SP/LR bila thread KELUAR adalah
     * thread user. Syaratnya = thread-nya user (user_sp != 0), BUKAN
     * frame-nya USR: tick bisa mempreempt thread user di dalam SVC
     * (tengah syscall) sehingga frame-nya SVC; banked SP/LR fisik
     * tetap milik thread ini (syscall tak menyentuh SP_usr/LR_usr,
     * single-CPU) dan wajib diselamatkan. Versi lama memakai
     * frame_is_user() sehingga thread user yang di-preempt di dalam
     * syscall tidak di-save dan saat switch-in mewarisi SP_usr fisik
     * thread user lain -> stack tertukar -> fault misterius. */
    if (out->user_sp != 0u)
        banked_get(&out->user_sp, &out->user_lr);
    ticks++;

    /*
     * Round-robin ke thread RUNNABLE berikutnya; yang BLOCKED dilewati.
     * Fase 12d: thread idle dilewati di sini — ia pilihan terakhir,
     * dipilih hanya bila tak ada thread RUNNABLE lain (untuk CPU
     * accounting yang jujur). Kalau semua blocked (tidak terjadi di
     * tes ini - selalu ada yang runnable), tetap di thread sekarang
     * agar tidak crash.
     */
    for (i = 0; i < nthreads; i++) {
        nxt++;
        if (nxt >= nthreads)
            nxt = 0;
        if (nxt == idle_idx)
            continue;               /* idle: hanya bila tak ada yang lain */
        if (threads[nxt].state == THREAD_RUNNABLE)
            break;
    }
    if (threads[nxt].state != THREAD_RUNNABLE &&
        idle_idx < nthreads &&
        threads[idle_idx].state == THREAD_RUNNABLE)
        nxt = idle_idx;             /* sistem idle */
    if (threads[nxt].state != THREAD_RUNNABLE)
        nxt = cur;
    cur = nxt;
    in = &threads[cur];
    /* Fase 12d: hitung tick idle untuk CPU%. */
    if (cur == idle_idx)
        idle_ticks++;

    /*
     * Fase 10: pulihkan SP_abt milik thread masuk, dengan validasi.
     * SP_abt harus berada dalam stack ABT privat thread ini; bila di
     * luar range, memori sudah korup -> parkir dengan diagnostik
     * (jangan pasang SP sampah). Frame juga divalidasi minimal
     * (non-NULL, 4-byte aligned) sebelum dipakai.
     */
    {
        uint32_t asp = abt_sp_save[cur];
        uint32_t lo = (uint32_t)&abt_stacks[cur][0];
        uint32_t hi = lo + ABT_STACK_SIZE;
        if (asp < lo || asp > hi) {
            uputs("\n[SCHED] ABT SP rusak!\n");
            for (;;) { }
        }
        if (!in->sp || ((uint32_t)in->sp & 3u)) {
            uputs("\n[SCHED] frame SP rusak!\n");
            for (;;) { }
        }
        abt_sp_set(asp);
    }

    /*
     * Ganti protection domain bila task-nya beda: TTBR0 + TLB flush.
     * Berjalan dalam konteks IRQ (IRQ ter-mask oleh exception itu
     * sendiri), tanpa mask/unmask manual seperti vm_probe - pola yang
     * sama dengan switch eksplisit Fase 5 yang stabil 25/25 run.
     */
    if (in->task && out->task && in->task != out->task)
        vm_space_switch(&in->task->vm);

    fpu_restore(&in->vfp);
    /* Fase 9: pulihkan banked SP/LR bila thread MASUK adalah thread
     * user (user_sp != 0) — tanpa syarat mode frame, simetris dengan
     * save di atas. Wajib selalu dipulihkan: banked register fisik
     * itu satu untuk semua thread user, jadi tiap switch-in ke thread
     * user harus memasang miliknya sendiri. */
    if (in->user_sp != 0u)
        banked_set(in->user_sp, in->user_lr);
    return in->sp;
}

/* Fase 11: dispatch IRQ device (mis. virtio-net). ID 32..159. */
#define DEV_IRQ_BASE 32u
#define DEV_IRQ_N    128u
static void (*dev_irq_fn[DEV_IRQ_N])(void);

void irq_dev_register(unsigned id, void (*fn)(void))
{
    if (id >= DEV_IRQ_BASE && id < DEV_IRQ_BASE + DEV_IRQ_N)
        dev_irq_fn[id - DEV_IRQ_BASE] = fn;
}

uint32_t *c_irq_handler(uint32_t *frame)
{
    unsigned id = gic_ack();

    if (id == TIMER_PPI_IRQ) {
        /* Reprogram first (clears the level), then EOI, then switch. */
        timer_set_tval(slice_ticks);
        gic_eoi(id);
        return sched_on_tick(frame);
    }
    if (id >= DEV_IRQ_BASE && id < DEV_IRQ_BASE + DEV_IRQ_N &&
        dev_irq_fn[id - DEV_IRQ_BASE]) {
        dev_irq_fn[id - DEV_IRQ_BASE]();
        gic_eoi(id);
        return frame;              /* device IRQ: tak pernah switch */
    }
    gic_eoi(id);
    return frame;
}
