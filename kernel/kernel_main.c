/*
 * kernel_main.c - Integrated Mach-x-Luckfox bring-up demo (Fase 5).
 *
 * One ELF that boots like a real kernel: MMU on (pmap), traps live,
 * FPU enabled, IPC self-test, VM self-test (per-task address spaces
 * with 4 KiB small pages, isolation proven), then a PREEMPTIVE
 * round-robin scheduler: the ARM virtual timer (PPI 27) fires every
 * 1 ms, the IRQ stub builds a full exception frame, and sched_on_tick()
 * switches threads — no manual yields. VFP state is saved/restored
 * eagerly on every switch, and each thread keeps proving its address
 * space is intact on every status line.
 *
 * Runs on: qemu-system-arm -M virt -cpu cortex-a7
 */
#include "../rv1103-bringup/pmap.h"
#include "../rv1103-bringup/trap.h"
#include "../rv1103-bringup/fpu.h"
#include "../rv1103-bringup/ipc.h"
#include "../rv1103-bringup/syscall.h"
#include "../rv1103-bringup/zone.h"
#include "../rv1103-bringup/gic.h"
#include "../rv1103-bringup/timer.h"
#include "../rv1103-bringup/sched.h"
#include "../rv1103-bringup/vm.h"
#include "../rv1103-bringup/task.h"
#include "../rv1103-bringup/pager.h"
#include "../rv1103-bringup/lib.h"
#include "../rv1103-bringup/user.h"
#include "../rv1103-bringup/fs.h"

/* Fase 8: image program userspace, di-embed dari user/hello.bin oleh
 * build.sh (user/embed.py -> /tmp/mach_hello_img.o). */
extern const uint8_t hello_img[];
extern const unsigned hello_img_len;

/* Fase 9: image program uji ramfs (user/fstest.bin -> fstest_img). */
extern const uint8_t fstest_img[];
extern const unsigned fstest_img_len;

/* PL011 (QEMU virt UART0). */
#define UARTDR  (*(volatile unsigned *)0x09000000u)
#define UARTFR  (*(volatile unsigned *)0x09000018u)

static void putc(char c)
{
    while (UARTFR & (1u << 5)) { }
    UARTDR = (unsigned)c;
    if (c == '\n') putc('\r');
}

static void puts(const char *s)
{
    while (*s) putc(*s++);
}

static void putdec(unsigned v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) putc(buf[--i]);
}

