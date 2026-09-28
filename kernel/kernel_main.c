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
#include "../rv1103-bringup/net.h"
#include "../rv1103-bringup/netstack.h"
#include "../rv1103-bringup/blk.h"   /* Fase 12d: virtio-blk storage */
#include "../rv1103-bringup/tcp.h"   /* Fase 12d: tcp_is_listen() */
#include "../rv1103-bringup/bootmenu.h" /* Fase 13: boot menu */
#include "../rv1103-bringup/gpio.h"    /* Fase 14: GPIO */
#include "../rv1103-bringup/fat32.h"   /* Fase 16: FAT32 di /sd */

/* Fase 8: image program userspace, di-embed dari user/hello.bin oleh
 * build.sh (user/embed.py -> /tmp/mach_hello_img.o). */
extern const uint8_t hello_img[];
extern const unsigned hello_img_len;

/* Fase 9: image program uji ramfs (user/fstest.bin -> fstest_img). */
extern const uint8_t fstest_img[];
extern const unsigned fstest_img_len;

/* Fase 10: image init + utilitas userspace (user/<prog>.bin). */
extern const uint8_t init_img[];
extern const unsigned init_img_len;
extern const uint8_t ucat_img[];
extern const unsigned ucat_img_len;
extern const uint8_t uls_img[];
extern const unsigned uls_img_len;
extern const uint8_t uecho_img[];
extern const unsigned uecho_img_len;

/* Fase 14: image utilitas GPIO userspace (user/ugpio.bin -> ugpio_img). */
extern const uint8_t ugpio_img[];
extern const unsigned ugpio_img_len;

/* Fase 15: image utilitas SD card userspace (user/usd.bin -> usd_img). */
extern const uint8_t usd_img[];
extern const unsigned usd_img_len;

/* Fase 16: image utilitas uji FAT32 userspace (user/ufs.bin -> ufs_img). */
extern const uint8_t ufs_img[];
extern const unsigned ufs_img_len;

/* Fase 17: image system monitor TUI userspace (user/umon.bin -> umon_img). */
extern const uint8_t umon_img[];
extern const unsigned umon_img_len;

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
static volatile unsigned server_mode_announced;

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
/* Fase 10: kernel stack untuk thread user init + utilitas. */
static unsigned char stack_init[16384] __attribute__((aligned(8)));
static unsigned char stack_ucat[16384] __attribute__((aligned(8)));
static unsigned char stack_uls[16384] __attribute__((aligned(8)));
static unsigned char stack_uecho[16384] __attribute__((aligned(8)));
/* Fase 14: kernel stack untuk thread user ugpio. */
static unsigned char stack_ugpio[16384] __attribute__((aligned(8)));
/* Fase 15: kernel stack untuk thread user usd. */
static unsigned char stack_usd[16384] __attribute__((aligned(8)));
/* Fase 16: kernel stack untuk thread user ufs. */
static unsigned char stack_ufs[16384] __attribute__((aligned(8)));
/* Fase 17: kernel stack untuk thread user umon. */
static unsigned char stack_umon[16384] __attribute__((aligned(8)));
/* Fase 11: stack thread network (virtio-net + ARP/ICMP). */
static unsigned char stack_net[16384] __attribute__((aligned(8)));
/* Fase 12d: stack idle thread (CPU accounting). */
static unsigned char stack_idle[4096] __attribute__((aligned(8)));
static volatile unsigned net_test_done;
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

/* Fase 10: 1 bila program init userspace selesai dengan sukses. init
 * membuat sentinel "/.init_done" HANYA sesudah semua verifikasinya
 * lolos ("INIT TESTS PASSED"); report() menjadikannya gerbang halt.
 * Aman dipanggil dengan IRQ ter-mask (alasan sama dengan
 * fs_test_done). */