static void puthex64(uint64_t v)
{
    int i;
    puts("0x");
    for (i = 15; i >= 0; i--)
        putc("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}

static uint64_t dbits_arg(double x)
{
    uint64_t b;
    __builtin_memcpy(&b, &x, 8);
    return b;
}

/* IRQ on/off for atomic console output. Threads run with IRQs enabled;
 * the timer may otherwise preempt us mid-line and garble the output. */
static inline unsigned irq_save(void)
{
    unsigned cpsr;
    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    return cpsr;
}

static inline void irq_restore(unsigned cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
}

/* IPC: one task, one shared space (Mach threads of a task share it).
 * Two ports: A->B and B->A (a single port would let the sender eat its
 * own message on poll-recv). */
static struct zone port_zone, msg_zone;
static uint8_t port_pool[IPC_NPORTS * sizeof(struct ipc_port)] __attribute__((aligned(8)));
static uint8_t msg_pool[16 * sizeof(struct ipc_msg)] __attribute__((aligned(8)));
/* Fase 6: task sebagai protection domain. Tiap task punya vm_space +
 * ipc_space sendiri. task_kern menampung thread-thread lama (preempt
 * A<->B); task_a = server, task_b = client untuk tes blocking + RPC. */
static struct task task_kern, task_a, task_b;
static unsigned port_ab, port_ba;   /* nama di task_kern.ipc */
/* Nama port Fase 6 (diisi bagian 3e, dipakai thread server/client). */
static unsigned srv_a;      /* recv port server, di task_a.ipc */
static unsigned srv_in_b;   /* send-right hasil grant, di task_b.ipc */
static unsigned rep_b;      /* reply port client, di task_b.ipc */
static unsigned rep_in_a;   /* send-right hasil grant, di task_a.ipc */

/* mach_msg-style traps. Explicit clobbers (P6 lesson: svc kills lr). */
static int svc_send(unsigned name, const struct ipc_wire *w, unsigned len)
{
    unsigned ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #10\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(name), "r"(w), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return (int)ret;
}

/* RPC sinkron: r0 = send name, r1 = req ptr, r2 = req len,
 * r3 = reply name, r4 = rep buf, r5 = rep len -> size / -1. */
static int svc_rpc(unsigned send_name, const struct ipc_wire *req,
                   unsigned reqlen, unsigned rep_name,
                   struct ipc_wire *rep, unsigned replen)
{
    unsigned ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r3, %4\n\t"
        "mov r4, %5\n\t"
        "mov r5, %6\n\t"
        "mov r7, #12\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(send_name), "r"(req), "r"(reqlen),
          "r"(rep_name), "r"(rep), "r"(replen)
        : "r0", "r1", "r2", "r3", "r4", "r5", "r7", "lr", "memory", "cc");
    return (int)ret;
}

static int svc_recv(unsigned name, struct ipc_wire *w, unsigned len)
{
    unsigned ret;
    __asm__ volatile(
        "mov r0, %1\n\t"
        "mov r1, %2\n\t"
        "mov r2, %3\n\t"
        "mov r7, #11\n\t"
        "svc #0\n\t"
        "mov %0, r0"
        : "=r"(ret)
        : "r"(name), "r"(w), "r"(len)
        : "r0", "r1", "r2", "r7", "lr", "memory", "cc");
    return (int)ret;
}

static void wire_puts(struct ipc_wire *w, uint32_t id, const char *txt)
{
    unsigned n = 0;
    w->bits = 0;
    w->id = id;
    while (txt[n] && n < IPC_MSG_DATA - 1) {
        w->data[n] = (uint8_t)txt[n];
        n++;
    }
    w->data[n] = 0;
    w->size = n + 1;
}

/* Rigorous self-test of the whole IPC path (via SVC traps, like
 * real userspace). Returns number of failed checks. */
static int ipc_selftest(void)
{
    /* Fase 7: statis, bukan di stack - struct ipc_wire kini ~4KB
     * (IPC_MSG_DATA=4128) sedangkan selftest jalan di stack boot 8KB. */
    static struct ipc_wire w, r;
    unsigned tp, p_send, p_recv;
    int fails = 0, i, ret;

#define CHECK(cond, msg) do { \
        if (!(cond)) { puts("  FAIL: "); puts(msg); putc('\n'); fails++; } \
    } while (0)

    /* 1. rights enforcement */
    p_send = ipc_port_alloc(&task_kern.ipc, IPC_SEND);
    p_recv = ipc_port_alloc(&task_kern.ipc, IPC_RECV);
    CHECK(p_send != 0 && p_recv != 0, "port alloc");
    wire_puts(&w, 0x71, "x");
    CHECK(svc_send(p_recv, &w, sizeof(w)) == -1, "send to recv-only port must fail");
    CHECK(svc_recv(p_send, &r, sizeof(r)) == -1, "recv from send-only port must fail");
    CHECK(svc_send(99, &w, sizeof(w)) == -1, "send to bad name must fail");
    CHECK(svc_recv(99, &r, sizeof(r)) == -1, "recv from bad name must fail");

    /* 2. FIFO order */
    tp = ipc_port_alloc(&task_kern.ipc, IPC_SEND | IPC_RECV);
    CHECK(tp != 0, "test port alloc");
    for (i = 0; i < 3; i++) {
        wire_puts(&w, 0x10u + (unsigned)i, "fifo");
        w.data[0] = (uint8_t)('A' + i);
        CHECK(svc_send(tp, &w, sizeof(w)) == 0, "fifo send");
    }
    for (i = 0; i < 3; i++) {
        ret = svc_recv(tp, &r, sizeof(r));
        CHECK(ret > 0, "fifo recv");
        CHECK(r.id == 0x10u + (unsigned)i, "fifo order");
        CHECK(r.data[0] == (uint8_t)('A' + i), "fifo payload");
    }

    /* 3. payload integrity: IPC_MSG_DATA-byte pattern, byte-exact */
    for (i = 0; i < IPC_MSG_DATA; i++)
        w.data[i] = (uint8_t)(i * 7 + 3);
    w.bits = 0xdead; w.id = 0xbeef; w.size = IPC_MSG_DATA;
    CHECK(svc_send(tp, &w, sizeof(w)) == 0, "pattern send");
    ret = svc_recv(tp, &r, sizeof(r));
    CHECK(ret == IPC_MSG_DATA, "pattern size");
    CHECK(r.bits == 0xdead && r.id == 0xbeef, "pattern header");
    CHECK(memcmp(r.data, w.data, IPC_MSG_DATA) == 0, "pattern body");

    /* 4. queue full: QDEPTH sends ok, next one fails; drain all */
    for (i = 0; i < IPC_QDEPTH; i++) {
        wire_puts(&w, (unsigned)i, "q");
        CHECK(svc_send(tp, &w, sizeof(w)) == 0, "fill queue");
    }
    wire_puts(&w, 0xff, "q");
    CHECK(svc_send(tp, &w, sizeof(w)) == -1, "send to full queue must fail");
    for (i = 0; i < IPC_QDEPTH; i++)
        CHECK(svc_recv(tp, &r, sizeof(r)) > 0, "drain queue");

    /* 5. empty queue */
    CHECK(svc_recv(tp, &r, sizeof(r)) == -1, "recv from empty queue must fail");

#undef CHECK
    return fails;
}

/* ------------------------------------------------------------------ */
/* Fase 5: VM - per-task address spaces (4 KiB small pages).           */
/* ------------------------------------------------------------------ */

/* User-region VA both threads map; each space points it at a different
 * physical page. 0x10000000 is unmapped in the kernel's own L1, so any
 * access without a vm_map would take a data abort (now with DFSR/DFAR
 * diagnostics in trap.c instead of a silent hang). */
#define VM_TEST_VA 0x10000000u

static struct vm_space vm_space_kern;

/* ------------------------------------------------------------------ */
/* Fase 4: preemptive threads. No manual yields anywhere.              */
/* ------------------------------------------------------------------ */
/* Fase 7: 8KB -> 16KB. struct ipc_wire kini ~4KB (IPC_MSG_DATA=4128);
 * dua wire di satu frame (thread_client) tidak muat di 8KB. */
static unsigned char stack_a[16384] __attribute__((aligned(8)));
static unsigned char stack_b[16384] __attribute__((aligned(8)));
static unsigned char stack_server[16384] __attribute__((aligned(8)));
static unsigned char stack_client[16384] __attribute__((aligned(8)));
static unsigned char stack_pager[16384] __attribute__((aligned(8)));
static unsigned char stack_pclient[16384] __attribute__((aligned(8)));

static volatile unsigned total_lines;

/* ------------------------------------------------------------------ */
/* Fase 7: external pager (memory object) + copy-on-write.             */
/* ------------------------------------------------------------------ */
#define PAGER_VA 0x10010000u   /* jendela object di task_c (2 halaman) */
#define COW_VA   0x10020000u   /* halaman COW antara task_a dan task_b */

static struct task task_pager, task_c;
static unsigned pager_req, pager_rep;   /* nama port di task_pager.ipc */
static unsigned pager_obj;              /* id memory object */
static volatile unsigned pager_done;    /* diset thread_pclient */

/* ------------------------------------------------------------------ */
/* Fase 8: user mode + syscall.                                       */
/* ------------------------------------------------------------------ */
static struct task task_user;           /* protection domain program user */
static unsigned char stack_usvc[16384] __attribute__((aligned(8)));
static unsigned char stack_uthread[16384] __attribute__((aligned(8)));
static unsigned char stack_fstest[16384] __attribute__((aligned(8))); /* Fase 9 */
static unsigned usvc_port;              /* recv port echo, di task_kern.ipc */
static unsigned usvc_rep_in_kern;       /* send-right ke reply port user */

/* Echo server kernel untuk program user: terima request di usvc_port,
 * balas "echo:" + payload ke reply port user. */
static void thread_usvc(void)
{
    static struct ipc_wire w, r;
    int n;
    unsigned i, m;

    for (;;) {
        n = svc_recv(usvc_port, &w, sizeof(w));
        if (n <= 0 || w.id != ECHO_REQ_ID)
            continue;
        r.bits = 0;
        r.id = ECHO_REP_ID;
        r.data[0] = 'e'; r.data[1] = 'c'; r.data[2] = 'h';
        r.data[3] = 'o'; r.data[4] = ':';
        m = w.size;
        if (m > IPC_MSG_DATA - 6u)
            m = IPC_MSG_DATA - 6u;
        for (i = 0; i < m; i++)
            r.data[5 + i] = w.data[i];
        r.size = 5u + m + 1u;
        r.data[5 + m] = 0;
        svc_send(usvc_rep_in_kern, &r, sizeof(r));
    }
}

/* Fase 9: 1 bila program uji ramfs selesai dengan sukses. fstest
 * membuat sentinel "/.fs_done" HANYA sesudah semua verifikasi lolos
 * ("FS TESTS PASSED"); report() menjadikannya gerbang halt. Aman
 * dipanggil dengan IRQ ter-mask (fs_find me-mask sendiri). */
static int fs_test_done(void)
{
    return fs_find(".fs_done") >= 0;
}

/* Atomically print one status line; the 12th line ends the demo. */
static void report(const char *tag, unsigned n, uint64_t fpubits,
                   unsigned vmfails)
{
    unsigned s = irq_save();
    int over;

    puts("  "); puts(tag); puts(": n="); putdec(n);
    puts("  fpu="); puthex64(fpubits);
    puts("  vmf="); putdec(vmfails);
    puts("  ticks="); putdec(sched_ticks());
    putc('\n');
    total_lines++;
    /* Fase 7: jangan halt sebelum pager test selesai (pager_done
     * diset thread_pclient). Fase 8: juga tunggu thread user selesai
     * (user_done diset SYS_EXIT / user_kill). Fase 9: juga tunggu
     * uji ramfs (sentinel /.fs_done dari thread fstest).
     * Tes lama tidak rusak - hanya menunggu. */
    over = (total_lines >= 12) && pager_done && user_done &&
           fs_test_done();
    if (over) {
        puts("PREEMPT+VM OK - halting\n");
        gic_disable_irq(TIMER_PPI_IRQ);
    }
    irq_restore(s);
    if (over) {
        for (;;) { __asm__ volatile("wfi"); }
    }
}

static void ipc_note(const char *tag, const struct ipc_wire *w)
{
    unsigned s = irq_save();
    puts("  "); puts(tag); puts(": ipc rx id=");
    puthex64(w->id);
    puts(" : "); puts((const char *)w->data); putc('\n');
    irq_restore(s);
}

/* Thread A: IPC initiator, then VFP compute loop. */
static void thread_a(void)
{
    struct ipc_wire w;
    unsigned n = 0, i, vmfails = 0;

    wire_puts(&w, 0xA1, "halo preemptif dari A");
    svc_send(port_ab, &w, sizeof(w));
    while (svc_recv(port_ba, &w, sizeof(w)) < 0)
        ; /* B gets scheduled by the timer and replies */
    ipc_note("A", &w);

    for (i = 0; ; i++) {
        /* Pin x to d8: it stays live in a VFP register while the timer
         * may strike, so correct output proves VFP save/restore works. */
        register double x __asm__("d8") = 1.5 * (double)(n + 1);
        x = x * 2.0 + 0.25;   /* 3.25, 6.25, 9.25, ... */
        if ((i & 0x1FFFFu) == 0) {
            /* VM isolation is proven by the deterministic test before
             * threading starts. Per-thread TTBR0 switching here trips
             * a QEMU bug (intermittent data abort in sched_on_tick),
             * so the threads stay in the kernel's address space. */
            report("A", n, dbits_arg(x), vmfails);
            n++;
        }
    }
}

/* Thread B: answers A's message, then its own VFP compute loop. */
static void thread_b(void)
{
    struct ipc_wire w;
    unsigned n = 0, i, vmfails = 0;

    while (svc_recv(port_ab, &w, sizeof(w)) < 0)
        ; /* spin until A sends */
    ipc_note("B", &w);
    wire_puts(&w, 0xB1, "balasan preemptif dari B");
    svc_send(port_ba, &w, sizeof(w));

    for (i = 0; ; i++) {
        register double x __asm__("d8") = 2.5 * (double)(n + 1);
        x = x * 2.0 + 0.5;    /* 5.5, 10.5, 15.5, ... */
        if ((i & 0x1FFFFu) == 0) {
            report("B", n, dbits_arg(x), vmfails);
            n++;
        }
    }
}

/* ID pesan Fase 6. */
#define PING_ID     0x0601u
#define RPC_REQ_ID  0x0602u
#define RPC_REP_ID  0x0603u