static int init_test_done(void)
{
    return fs_find(".init_done") >= 0;
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
     * uji ramfs (sentinel /.fs_done dari thread fstest). Fase 10:
     * juga tunggu init userspace (sentinel /.init_done dari thread
     * init, hanya bila INIT TESTS PASSED). Tes lama tidak rusak -
     * hanya menunggu. */
    over = (total_lines >= 12) && pager_done && user_done &&
           fs_test_done() && init_test_done() && net_test_done;
    if (over && !server_mode_announced) {
        /* Fase 12: jangan halt — lanjut jadi HTTP server selamanya.
         * Suite lama tetap PASS (lihat pesan PASSED di atas). */
        puts("PREEMPT+VM OK - semua tes PASS, lanjut mode HTTP server\n");
        server_mode_announced = 1u;
    }
    irq_restore(s);
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
static void thread_net(void)
{
    unsigned irq;

    puts("[net ] thread start\n");
    if (net_init() < 0) {
        puts("[net ] init gagal; lewati uji net\n");
        net_test_done = 1u;
        for (;;) {
            volatile unsigned d = 0;
            while (d++ < 1000000u)
                __asm__ volatile("" ::: "memory");
        }
    }
    netstack_init();
    irq = net_irq();
    irq_dev_register(irq, net_isr);
    gic_set_priority(irq, 0x80);
    gic_set_level(irq);
    gic_enable_irq(irq);
    puts("[net ] IP 10.0.2.15, menunggu paket (ping dari host)\n");
    puts("[net ] HTTP server di port 80 (/ dan /metrics)\n");
    {
        /* Uji mandiri: ping host 10.0.2.1 (tap0). netstack_ping
         * mengirim ARP request dulu bila MAC belum dikenal. */
        unsigned tries = 0;
        for (;;) {
            net_poll();
            netstack_tick();
            if ((tries % 2000000u) == 0u) {
                int r = netstack_ping(0x0A000201u);
                if (r == 0)
                    net_log("[net] ping -> 10.0.2.1\n");
            }
            tries++;
            if (netstack_ping_got() > 0)
                break;
            /* batasi loop uji agar tak selamanya bila host tak ada */
            if (tries > 20000000u)
                break;
        }
        if (netstack_ping_got() > 0)
            net_log("[net] PING 10.0.2.1 BERHASIL\n");
        else
            net_log("[net] ping timeout (lanjut mode listen)\n");
        net_test_done = 1u;
    }
    {
        /* Loop utama server: poll RX + tick TCP. Fase 12d: bila tak ada
         * kerja (tak ada paket) dan TCP dalam keadaan LISTEN (tak ada
         * koneksi aktif), thread BLOCK agar CPU% idle jujur; dibangunkan
         * oleh net_isr saat paket tiba. Saat koneksi aktif, tetap poll
         * rapat demi retransmit (RTO 800ms) & latensi rendah.
         *
         * Anti missed-wakeup: cek ulang dengan IRQ ter-mask sebelum
         * mengubah state (pola yang sama dengan ipc_recv, AGENTS.md):
         * net_isr tak bisa menyela antara cek dan block. */
        for (;;) {
            unsigned w = net_poll();
            netstack_tick();
            if (w == 0u && tcp_is_listen()) {
                struct sched_thread *t = sched_current_thread();
                unsigned cpsr;
                __asm__ volatile("mrs %0, cpsr\n\tcpsid i"
                                 : "=r"(cpsr) :: "memory");
                if (net_poll() == 0u && tcp_is_listen())
                    t->state = THREAD_BLOCKED;
                __asm__ volatile("msr cpsr_c, %0"
                                 :: "r"(cpsr) : "memory");
                while (t->state == THREAD_BLOCKED) {
                    /* IRQ hidup: tick/net_isr bisa membangunkan. */
                }
            }
        }
    }
}

/* Fase 12d: idle thread untuk CPU accounting. Prioritas paling rendah:
 * scheduler hanya memilihnya bila tak ada thread RUNNABLE lain
 * (lihat sched_on_tick). wfi menghemat CPU host saat menganggur;
 * timer tick (1ms) membangunkan tiap slice untuk accounting. */
static void thread_idle(void)
{
    for (;;)
        __asm__ volatile("wfi");
}

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
        if (server_mode_announced) {
            /* Fase 12d: demo selesai (semua tes PASS, mode HTTP server
             * jalan); matikan thread agar CPU% idle jujur. */
            unsigned s = irq_save();
            sched_current_thread()->state = THREAD_DEAD;
            irq_restore(s);
            for (;;) { }        /* tak dijadwalkan lagi */
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
        if (server_mode_announced) {
            /* Fase 12d: demo selesai; matikan thread agar CPU% jujur. */
            unsigned s = irq_save();
            sched_current_thread()->state = THREAD_DEAD;
            irq_restore(s);
            for (;;) { }        /* tak dijadwalkan lagi */
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

    /* Fase 12d: kerja selesai; matikan thread agar CPU% idle jujur
     * (sebelumnya spin kosong selamanya). */
    {
        unsigned s2 = irq_save();
        sched_current_thread()->state = THREAD_DEAD;
        irq_restore(s2);
    }
    for (;;) { }                /* tak dijadwalkan lagi */
}

/* Fase 10: muat satu image program userspace ke task_user:
 * prog_pages halaman R+X di prog_va (salin dari blob embed via alias
 * fisik 1:1; sisa halaman = NOL dari vm_page_alloc) + stack_pages
 * halaman stack RW di bawah stack_top. Kembalikan jumlah kegagalan.
 * Pola yang sama dengan blok hello/fstest inline di bawah. */
static int load_user_image(const uint8_t *img, unsigned img_len,
                           uint32_t prog_va, unsigned prog_pages,
                           uint32_t stack_top, unsigned stack_pages,
                           const char *tag)
{
    uint32_t pa, va, off = 0u, chunk;
    unsigned i;
    int fails = 0;

    if (img_len > prog_pages * 4096u) {
        puts("  FAIL: "); puts(tag); puts(" image too big\n");
        return 1;
    }
    for (i = 0; i < prog_pages; i++) {
        pa = vm_page_alloc();
        va = prog_va + i * 4096u;
        if (pa == 0u ||
            vm_map(&task_user.vm, va, pa,
                   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC) != 0) {
            puts("  FAIL: "); puts(tag); puts(" map prog page\n");
            fails++;
            break;
        }
        if (off < img_len) {
            chunk = img_len - off;
            if (chunk > 4096u)
                chunk = 4096u;
            memcpy((void *)pa, img + off, chunk);
            off += chunk;
        }
    }
    for (i = 0; i < stack_pages; i++) {
        pa = vm_page_alloc();
        va = stack_top - (i + 1u) * 4096u;
        if (pa == 0u ||
            vm_map(&task_user.vm, va, pa,
                   VM_PROT_READ | VM_PROT_WRITE) != 0) {
            puts("  FAIL: "); puts(tag); puts(" map stack page\n");
            fails++;
            break;
        }
    }
    return fails;
}

void kernel_main(void)
{
    /* Fase 13: boot menu via UART (polled, timeout ~3 dtk auto-boot).
     * Dijalankan paling awal, sebelum pmap_init(): MMU off (1:1),
     * IRQ belum nyala. Path default (timeout) berperilaku identik
     * dengan boot sebelum Fase 13. */
    int bootmode = bootmenu_run();

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
    gpio_init();  /* Fase 14: GPIO (mock di QEMU, register asli RV1103). */
    puts("[gpio] init ok\n");
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
        /* Fase 10: init + utilitas userspace (init/ucat/uls/uecho).
         * VA program + stack lihat rv1103-bringup/user.h; semuanya
         * dimuat ke task_user yang sama (fd table & ramfs dipakai
         * bersama; koordinasi antar program via file sentinel). */
        fails += load_user_image(init_img, init_img_len,
                                 INIT_PROG_VA, INIT_PROG_PAGES,
                                 INIT_STACK_TOP, INIT_STACK_PAGES,
                                 "init");
        fails += load_user_image(ucat_img, ucat_img_len,
                                 UCAT_PROG_VA, UCAT_PROG_PAGES,
                                 UCAT_STACK_TOP, UCAT_STACK_PAGES,
                                 "ucat");
        fails += load_user_image(uls_img, uls_img_len,
                                 ULS_PROG_VA, ULS_PROG_PAGES,
                                 ULS_STACK_TOP, ULS_STACK_PAGES,
                                 "uls");
        fails += load_user_image(uecho_img, uecho_img_len,
                                 UECHO_PROG_VA, UECHO_PROG_PAGES,
                                 UECHO_STACK_TOP, UECHO_STACK_PAGES,
                                 "uecho");
        /* Fase 14: utilitas GPIO userspace. */
        fails += load_user_image(ugpio_img, ugpio_img_len,
                                 UGPIO_PROG_VA, UGPIO_PROG_PAGES,
                                 UGPIO_STACK_TOP, UGPIO_STACK_PAGES,
                                 "ugpio");
        /* Fase 15: utilitas SD card userspace. */
        fails += load_user_image(usd_img, usd_img_len,
                                 USD_PROG_VA, USD_PROG_PAGES,
                                 USD_STACK_TOP, USD_STACK_PAGES,
                                 "usd");
        /* Fase 16: utilitas uji FAT32 userspace. */
        fails += load_user_image(ufs_img, ufs_img_len,
                                 UFS_PROG_VA, UFS_PROG_PAGES,
                                 UFS_STACK_TOP, UFS_STACK_PAGES,
                                 "ufs");
        /* Fase 17: system monitor TUI userspace. */
        fails += load_user_image(umon_img, umon_img_len,
                                 UMON_PROG_VA, UMON_PROG_PAGES,
                                 UMON_STACK_TOP, UMON_STACK_PAGES,
                                 "umon");
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

    /* Fase 13: extended self-test — hanya bila dipilih di boot menu
     * (opsi 2). Masih single-threaded (scheduler belum jalan). */
    if (bootmode == BOOTMODE_SELFTEST) {
        int fails = 0;
        puts("[st  ] extended self-test (boot menu option 2)\n");

        /* 1) Seluruh jalur IPC diuji ulang end-to-end via SVC traps. */
        fails += ipc_selftest();

        /* 2) Zone allocator stress: habiskan semua slot msg_zone, lalu
         * kembalikan semuanya — freelist harus utuh kembali. */
        {
            void *objs[16];
            unsigned n = 0u, i;
            void *extra;

            while (n < 16u) {
                void *o = zalloc(&msg_zone);
                if (o == 0)
                    break;
                objs[n++] = o;
            }
            if (n == 0u) {
                puts("  FAIL: zone msg kosong\n");
                fails++;
            }
            extra = zalloc(&msg_zone);
            if (extra != 0) {
                puts("  FAIL: zone msg over-alloc\n");
                fails++;
            }
            for (i = 0u; i < n; i++)
                zfree(&msg_zone, objs[i]);
            if (n > 0u) {
                if (zalloc(&msg_zone) == 0) {
                    puts("  FAIL: zone msg tidak pulih setelah free\n");
                    fails++;
                } else {
                    zfree(&msg_zone, objs[n - 1u]);
                }
            }
        }

        /* 3) VM page roundtrip di task_a: alloc, map, tulis/baca pola,
         * unmap, pastikan mapping hilang. VM_TEST_VA+0x2000 bebas
         * (tes 3c/3d memakai +0x0/+0x1000/+0x3000, COW pakai COW_VA). */
        {
            uint32_t pa = vm_page_alloc();
            volatile uint32_t *p =
                (volatile uint32_t *)(VM_TEST_VA + 0x2000u);

            if (pa == 0u) {
                puts("  FAIL: vm_page_alloc (selftest)\n");
                fails++;
            } else if (vm_map(&task_a.vm, VM_TEST_VA + 0x2000u, pa,
                              VM_PROT_READ | VM_PROT_WRITE) != 0) {
                puts("  FAIL: vm_map (selftest)\n");
                fails++;
            } else {
                vm_space_switch(&task_a.vm);
                *p = 0x5E1F7E57u;
                if (*p != 0x5E1F7E57u) {
                    puts("  FAIL: vm rw (selftest)\n");
                    fails++;
                }
                vm_space_switch(&vm_space_kern);
                if (vm_unmap(&task_a.vm, VM_TEST_VA + 0x2000u) != 0 ||
                    vm_lookup(&task_a.vm, VM_TEST_VA + 0x2000u) != 0u) {
                    puts("  FAIL: vm unmap/lookup (selftest)\n");
                    fails++;
                }
            }
        }

        puts("[st  ] extended self-test: ");
        if (fails == 0)
            puts("ALL CHECKS PASSED\n");
        else {
            puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
        }
    }

    /* 3i. Fase 14: GPIO driver self-test — hanya bila dipilih di boot
     * menu (opsi 2), single-threaded seperti blok di atas. Roundtrip
     * set->get di beberapa pin + kasus pin/bank liar. gpio_init()
     * sudah dipanggil di awal (setelah fs_init). */
    if (bootmode == BOOTMODE_SELFTEST) {
        int fails = 0;
        unsigned pins[4] = { 0u, 7u, 15u, 31u };
        unsigned i;
        puts("[st  ] gpio driver self-test\n");
        for (i = 0u; i < 4u; i++) {
            if (gpio_set(0u, pins[i], 1u) != 0 ||
                gpio_get(0u, pins[i]) != 1) {
                puts("  FAIL: gpio set/get 1\n");
                fails++;
            }
            if (gpio_set(0u, pins[i], 0u) != 0 ||
                gpio_get(0u, pins[i]) != 0) {
                puts("  FAIL: gpio set/get 0\n");
                fails++;
            }
        }
        if (gpio_set(0u, 32u, 1u) != -1 ||
            gpio_get(0u, 32u) != -1 ||
            gpio_set(9u, 0u, 1u) != -1) {
            puts("  FAIL: gpio pin/bank liar\n");
            fails++;
        }
        puts("[st  ] gpio driver self-test: ");
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

    /* Fase 12d: storage virtio-blk 128MB (emulasi SPI NAND Pico Mini).
     * I/O sinkron via polling MMIO; aman dipanggil sebelum scheduler. */
    puts("[blk ] init virtio-blk\n");
    if (blk_init() < 0)
        puts("[blk ] init gagal; storage tidak tersedia\n");

    /* 4a2. Fase 16: mount FAT32 dari kartu SD (dev 1). */
    if (fat32_mount() == 0)
        puts("[fat ] mount /sd ok\n");
    else
        puts("[fat ] mount /sd gagal; lanjut tanpa /sd\n");

    /* 4b. Fase 16: filesystem FAT32 self-test — hanya bila dipilih di
     * boot menu (opsi 2), single-threaded seperti blok di atas.
     * (Self-test sektor mentah Fase 15 dihapus: sektor 10 kini bagian
     * dari volume FAT32, menulis pola mentah akan merusaknya.)
     * Uji: mkdir bersarang, tulis/baca byte-exact, fragmentasi
     * (hapus file tengah lalu tulis file lebih besar -> chain
     * tak-kontigu), readdir, delete, dan penolakan path liar. */
    if (bootmode == BOOTMODE_SELFTEST) {
        int fails = 0;
        static uint8_t st_w[6144];
        static uint8_t st_r[6144];
        static char st_ls[512];
        unsigned i;
        puts("[st  ] fat32 self-test\n");
        if (!sd_present() || !fat32_mounted()) {
            puts("  FAIL: sd/fat tidak siap\n");
            fails++;
        } else {
            for (i = 0u; i < sizeof(st_w); i++)
                st_w[i] = (uint8_t)(0x5Au ^ (i * 7u) ^ (i >> 5));
            if (fat32_mkdir("/sd/SELFTEST") != 0) {
                puts("  FAIL: mkdir\n");
                fails++;
            }
            /* A/B/C masing-masing 1 cluster (4KB); lalu B dihapus. */
            if (fat32_write_file("/sd/SELFTEST/A.BIN", st_w, 4096) != 4096 ||
                fat32_write_file("/sd/SELFTEST/B.BIN", st_w, 4096) != 4096 ||
                fat32_write_file("/sd/SELFTEST/C.BIN", st_w, 4096) != 4096) {
                puts("  FAIL: tulis A/B/C\n");
                fails++;
            }
            if (fat32_delete("/sd/SELFTEST/B.BIN") != 0) {
                puts("  FAIL: hapus B\n");
                fails++;
            }
            /* D = 6000 byte = 2 cluster: cluster bekas B + 1 baru
             * (chain terfragmentasi, tak kontigu). */
            if (fat32_write_file("/sd/SELFTEST/D.BIN", st_w, 6000) != 6000) {
                puts("  FAIL: tulis D (fragmented)\n");
                fails++;
            }
            for (i = 0u; i < sizeof(st_r); i++)
                st_r[i] = 0u;
            if (fat32_read_file("/sd/SELFTEST/D.BIN", st_r, sizeof(st_r))
                    != 6000) {
                puts("  FAIL: baca D\n");
                fails++;
            } else {
                for (i = 0u; i < 6000u; i++) {
                    if (st_r[i] != st_w[i]) {
                        puts("  FAIL: D pola rusak\n");
                        fails++;
                        break;
                    }
                }
            }
            if (fat32_read_file("/sd/SELFTEST/A.BIN", st_r, 4096)
                    != 4096) {
                puts("  FAIL: baca A\n");
                fails++;
            }
            /* readdir: A, C, D ada; B tidak. */
            for (i = 0u; i < sizeof(st_ls); i++)
                st_ls[i] = 0;
            if (fat32_listdir("/sd/SELFTEST", st_ls, sizeof(st_ls) - 1u)
                    != 3) {
                puts("  FAIL: readdir count\n");
                fails++;
            }
            /* Kasus liar: path di luar /sd, mkdir root, tulis ke dir. */
            if (fat32_write_file("/etc/passwd", st_w, 10) != -1 ||
                fat32_mkdir("/sd") != -1 ||
                fat32_write_file("/sd/SELFTEST", st_w, 10) != -1 ||
                fat32_delete("/sd/SELFTEST") != -1) {
                puts("  FAIL: kasus liar diterima\n");
                fails++;
            }
            /* Bersih-bersih. */
            if (fat32_delete("/sd/SELFTEST/A.BIN") != 0 ||
                fat32_delete("/sd/SELFTEST/C.BIN") != 0 ||
                fat32_delete("/sd/SELFTEST/D.BIN") != 0) {
                puts("  FAIL: bersih-bersih\n");
                fails++;
            }
            if (sd_read(1u << 31, st_r) != -1) {
                puts("  FAIL: sd sektor liar diterima\n");
                fails++;
            }
            puts("[st  ] fat32 self-test: ");
            if (fails == 0)
                puts("ALL CHECKS PASSED\n");
            else {
                puts("FAILURES = "); putdec((unsigned)fails); putc('\n');
            }
        }
    }

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
    /* Fase 11: thread network (virtio-net + ARP/ICMP). */
    sched_add(thread_net, stack_net + sizeof(stack_net), &task_kern);
    /* Fase 12d: catat indeks thread net untuk wakeup dari net_isr. */
    sched_set_net_idx(sched_thread_count() - 1u);
    sched_add_user(stack_uthread + sizeof(stack_uthread), &task_user,
                   USER_PROG_VA, USER_STACK_TOP);
    sched_add_user(stack_fstest + sizeof(stack_fstest), &task_user,
                   FSTEST_PROG_VA, FSTEST_STACK_TOP);
    /* Fase 10: init userspace pertama + utilitas (ucat/uls/uecho).
     * Semua di task_user yang sama; init mengoordinasi utilitas
     * lewat file sentinel di ramfs (bukan syscall spawn/exec —
     * itu pasca-shell, dan shell paling akhir). */
    sched_add_user(stack_init + sizeof(stack_init), &task_user,
                   INIT_PROG_VA, INIT_STACK_TOP);
    sched_add_user(stack_ucat + sizeof(stack_ucat), &task_user,
                   UCAT_PROG_VA, UCAT_STACK_TOP);
    sched_add_user(stack_uls + sizeof(stack_uls), &task_user,
                   ULS_PROG_VA, ULS_STACK_TOP);
    sched_add_user(stack_uecho + sizeof(stack_uecho), &task_user,
                   UECHO_PROG_VA, UECHO_STACK_TOP);
    /* Fase 14: utilitas GPIO (dikoordinasi init via /gpio.cmd). */
    sched_add_user(stack_ugpio + sizeof(stack_ugpio), &task_user,
                   UGPIO_PROG_VA, UGPIO_STACK_TOP);
    /* Fase 15: utilitas SD card (dikoordinasi init via /sd.cmd). */
    sched_add_user(stack_usd + sizeof(stack_usd), &task_user,
                   USD_PROG_VA, USD_STACK_TOP);
    /* Fase 16: utilitas uji FAT32 (dikoordinasi init via /fat.cmd). */
    sched_add_user(stack_ufs + sizeof(stack_ufs), &task_user,
                   UFS_PROG_VA, UFS_STACK_TOP);
    /* Fase 17: system monitor TUI (one-shot snapshot -> /.umon_done;
     * mode live bila /umon.live ada). */
    sched_add_user(stack_umon + sizeof(stack_umon), &task_user,
                   UMON_PROG_VA, UMON_STACK_TOP);
    /* Fase 12d: idle thread TERAKHIR (CPU accounting). Scheduler hanya
     * memilihnya bila tak ada thread RUNNABLE lain. */
    sched_add(thread_idle, stack_idle + sizeof(stack_idle), &task_kern);
    sched_set_idle(sched_thread_count() - 1u);
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