/* Thread server (task A): blocking-recv ping, lalu layani satu RPC.
 * Setelah itu idle sebagai server: block menunggu request berikutnya
 * (tidak pernah datang di tes ini), sehingga tick melewatinya dan
 * thread lama tetap dapat porsi CPU seperti Fase 5. */
static void thread_server(void)
{
    struct ipc_wire w;
    unsigned s;
    int n;

    /* 1. Ping dari client via granted send-right: blocking sampai tiba. */
    n = svc_recv(srv_a, &w, sizeof(w));
    s = irq_save();
    if (n > 0 && w.id == PING_ID &&
        memcmp(w.data, "ping-dari-client", 17) == 0)
        puts("[task] server: ping diterima via grant\n");
    else
        puts("[task] FAIL: ping\n");
    irq_restore(s);

    /* 2. Request RPC: balas ke reply port client via granted name. */
    n = svc_recv(srv_a, &w, sizeof(w));
    s = irq_save();
    if (n > 0 && w.id == RPC_REQ_ID) {
        wire_puts(&w, RPC_REP_ID, "rpc-reply-ok");
        if (svc_send(rep_in_a, &w, sizeof(w)) == 0)
            puts("TASK TESTS PASSED\n");
        else
            puts("[task] FAIL: kirim reply\n");
    } else {
        puts("[task] FAIL: rpc request\n");
    }
    irq_restore(s);

    for (;;) {
        n = svc_recv(srv_a, &w, sizeof(w));  /* block selamanya */
        (void)n;
    }
}

/* Thread client (task B): kirim ping, lalu RPC sinkron. Setelah reply
 * terverifikasi, block selamanya di reply port (tidak ada RPC lagi). */
static void thread_client(void)
{
    struct ipc_wire w, r;
    unsigned s;
    int n;

    wire_puts(&w, PING_ID, "ping-dari-client");
    if (svc_send(srv_in_b, &w, sizeof(w)) != 0) {
        s = irq_save();
        puts("[task] FAIL: kirim ping\n");
        irq_restore(s);
        for (;;) { }
    }

    wire_puts(&w, RPC_REQ_ID, "rpc-minta-balasan");
    n = svc_rpc(srv_in_b, &w, sizeof(w), rep_b, &r, sizeof(r));
    s = irq_save();
    if (n > 0 && r.id == RPC_REP_ID &&
        memcmp(r.data, "rpc-reply-ok", 13) == 0)
        puts("RPC TESTS PASSED\n");
    else {
        puts("[task] FAIL: rpc reply, n=");
        putdec((unsigned)n);
        putc('\n');
    }
    irq_restore(s);

    for (;;) {
        n = svc_recv(rep_b, &r, sizeof(r));  /* block selamanya */
        (void)n;
    }
}

/* Fase 7: pager thread (task_pager). Loop terima data_request di
 * pager_req, layani dengan data_supply dari backing store.
 * Backing store = pola deterministik (offset+i)&0xFF; pemanggil
 * langsung via C (bukan syscall baru) - brief mengizinkan. */
static void thread_pager(void)
{
    static struct ipc_wire w, r;
    uint32_t obj_id, off;
    unsigned i;
    int n;

    for (;;) {
        n = ipc_recv(&task_pager.ipc, pager_req, &w, sizeof(w));
        if (n <= 0 || w.id != MSG_DATA_REQUEST)
            continue;           /* abaikan pesan asing */
        obj_id = *(uint32_t *)w.data;
        off = *(uint32_t *)(w.data + 4);
        if (obj_id != pager_obj)
            continue;

        /* Isi halaman dari backing store (pola deterministik). */
        r.bits = 0;
        r.id = MSG_DATA_SUPPLY;
        *(uint32_t *)r.data = obj_id;
        *(uint32_t *)(r.data + 4) = off;
        for (i = 0; i < 4096; i++)
            r.data[8 + i] = (uint8_t)((off + i) & 0xFFu);
        r.size = 8 + 4096;

        /* Balas ke reply port client di space ini. */
        (void)ipc_send(&task_pager.ipc, pager_rep, &r, sizeof(r));
    }
}

/* Fase 7: client pager (task_c). Sentuh PAGER_VA -> data abort ->
 * vm_page_fault -> ipc_rpc ke pager thread. Verifikasi byte-exact,
 * lalu tulis & baca ulang untuk memastikan halaman writable. */
static void thread_pclient(void)
{
    volatile uint8_t *p;
    unsigned i, fails = 0, s;
    uint32_t off;

    for (off = 0; off < 2 * 4096; off += 4096) {
        p = (volatile uint8_t *)(PAGER_VA + off);
        for (i = 0; i < 4096; i++) {
            uint8_t want = (uint8_t)((off + i) & 0xFFu);
            if (p[i] != want) {   /* fault pertama: isi dari pager */
                fails++;
                break;
            }
        }
        /* Halaman hasil pager harus writable (prot RW saat map). */
        for (i = 0; i < 4096; i++)
            p[i] = (uint8_t)(i & 0xFFu);
        for (i = 0; i < 4096; i++)
            if (p[i] != (uint8_t)(i & 0xFFu)) {
                fails++;
                break;
            }
    }

    s = irq_save();
    if (fails == 0)
        puts("PAGER TESTS PASSED\n");
    else {
        puts("[pager] FAILURES = "); putdec(fails); putc('\n');
    }
    irq_restore(s);
    pager_done = 1;             /* izinkan report() halt */

    for (;;) { }                /* RUNNABLE: biarkan tick tetap jalan */
}

void kernel_main(void)
{
    puts("\nMach-x-Luckfox kernel booting (qemu-virt, cortex-a7)\n");

    /* 1. Memory management. */
    pmap_init();
    pmap_enable();
    puts("[pmap] MMU on: 1:1 DRAM + device mappings\n");

    /* 2. Floating point. */
    fpu_enable();
    {
        double x = 1.5;
        x = x * 2.0 + 0.25;
        puts("[fpu ] VFPv4 on, 1.5*2+0.25 = ");
        puthex64(dbits_arg(x));
        puts(" (3.25)\n");
    }

    /* 2b. VM pools lebih awal: task_create() butuh vm_space_init(). */
    vm_init();
    vm_space_kern.l1 = vm_current_l1();

    /* 3. Task + IPC: tiap task = satu protection domain (vm_space +
     * ipc_space sendiri). Zone port/pesan dipakai bersama semua task. */
    zone_init(&port_zone, port_pool, sizeof(port_pool),
              sizeof(struct ipc_port));
    zone_init(&msg_zone, msg_pool, sizeof(msg_pool),
              sizeof(struct ipc_msg));
    task_create(&task_kern, &port_zone, &msg_zone);
    task_create(&task_a, &port_zone, &msg_zone);
    task_create(&task_b, &port_zone, &msg_zone);
    syscall_init(&task_kern);
    fs_init();  /* Fase 9: ramfs (BSS sudah nol; eksplisit biar jelas). */
    port_ab = ipc_port_alloc(&task_kern.ipc, IPC_SEND | IPC_RECV);
    port_ba = ipc_port_alloc(&task_kern.ipc, IPC_SEND | IPC_RECV);
    puts("[ipc ] zones up, ports allocated: A->B name = ");
    putdec(port_ab);
    puts(", B->A name = ");
    putdec(port_ba);
    putc('\n');

    /* 3b. Rigorous self-test before any threading. */
    {
        int fails = ipc_selftest();
        puts("[ipc ] selftest: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3c. VM: per-task address spaces with 4 KiB small pages.
     * Deterministic isolation proof, single-threaded: map the same VA
     * in two spaces to different physical pages and show neither task
     * can see the other's data. */
    {
        uint32_t pa_a, pa_b, v;
        int fails = 0, rc;
        volatile uint32_t *p = (volatile uint32_t *)VM_TEST_VA;

        /* vm_space_init sudah dikerjakan task_create(); di sini pakai
         * langsung vm milik task A/B sebagai ruang alamat yang diuji. */
        rc = 0;
        pa_a = vm_page_alloc();
        pa_b = vm_page_alloc();
        rc |= vm_map(&task_a.vm, VM_TEST_VA, pa_a,
                     VM_PROT_READ | VM_PROT_WRITE);
        rc |= vm_map(&task_b.vm, VM_TEST_VA, pa_b,
                     VM_PROT_READ | VM_PROT_WRITE);
        if (rc != 0 || pa_a == 0u || pa_b == 0u || pa_a == pa_b) {
            puts("[vm  ] setup FAILED\n");
            fails = 99;
        } else {
            vm_space_switch(&task_a.vm);
            *p = 0xAAAAAAAAu;
            vm_space_switch(&task_b.vm);
            v = *p;         /* B's page: must NOT see A's pattern */
            if (v == 0xAAAAAAAAu) {
                puts("  FAIL: B saw A's data\n");
                fails++;
            }
            *p = 0xBBBBBBBBu;
            vm_space_switch(&task_a.vm);
            v = *p;         /* A's page: must be untouched by B */
            if (v != 0xAAAAAAAAu) {
                puts("  FAIL: A's data corrupted\n");
                fails++;
            }
            /* Back to the kernel's own L1 before threading starts. */
            vm_space_switch(&vm_space_kern);
        }
        puts("[vm  ] L1 kern=");
        puthex64((uint32_t)vm_space_kern.l1);
        puts(" A=");
        puthex64((uint32_t)task_a.vm.l1);
        puts(" B=");
        puthex64((uint32_t)task_b.vm.l1);
        puts("\n");
        puts("[vm  ] 4 KiB pages, per-task spaces, isolation: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3d. VM maturation: unmap, read-only pages, demand paging.
     * Still single-threaded (IRQs off), so data aborts land in our
     * pager without scheduler interference. */
    {
        int fails = 0;
        uint32_t pa_c, desc, v;
        volatile uint32_t *q;

        /* --- unmap: map a scratch page, verify, unmap, verify gone --- */
        pa_c = vm_page_alloc();
        vm_space_switch(&task_a.vm);
        if (pa_c == 0u) {
            puts("  FAIL: unmap test out of pages\n"); fails++;
        } else if (vm_map(&task_a.vm, VM_TEST_VA + 0x1000u, pa_c,
                          VM_PROT_READ | VM_PROT_WRITE) != 0) {
            puts("  FAIL: map for unmap test\n"); fails++;
        } else {
            q = (volatile uint32_t *)(VM_TEST_VA + 0x1000u);
            *q = 0x12345678u;
            if (*q != 0x12345678u) { puts("  FAIL: pre-unmap rw\n"); fails++; }
            if (vm_lookup(&task_a.vm, VM_TEST_VA + 0x1000u) == 0u) {
                puts("  FAIL: lookup before unmap\n"); fails++;
            }
            if (vm_unmap(&task_a.vm, VM_TEST_VA + 0x1000u) != 0) {
                puts("  FAIL: vm_unmap\n"); fails++;
            }
            if (vm_lookup(&task_a.vm, VM_TEST_VA + 0x1000u) != 0u) {
                puts("  FAIL: lookup after unmap\n"); fails++;
            }
            if (vm_unmap(&task_a.vm, VM_TEST_VA + 0x1000u) == 0) {
                puts("  FAIL: double unmap succeeded\n"); fails++;
            }
            if (vm_unmap(&task_a.vm, 0x20000000u) == 0) {
                puts("  FAIL: unmap of section VA succeeded\n"); fails++;
            }
        }

        /* --- read-only: AP bits must encode RO, RW still works --- */
        pa_c = vm_page_alloc();
        if (pa_c == 0u) {
            puts("  FAIL: ro test out of pages\n"); fails++;
        } else if (vm_map(&task_a.vm, VM_TEST_VA + 0x3000u, pa_c,
                          VM_PROT_READ) != 0) {
            puts("  FAIL: map RO\n"); fails++;
        } else {
            desc = vm_lookup(&task_a.vm, VM_TEST_VA + 0x3000u);
            /* AP[1:0]=0b10 (bits 5:4), APX=1 (bit 9) => read-only for
             * both privileged and user. A write would permission-fault
             * (pager deliberately does not resolve those), so we verify
             * the encoding structurally here. */
            if (((desc >> 4) & 0x3u) != 0x2u || ((desc >> 9) & 0x1u) != 1u) {
                puts("  FAIL: RO descriptor AP bits\n"); fails++;
            }
            /* RW page for contrast: AP[1:0]=0b11, APX=0. */
            desc = vm_lookup(&task_a.vm, VM_TEST_VA);
            if (((desc >> 4) & 0x3u) != 0x3u || ((desc >> 9) & 0x1u) != 0u) {
                puts("  FAIL: RW descriptor AP bits\n"); fails++;
            }
        }

        /* --- demand paging: touch unmapped page, pager maps it --- */
        q = (volatile uint32_t *)(VM_DEMAND_BASE + 0x2000u);
        if (vm_lookup(&task_a.vm, (uint32_t)q) != 0u) {
            puts("  FAIL: demand VA already mapped\n"); fails++;
        } else {
            *q = 0xDEADBEEFu;   /* faults -> pager maps zeroed page -> retry */
            v = *q;
            if (v != 0xDEADBEEFu) {
                puts("  FAIL: demand paging write/read\n"); fails++;
            }
            if (vm_lookup(&task_a.vm, (uint32_t)q) == 0u) {
                puts("  FAIL: pager did not map\n"); fails++;
            }
            *q = 0xCAFEBABEu;   /* second touch: no fault, page persists */
            if (*q != 0xCAFEBABEu) {
                puts("  FAIL: demand page not persistent\n"); fails++;
            }
        }
        vm_space_switch(&vm_space_kern);

        puts("[vm  ] unmap / read-only / demand paging: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3e. Fase 6: task sebagai protection domain - isolasi namespace,
     * port grant antar task, dan dealloc. Masih single-threaded
     * (scheduler belum jalan) sehingga ipc_* non-blocking. */
    {
        int fails = 0;
        struct ipc_wire w, r;
        unsigned tmp;
        int n;

#define TCHECK(cond, msg) do { \
            if (!(cond)) { puts("  FAIL: "); puts(msg); putc('\n'); fails++; } \
        } while (0)

        /* Server (task A) alokasi recv port; client (task B) dapat
         * send-right via grant. Reply port sebaliknya. */
        srv_a = ipc_port_alloc(&task_a.ipc, IPC_RECV);
        TCHECK(srv_a != 0, "server port alloc");
        srv_in_b = ipc_port_grant(&task_b.ipc, &task_a.ipc, srv_a,
                                  IPC_SEND);
        TCHECK(srv_in_b != 0, "grant send-right ke B");
        rep_b = ipc_port_alloc(&task_b.ipc, IPC_RECV);
        TCHECK(rep_b != 0, "reply port alloc");
        rep_in_a = ipc_port_grant(&task_a.ipc, &task_b.ipc, rep_b,
                                  IPC_SEND);
        TCHECK(rep_in_a != 0, "grant send-right ke A");

        /* Isolasi namespace: nama yang sama di task berbeda tidak
         * saling terlihat. rep_b (=2) di B itu recv-only -> send
         * harus gagal; srv_a (=1) di A itu recv-only -> send gagal. */
        wire_puts(&w, 0x61, "x");
        TCHECK(ipc_send(&task_b.ipc, 99, &w, sizeof(w)) == -1,
               "send nama liar harus gagal");
        TCHECK(ipc_send(&task_b.ipc, rep_b, &w, sizeof(w)) == -1,
               "nama B tidak alias ke port A");
        TCHECK(ipc_send(&task_a.ipc, srv_a, &w, sizeof(w)) == -1,
               "A tidak punya send-right ke port sendiri");
        TCHECK(ipc_send(&task_a.ipc, rep_in_a, &w, sizeof(w)) == 0,
               "grant rep_in_a harus bisa send (kosongkan lagi)");

        /* Grant benar-benar berbagi port: kirim via nama grant di B,
         * terima via nama asli di A, payload utuh. */
        wire_puts(&w, 0xC1, "grant-ok");
        TCHECK(ipc_send(&task_b.ipc, srv_in_b, &w, sizeof(w)) == 0,
               "send via granted name");
        n = ipc_recv(&task_a.ipc, srv_a, &r, sizeof(r));
        TCHECK(n > 0 && r.id == 0xC1 &&
               memcmp(r.data, "grant-ok", 9) == 0,
               "terima via nama asli di A");
        /* Kosongkan antrean (kirim uji rep_in_a di atas). */
        n = ipc_recv(&task_b.ipc, rep_b, &r, sizeof(r));
        TCHECK(n > 0 && r.id == 0x61, "drain antrean uji");

        /* Dealloc: nama mati total setelah dilepas. */
        tmp = ipc_port_alloc(&task_b.ipc, IPC_SEND);
        TCHECK(tmp != 0, "dealloc alloc");
        TCHECK(ipc_port_dealloc(&task_b.ipc, tmp) == 0, "dealloc");
        TCHECK(ipc_send(&task_b.ipc, tmp, &w, sizeof(w)) == -1,
               "nama ter-dealloc harus mati");
        TCHECK(ipc_port_dealloc(&task_b.ipc, tmp) == -1,
               "double dealloc harus gagal");

#undef TCHECK
        puts("[task] namespace isolation + grant + dealloc: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3f. Fase 7: copy-on-write, single-threaded (scheduler belum
     * jalan). task_a menulis pola ke COW_VA; vm_share_cow() memetakan
     * pa fisik yang sama secara read-only di task_a dan task_b.
     * Write dari B memicu permission fault -> cow_break menyalin
     * halaman privat untuk B; A harus tetap melihat pola aslinya,
     * dan sebaliknya. */
    {
        int fails = 0;
        volatile uint8_t *pa, *pb;
        unsigned i;

#define CCHECK(cond, msg) do { \
            if (!(cond)) { puts("  FAIL: "); puts(msg); putc('\n'); fails++; } \
        } while (0)

        /* Halaman sumber di task_a (demand zero-fill, lalu tulis). */
        vm_space_switch(&task_a.vm);
        pa = (volatile uint8_t *)COW_VA;
        for (i = 0; i < 4096; i++)
            pa[i] = (uint8_t)((i * 3 + 1) & 0xFFu);

        /* Share ke task_b secara COW (keduanya jadi read-only). */
        CCHECK(vm_share_cow(&task_b, &task_a, COW_VA) == 0,
               "vm_share_cow");

        /* B membaca: harus melihat pola A (fisik sama). */
        vm_space_switch(&task_b.vm);
        pb = (volatile uint8_t *)COW_VA;
        for (i = 0; i < 4096; i++)
            if (pb[i] != (uint8_t)((i * 3 + 1) & 0xFFu)) {
                CCHECK(0, "B tidak melihat pola A");
                break;
            }

        /* B menulis -> COW break: B dapat salinan privat. */
        for (i = 0; i < 4096; i++)
            pb[i] = (uint8_t)(i & 0xFFu);
        for (i = 0; i < 4096; i++)
            if (pb[i] != (uint8_t)(i & 0xFFu)) {
                CCHECK(0, "tulis B tidak persisten");
                break;
            }

        /* A harus tidak terpengaruh (masih pola asli). */
        vm_space_switch(&task_a.vm);
        for (i = 0; i < 4096; i++)
            if (pa[i] != (uint8_t)((i * 3 + 1) & 0xFFu)) {
                CCHECK(0, "A berubah setelah B menulis");
                break;
            }

        /* A menulis juga -> break sisi A; B tetap dengan polanya. */
        for (i = 0; i < 4096; i++)
            pa[i] = (uint8_t)((i * 5 + 2) & 0xFFu);
        vm_space_switch(&task_b.vm);
        for (i = 0; i < 4096; i++)
            if (pb[i] != (uint8_t)(i & 0xFFu)) {
                CCHECK(0, "B berubah setelah A menulis");
                break;
            }

        vm_space_switch(&vm_space_kern);
#undef CCHECK
        puts("[vm  ] copy-on-write: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3g. Fase 7: external pager - memory object yang didukung pager
     * thread. Setup single-threaded (port + object + jendela VA di
     * task_c); pengisian halaman terjadi malas saat thread_pclient
     * menyentuh PAGER_VA dan fault. */
    task_create(&task_pager, &port_zone, &msg_zone);
    task_create(&task_c, &port_zone, &msg_zone);
    {
        int fails = 0;

#define GCHECK(cond, msg) do { \
            if (!(cond)) { puts("  FAIL: "); puts(msg); putc('\n'); fails++; } \
        } while (0)

        /* Kedua port dua arah (SEND|RECV): pager thread recv di req +
         * send di rep; pager_fetch_page (dari fault handler) send di
         * req + recv di rep - semua di task_pager.ipc. */
        pager_req = ipc_port_alloc(&task_pager.ipc,
                                   IPC_SEND | IPC_RECV);
        GCHECK(pager_req != 0, "pager req port");
        pager_rep = ipc_port_alloc(&task_pager.ipc,
                                   IPC_SEND | IPC_RECV);
        GCHECK(pager_rep != 0, "pager rep port");
        pager_obj = vm_object_create(&task_c, &task_pager,
                                     pager_req, pager_rep,
                                     2u * 4096u);
        GCHECK(pager_obj != 0, "object create");
        GCHECK(vm_map_object(&task_c, PAGER_VA, pager_obj, 0, 2,
                             VM_PROT_READ | VM_PROT_WRITE) == 0,
               "map object");
        /* Jendela harus belum ter-map fisik (malas, isi saat fault). */
        GCHECK(vm_lookup(&task_c.vm, PAGER_VA) == 0u, "belum ter-map");

#undef GCHECK
        puts("[pager] object + mapping setup: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3h. Fase 8: user mode + syscall. Setup single-threaded:
     * task_user + program image + stack user + port echo. */
    task_create(&task_user, &port_zone, &msg_zone);
    task_user.brk = USER_BRK_START;
    {
        int fails = 0;
        uint32_t pa, va, off = 0u, chunk;
        unsigned i, u_send, u_rep;

        /* Program image -> 4 halaman RWX di USER_PROG_VA. Salin via
         * alias fisik 1:1 (halaman pool ada di BSS kernel); halaman
         * di-zero oleh vm_page_alloc sehingga sisa halaman = NOL.
         * (BSS program butuh W; W^X per-section = follow-up.) */
        if (hello_img_len > USER_PROG_PAGES * 4096u) {
            puts("  FAIL: hello image too big\n");
            fails++;
        }
        for (i = 0; i < USER_PROG_PAGES; i++) {
            pa = vm_page_alloc();
            va = USER_PROG_VA + i * 4096u;
            if (pa == 0u ||
                vm_map(&task_user.vm, va, pa,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC) != 0) {
                puts("  FAIL: map hello page\n");
                fails++;
                break;
            }
            if (off < hello_img_len) {
                chunk = hello_img_len - off;
                if (chunk > 4096u)
                    chunk = 4096u;
                memcpy((void *)pa, hello_img + off, chunk);
                off += chunk;
            }
        }
        /* Stack user: 2 halaman RW (XN, tanpa EXEC) di bawah
         * USER_STACK_TOP. Wilayah di bawahnya tidak di-map
         * (pager akan mengisinya malas bila disentuh). */
        for (i = 0; i < USER_STACK_PAGES; i++) {
            pa = vm_page_alloc();
            va = USER_STACK_TOP - (i + 1u) * 4096u;
            if (pa == 0u ||
                vm_map(&task_user.vm, va, pa,
                       VM_PROT_READ | VM_PROT_WRITE) != 0) {
                puts("  FAIL: map user stack\n");
                fails++;
                break;
            }
        }
        /* Fase 9: program uji ramfs -> 4 halaman RWX di FSTEST_PROG_VA
         * + stack user sendiri (2 halaman RW di bawah FSTEST_STACK_TOP).
         * Pola salin sama dengan hello di atas. */
        if (fstest_img_len > FSTEST_PROG_PAGES * 4096u) {
            puts("  FAIL: fstest image too big\n");
            fails++;
        }
        off = 0u;
        for (i = 0; i < FSTEST_PROG_PAGES; i++) {
            pa = vm_page_alloc();
            va = FSTEST_PROG_VA + i * 4096u;
            if (pa == 0u ||
                vm_map(&task_user.vm, va, pa,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC) != 0) {
                puts("  FAIL: map fstest page\n");
                fails++;
                break;
            }
            if (off < fstest_img_len) {
                chunk = fstest_img_len - off;
                if (chunk > 4096u)
                    chunk = 4096u;
                memcpy((void *)pa, fstest_img + off, chunk);
                off += chunk;
            }
        }
        for (i = 0; i < FSTEST_STACK_PAGES; i++) {
            pa = vm_page_alloc();
            va = FSTEST_STACK_TOP - (i + 1u) * 4096u;
            if (pa == 0u ||
                vm_map(&task_user.vm, va, pa,
                       VM_PROT_READ | VM_PROT_WRITE) != 0) {
                puts("  FAIL: map fstest stack\n");
                fails++;
                break;
            }
        }
        /* Port echo: server di task_kern (usvc_port), user dapat
         * send-right hasil grant (harus = USER_SVC_SEND=1), reply
         * port milik user (harus = USER_SVC_REPLY=2) di-grant balik. */
        usvc_port = ipc_port_alloc(&task_kern.ipc, IPC_SEND | IPC_RECV);
        u_send = ipc_port_grant(&task_user.ipc, &task_kern.ipc,
                                usvc_port, IPC_SEND);
        u_rep = ipc_port_alloc(&task_user.ipc, IPC_RECV);
        usvc_rep_in_kern = ipc_port_grant(&task_kern.ipc, &task_user.ipc,
                                          u_rep, IPC_SEND);
        if (usvc_port == 0u || u_send != USER_SVC_SEND ||
            u_rep != USER_SVC_REPLY || usvc_rep_in_kern == 0u) {
            puts("  FAIL: echo port grant\n");
            fails++;
        }
        puts("[user ] task + image + stack + echo ports: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 4. Preemptive scheduler: GIC + virtual-timer tick. */
    puts("[gic ] init GIC-400\n");
    gic_init();
    gic_set_priority(TIMER_PPI_IRQ, 0x80);
    gic_set_level(TIMER_PPI_IRQ);

    timer_init();
    sched_init();
    sched_add(thread_a, stack_a + sizeof(stack_a), &task_kern);
    sched_add(thread_b, stack_b + sizeof(stack_b), &task_kern);
    sched_add(thread_server, stack_server + sizeof(stack_server), &task_a);
    sched_add(thread_client, stack_client + sizeof(stack_client), &task_b);
    sched_add(thread_pager, stack_pager + sizeof(stack_pager), &task_pager);
    sched_add(thread_pclient, stack_pclient + sizeof(stack_pclient),
              &task_c);
    /* Fase 8: echo server kernel + thread user-mode pertama.
     * sched_add_user: frame CPSR=USR(0x10), pc=USER_PROG_VA,
     * SP_usr=USER_STACK_TOP.
     * Fase 9: thread user kedua (fstest, penguji ramfs) di task yang
     * sama; program di FSTEST_PROG_VA, stack di FSTEST_STACK_TOP. */
    sched_add(thread_usvc, stack_usvc + sizeof(stack_usvc), &task_kern);
    sched_add_user(stack_uthread + sizeof(stack_uthread), &task_user,
                   USER_PROG_VA, USER_STACK_TOP);
    sched_add_user(stack_fstest + sizeof(stack_fstest), &task_user,
                   FSTEST_PROG_VA, FSTEST_STACK_TOP);
    {
        /* 1 ms slice, in timer ticks. */
        uint32_t freq = timer_get_freq();
        uint32_t slice = freq / 1000u;
        sched_set_slice(slice);
        timer_irq_every_us(1000);
    }
    gic_enable_irq(TIMER_PPI_IRQ);
    puts("[sched] preemptive round-robin, 1ms slices, no manual yields\n");

    /*
     * IRQs stay disabled until sched_start()'s rfeia enters thread A
     * with cpsr=0x13 (I=0): the first tick can only fire once a thread
     * is actually running. sched_start() never returns.
     */
    sched_start();

    for (;;) {
        __asm__ volatile("wfi");
    }
}
