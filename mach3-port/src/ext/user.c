/*
 * mach3-port/src/ext/user.c -- M4 item 4: user mode + syscall interface.
 *
 * user_selftest(): called from startrtclock() (after task_selftest).
 *   Creates a user task (task_create, private pmap via task->map->pmap),
 *   maps a code page + stack page with user permissions, switches TTBR0
 *   to the user L1, and enters USR mode via user_enter_test().
 *
 *   Phase A (clean): embedded program does SYS_WRITE then SYS_EXIT(0).
 *   Phase B (fault): embedded program does SYS_WRITE then touches 0x0;
 *     the data-abort handler kills just the user context (redirect to
 *     user_exit_trampoline) instead of panicking the kernel.
 *
 * Syscall ABI (ARM EABI-like): number in r7, args in r0-r2, return in
 * r0; svc #0.
 */
#include <mach/machine/vm_types.h>
#include <mach/kern_return.h>
#include <mach/vm_prot.h>
#include <machine/trap_frame.h>
#include <machine/pmap.h>
#include <machine/machspl.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include "ramfs.h"
#include "ipc_glue.h"

extern void	panic(const char *, ...);
extern int	printf(const char *, ...);
extern int	lfs_run_test(void);	/* lfs_port.c (B1) */
extern int	lfs_port_mount(void);
extern int	lfs_port_list(char *, unsigned int);
extern int	lfs_port_read(const char *, unsigned char *, unsigned int);
extern int	lfs_port_write(const char *, const unsigned char *,
		    unsigned int);
/* ONB-1 (webui.c): web shell bridge + device PIN. */
extern int	webui_shell_take(char *, unsigned int);
extern int	webui_shell_post(const char *, unsigned int);
extern void	webui_shell_executor_died(void);	/* FND-2 */
extern int	webui_pin_set(const char *, unsigned int);
extern int	webui_pin_present(void);
extern int	webui_pin_verify(const char *, unsigned int);
extern int	credstore_set(const char *, const unsigned char *,
		    unsigned int);
extern int	credstore_get(const char *, unsigned char *,
		    unsigned int);
extern int	credstore_present(const char *);
extern int	credstore_clear(const char *);
extern int	credstore_selftest(void);
extern int	bootstate_init(void);	/* bootstate.c (B2) */
extern int	bootstate_safe_mode(void);
extern void	bootstate_healthy(unsigned int);
extern void	bootstate_note_software_reset(unsigned int, int);
extern unsigned int psci_system_reset(void);	/* psci.c (FND-1) */
extern unsigned int psci_system_off(void);	/* psci.c (FND-1) */
extern void	bootstate_crash_sim(void);
extern void	bootstate_info(unsigned int *, unsigned int *,
		    unsigned int *, unsigned int *);
extern int	bootstate_log_get(unsigned int, void *);
extern unsigned int bootstate_flags(void);
extern void	bootstate_set_provisioned(int);
extern void	bootstate_set_gui_mode(unsigned int);
extern unsigned int bootstate_gui_mode(void);
extern int	bootstate_update_stage(unsigned int);
extern void	bootstate_update_info(unsigned int *, unsigned int *,
		    unsigned int *, unsigned int *);
extern void	bootstate_sabotage_arm(void);
extern int	bootstate_sabotage_take(void);
extern int	bootstate_update_cancel(void);
extern int	upd_verify_package(const unsigned char *,
		    unsigned int, unsigned int *);
extern unsigned int bootstate_log_count(void);
extern void	bootstate_log_event(unsigned int, unsigned int,
		    const char *);	/* bootstate.c (FND-3) */
extern void	bootstate_note_watchdog(unsigned int,
		    const char *);	/* bootstate.c (FND-3) */
extern unsigned int arm_timer_ticks(void);
extern void	uart_putc(char c);

/* userasm.s */
extern unsigned int	user_enter_test(unsigned int user_sp,
					  unsigned int user_pc);
extern void		user_exit_trampoline(void);

/* uprog.s: embedded position-independent user programs */
extern unsigned char	_uprog_start[], _uprog_end[];
extern unsigned char	_uprog_fault[];

/* Embedded user programs (Fase B/C, generated at build). */
extern unsigned char	init_img[];
extern unsigned int	init_img_len;
extern unsigned char	ucat_img[];
extern unsigned int	ucat_img_len;
extern unsigned char	uls_img[];
extern unsigned int	uls_img_len;
extern unsigned char	uecho_img[];
extern unsigned int	uecho_img_len;
extern unsigned char	umon_img[];
extern unsigned int	umon_img_len;
extern unsigned char	ugpio_img[];
extern unsigned int	ugpio_img_len;
extern unsigned char	usd_img[];
extern unsigned int	usd_img_len;
extern unsigned char	ufs_img[];
extern unsigned int	ufs_img_len;
extern unsigned char	utimeset_img[];	/* F2: regression test TIME_SET/GET */
extern unsigned int	utimeset_img_len;
extern unsigned char	uaudiotest_img[];	/* D1: ACODEC init + readback */
extern unsigned int	uaudiotest_img_len;
extern unsigned char	uaudiocap_img[];	/* D2: I2S RX capture */
extern unsigned int	uaudiocap_img_len;
extern unsigned char	uaudiogain_img[];	/* D3: gain + overrun */
extern unsigned int	uaudiogain_img_len;
extern unsigned char	uvoice_img[];	/* D4: VOICE.START + AUD uplink */
extern unsigned int	uvoice_img_len;
#ifdef QA_TIER_A
extern unsigned char	ulinktest_img[];	/* C6L-1: link layer + wire freeze */
extern unsigned int	ulinktest_img_len;
extern unsigned char	ulinkbr_img[];	/* C6L-2: bridge test */
extern unsigned char	ulink3_img[];	/* C6L-3: v2 completion test */
extern unsigned int	ulink3_img_len;
extern unsigned char	ulinkaud_img[];	/* C6L-4: AUD integrity test */
extern unsigned int	ulinkaud_img_len;
extern unsigned int	ulinkbr_img_len;
extern unsigned char	ulink5_img[];	/* C6L-5: linkd client test */
extern unsigned int	ulink5_img_len;
extern unsigned char	ulinktools_img[];	/* C6L-5: stub tools server */
extern unsigned int	ulinktools_img_len;
extern unsigned char	ulink5die_img[];	/* C6L-5: client-death probe */
extern unsigned int	ulink5die_img_len;
#endif /* QA_TIER_A */
extern unsigned char	upl330_img[];	/* D2b: PL330 microcode */
extern unsigned int	upl330_img_len;
extern unsigned char	ulfs_img[];	/* B1: littlefs test */
extern unsigned int	ulfs_img_len;
extern unsigned char	ublogtest_img[];	/* B2: bootlog diagnostic */
extern unsigned int	ublogtest_img_len;
extern unsigned char	ufirst_img[];	/* B3: first-boot */
extern unsigned int	ufirst_img_len;
extern unsigned char	utimegate_img[];	/* B3: time quality test */
extern unsigned int	utimegate_img_len;
extern unsigned char	uupdsig_img[];	/* B5.3: signed update test */
extern unsigned int	uupdsig_img_len;
extern unsigned char	ucred_img[];	/* B5.5: credential store test */
extern unsigned int	ucred_img_len;
extern unsigned char	utcpkeep_img[];	/* F1 SEMENTARA: keep-alive RX test */
extern unsigned int	utcpkeep_img_len;
extern unsigned char	face_img[];	/* App A1/A2: Qabot server */
extern unsigned int	face_img_len;
extern unsigned char	sh_img[];	/* Q9: shell qaon> */
extern unsigned int	sh_img_len;
extern unsigned char	uiapp_img[];	/* App A2: menu/settings/monitor */
extern unsigned int	uiapp_img_len;
extern unsigned char	ntp_img[];	/* App A4: sinkron jam via NTP */
extern unsigned int	ntp_img_len;
extern unsigned char	utcpcli_img[];	/* Q2a SEMENTARA: uji TCP client */
extern unsigned int	utcpcli_img_len;
extern unsigned char	qabotd_img[];	/* Q4: daemon persisten Qabot */
extern unsigned int	qabotd_img_len;
extern unsigned char	uipcld_img[];	/* IPC-4: concurrent client daemon (test-only) */
extern unsigned int	uipcld_img_len;
extern unsigned char	linkd_img[];	/* C6L-5: link daemon (owns the transport) */
extern unsigned int	linkd_img_len;
extern unsigned char	ulink5d_img[];	/* C6L-5: second client daemon (test-only) */
extern unsigned int	ulink5d_img_len;
extern unsigned char	qabot_img[];	/* Qabot harness Q1: ReAct + mock */
extern unsigned int	qabot_img_len;

/* trap.c (Fase B) */
extern unsigned int	arm_timer_ticks(void);

/* netstack.c (App A4: UDP untuk DNS/NTP) */
extern int	netstack_udp_send(unsigned int dst, unsigned short dport,
				      const unsigned char *data, unsigned dlen);
extern int	netstack_udp_recv(unsigned char *buf, unsigned maxlen,
				      unsigned int *src_ip,
				      unsigned short *src_port);

/* tcp.c (Q2a: TCP client, satu koneksi aktif). */
extern int	tcp_client_connect(unsigned int dst_ip, unsigned short dst_port);
extern int	acodec_init(void);
extern unsigned int	acodec_reg_read(unsigned int off);
extern int	audio_start(unsigned int rate, unsigned int bits,
			    unsigned int ch);
extern int	audio_read(unsigned char *dst, unsigned int len);
extern void	audio_stop(void);
extern int	audio_set_mic_gain(unsigned int level);
extern unsigned int	audio_overrun_count(void);
extern int	pl330_init(void);
extern int	pl330_audio_rx(unsigned int dst, unsigned int nwords);
extern unsigned int	pl330_ucode_len(void);
extern unsigned int	pl330_ucode_byte(unsigned int idx);
extern unsigned char	pl330_ucode_buf[64];
extern unsigned int	pl330_ucode_buflen;
extern int	tcp_client_status(void);
extern int	tcp_client_send(const unsigned char *p, unsigned len);
extern int	tcp_client_recv(unsigned char *buf, unsigned maxlen);
extern void	tcp_client_close(void);

/* clock.c (Fase B) */
extern void	arm_timer_enable(void);

/* uart.c: non-blocking console read (Fase C, SYS_READ_CONSOLE). */
extern int	cnmaygetc(void);

/* blk.c: kapasitas storage real (Fase C, SYS_STAT). */
extern unsigned int	blk_total_sectors(void);

/* MI vm_page.c: jumlah halaman bebas saat ini (Fase C, SYS_STAT). */
extern int	vm_page_free_count;

/* arm_init.c: total RAM (Fase C, SYS_STAT). */
extern vm_offset_t	mem_size;

/* gpio.c (Fase D, SYS_GPIO_SET/GET). Bank di-fix 0 (seperti kernel lama). */
extern int	gpio_set(unsigned int bank, unsigned int pin,
			 unsigned int val);
extern int	gpio_get(unsigned int bank, unsigned int pin);

/* blk.c (Fase D, SYS_SD_READ/WRITE). */
extern int	sd_present(void);
extern int	sd_read(unsigned int sector, unsigned char *data);
extern int	sd_write(unsigned int sector, const unsigned char *data);

/* fat32.c (Fase D, SYS_MKDIR/FAT_WRITE/FAT_READ/FAT_DELETE/READDIR). */
extern int	fat32_mounted(void);
extern int	fat32_mount(void);
extern int	fat32_mkdir(const char *path);
extern int	fat32_write_file(const char *path, const unsigned char *data,
				 unsigned int len);
extern int	fat32_read_file(const char *path, unsigned char *dst,
				unsigned int max);
extern int	fat32_delete(const char *path);
extern int	fat32_listdir(const char *path, char *dst, unsigned int max);

extern void	gpu_splash(unsigned int pct, unsigned int stage);
/* gpu.c (App A1, SYS_DISPLAY_INFO/FLUSH). */
extern int	gpu_available_p(void);
extern int	gpu_flush_strip(unsigned int x, unsigned int y,
				unsigned int w, unsigned int h,
				const unsigned short *rgb565);

/* Info display untuk SYS_DISPLAY_INFO (12 byte). */
struct qaon_display {
	unsigned int	width;
	unsigned int	height;
	unsigned int	format;	/* 0 = input RGB565 (strip parsial) */
};
#define	QAON_DISP_W	240u
#define	QAON_DISP_H	240u

/* pmap.c */
extern void	arm_pmap_activate_user(pmap_t pmap);
extern void	arm_pmap_activate_kernel(void);

#define	USER_CODE_VA	0x100000u
#define	USER_STACK_VA	0x101000u
#define	USER_PGBYTES	4096u

/* Fase B init task layout (separate user task from the selftest above). */
#define	INIT_CODE_VA	0x100000u	/* init image (npages, R/W) */
/* Q2b: stack & heap VA kini DINAMIS per program (setup_uprog_task):
 * stack tepat setelah code (stack_pages), heap tepat setelah stack.
 * INIT_STACK_VA/INIT_HEAP_VA di bawah hanya untuk user_selftest. */
#define	INIT_STACK_VA	0x110000u	/* selftest: 1 page */
#define	INIT_HEAP_VA	0x120000u	/* selftest: sbrk region */
#define	INIT_HEAP_PAGES	16u		/* 64 KB pre-allocated heap */
#define	USER_VA_BASE	0x100000u	/* lowest valid user VA */
#define	USER_VA_TOP	0x130000u	/* highest valid user VA */

/*
 * Syscalls (ABI Fase B/C — kontinu dengan kernel lama;
 * lihat docs/SYSCALL-ABI.md).
 */
#define	SYS_WRITE	20u	/* write(fd, buf, len) -> len or -1 */
#define	SYS_YIELD	21u	/* yield remainder of slice -> 0 */
#define	SYS_EXIT	22u	/* exit(code) -> never returns */
#define	SYS_SBRK	24u	/* sbrk(incr) -> old brk or -1 */
#define	SYS_OPEN	30u	/* open(path, flags) -> fd or -1 */
#define	SYS_READ	31u	/* read(fd, buf, len) -> bytes or -1 */
#define	SYS_CLOSE	32u	/* close(fd) -> 0 or -1 */
#define	SYS_LS		33u	/* ls(buf, max) -> file count or -1 */
#define	SYS_DELETE	34u	/* delete(path) -> 0 or -1 */
#define	SYS_STAT	57u	/* stat(buf, len) -> 0 or -1 */
#define	SYS_TLIST	58u	/* tlist(buf, max) -> entries or -1 */
#define	SYS_READ_CONSOLE 59u	/* read_console() -> byte or -1 */
#define	SYS_GPIO_SET	40u	/* gpio_set(pin, val) -> 0 or -1 (Fase D) */
#define	SYS_GPIO_GET	41u	/* gpio_get(pin) -> 0/1 or -1 (Fase D) */
#define	SYS_SD_READ	50u	/* sd_read(sector, buf512) -> 0 or -1 (Fase D) */
#define	SYS_SD_WRITE	51u	/* sd_write(sector, buf512) -> 0 or -1 (Fase D) */
#define	SYS_MKDIR	52u	/* mkdir(path) -> 0 or -1 (Fase D) */
#define	SYS_FAT_WRITE	53u	/* fat_write(path, buf, len) -> bytes/-1 (Fase D) */
#define	SYS_FAT_READ	54u	/* fat_read(path, buf, max) -> bytes/-1 (Fase D) */
#define	SYS_FAT_DELETE	55u	/* fat_delete(path) -> 0 or -1 (Fase D) */
#define	SYS_READDIR	56u	/* readdir(path, buf, max) -> count/-1 (Fase D) */
#define	SYS_DISPLAY_INFO 60u	/* display_info(buf, len) -> 0/-1 (App A1) */
#define	SYS_DISPLAY_FLUSH 61u	/* display_flush(x,y,w,h,buf,len) -> 0/-1 */
/* App A2: protokol token display + input (arbiter di kernel). */
#define	SYS_DISPLAY_GRANT 62u	/* face->kernel: beri token ke uiapp -> 0/-1 */
#define	SYS_DISPLAY_ACQUIRE 63u	/* -> 1 bila pemegang token, else 0 */
#define	SYS_DISPLAY_RELEASE 64u	/* pemegang kembalikan token -> 0/-1 */
#define	SYS_DISPLAY_GET_EVENT 65u	/* -> EV_* atau -1 (kosong/bukan pemegang) */
#define	SYS_DISPLAY_STATUS 66u	/* -> id pemegang token (0=face,1=uiapp) */
#define	SYS_UPTIME	67u	/* -> milidetik sejak boot */
#define	SYS_DISPLAY_SLEEP 68u	/* r0=1: uiapp minta sleep; r0=0: face ambil+clear */
#define	SYS_TIME_SET	69u	/* App A4: time_set(unix_sec) -> 0 */
#define	SYS_TIME_GET	70u	/* App A4: -> detik Unix / 0 (belum di-set) */
#define	SYS_UDP_SEND	71u	/* App A4: udp_send(ip,port,buf,len) -> 0/-1 */
#define	SYS_UDP_RECV	72u	/* App A4: udp_recv(buf,max,&ip,&port) -> n/0/-1 */
/* Q2a: TCP client (satu koneksi aktif, pola request/response). */
#define	SYS_TCP_CONNECT	73u	/* tcp_connect(ip,port) -> 0/-1 */
#define	SYS_TCP_STATUS	74u	/* -> 0=CLOSED 1=SYN_SENT 2=ESTAB 3=FIN_SENT */
#define	SYS_TCP_SEND	75u	/* tcp_send(buf,len) -> n/0(coba lagi)/-1 */
#define	SYS_TCP_RECV	76u	/* tcp_recv(buf,max) -> n/0(belum ada)/-1 */
#define	SYS_TCP_CLOSE	77u	/* tcp_close() -> 0 */
#define	SYS_FACE_EXPR	78u	/* Q4: face_expr(expr, text) / poll. Lihat PLAN-Q4Q7Q8.md */
#define	SYS_SPAWN	79u	/* Q9: spawn(path) -> 0 ok, -1 gagal */
#define	SYS_SPAWN_WAIT	80u	/* Q9: tunggu spawn -> -1 jalan, else exit code */
#define	SYS_CONSOLE_TAKEOVER 81u /* Q9: jadi eksklusif pembaca console */
#define	SYS_AUDIO_INIT	82u	/* D1: acodec_init() -> 0/-1 */
#define	SYS_AUDIO_REG	83u	/* D1: acodec_reg_read(off) -> val */
#define	SYS_AUDIO_START	84u	/* D2: audio_start(rate,bits,ch) -> 0/-1 */
#define	SYS_AUDIO_READ	85u	/* D2: audio_read(buf,len) -> n/0/-1 */
#define	SYS_AUDIO_STOP	86u	/* D2: audio_stop() -> 0 */
#define	SYS_AUDIO_GAIN	87u	/* D3: audio_set_mic_gain(level) -> 0/-1 */
#define	SYS_AUDIO_OVERRUN 88u	/* D3: audio_overrun_count() -> n */
#define	SYS_PL330_INIT	89u	/* D2b: pl330_init() -> 0/-1 */
#define	SYS_PL330_RX	90u	/* D2b: pl330_audio_rx(dst,n) -> 0/-1 */
#define	SYS_PL330_UCLEN	91u	/* D2b: pl330_ucode_len() -> n */
#define	SYS_PL330_UCBYTE 92u	/* D2b: pl330_ucode_byte(i) -> b */
#define	SYS_LFS_RUNTEST	93u	/* B1: lfs_run_test() -> stage bitmask */
#define	SYS_LFS_MOUNT	94u	/* B1.5: lfs_port_mount() -> 0/-1 */
#define	SYS_LFS_LIST	95u	/* B1.5: (buf, max) -> bytes/-1 */
#define	SYS_LFS_READ	96u	/* B1.5: (path, buf, max) -> len/-1 */
#define	SYS_LFS_WRITE	97u	/* B1.5: (path, buf, len) -> 0/-1 */
#define	SYS_BOOTSTATE	98u	/* B2/B3/B4: subs 0 info 1 healthy 2 crash 3 flags 4/5 prov 6 updinfo 7 stage 8 sabotage */
#define	SYS_BOOTLOG	99u	/* B2: a0=idx/0xFFFFFFFF=count, a1=rec */
#define	SYS_TIME_QUALITY 100u	/* B3: -> 0 unsynced / 1 NTP synced */
#define	SYS_TIME_SYNC	101u	/* B3: mark clock NTP-synced */
#define	SYS_GUI_START	102u	/* Boot UX: launch GUI daemons on demand */
#define	SYS_WEBUI	103u	/* ONB-1: web UI bridge (shell queue + PIN) */
#define	SYS_CRED	104u	/* B5.5: credential store */
#define	SYS_EVENT_INJECT 105u	/* Dev/test: inject a button event into evq */
#define	SYS_RESET	106u	/* FND-1: software reset (PSCI SYSTEM_RESET) */
#define	SYS_POWEROFF	107u	/* FND-1: power off (PSCI SYSTEM_OFF) */
#define	SYS_SEND	10u	/* IPC-1: ipc send (dest, buf, len, flags) -> 0/UIPC_E* */
#define	SYS_RECV	11u	/* IPC-1: ipc recv (port, buf, max) -> len/UIPC_E*; IPC-2: a3=timeout ms (0=poll, >0=block till deadline, -1=block, no deadline) */
#define	SYS_RPC		12u	/* IPC-4: rpc (dest, buf, len, timeout_ms) -> reply len/UIPC_E*; request in, reply out, same buffer */
#define	SYS_RPC_USER	23u	/* IPC-4: rpc by registry name (name string, buf, len, timeout_ms) */
#define	SYS_IPC_PORT	25u	/* IPC-1: port mgmt; IPC-4: + registry (sub, a1, a2) */

/* Kode event input (App A2; disalin ke user/ulib/ulib.h). */
#define	EV_UP		0
#define	EV_DOWN		1
#define	EV_LEFT		2
#define	EV_RIGHT	3
#define	EV_OK		4
#define	EV_BACK		5
#define	EV_MENU		6
#define	EV_TICK		7

/*
 * struct qaon_stat (Fase C): layout DISALIN MANUAL ke user/ulib/ulib.h.
 * Field yang belum tersedia di port Mach 3 ini diisi sentinel
 * QAON_UNKNOWN (0xFFFFFFFF) — "tandai yang belum ada".
 */
#define	QAON_UNKNOWN	0xFFFFFFFFu

struct qaon_stat {
	unsigned int	uptime_ms;	/* timer_ticks * 10 (100 Hz) */
	unsigned int	cpu_pct;	/* QAON_UNKNOWN: belum ada idle accounting */
	unsigned int	mem_used_kb;	/* (total_pages - vm_page_free_count) * 4 */
	unsigned int	mem_total_kb;	/* mem_size / 1024 */
	unsigned int	blk_total_sec;	/* blk_total_sectors() (real) */
	unsigned int	blk_used_sec;	/* QAON_UNKNOWN: belum ada FS di disk */
	unsigned int	net_rx_kb;	/* 0: belum ada driver net */
	unsigned int	net_tx_kb;	/* 0: belum ada driver net */
	unsigned int	nthreads;	/* jumlah user task yang diluncurkan */
};

struct qaon_tentry {
	unsigned int	id;
	unsigned int	state;	/* 0=RUNNABLE, 2=EXITED (ala kernel lama) */
	unsigned int	user;	/* 1 = user task */
};

/* user_exit_trampoline codes */
#define	USER_EXIT_DABT	0xDAB7u	/* data abort in user mode */
#define	USER_EXIT_PABT	0x9AB7u	/* prefetch abort in user mode */
#define	USER_EXIT_UNDEF	0x5EEDu	/* undefined insn in user mode */

/* --- cache maintenance (Cortex-A7, 64B lines; same as blk.c) --- */
#define	CACHE_LINE	64u

static void
dcache_clean_range(unsigned int va, unsigned int len)
{
	unsigned int a, end;

	end = (va + len + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
	for (a = va & ~(CACHE_LINE - 1u); a < end; a += CACHE_LINE)
		__asm__ volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r" (a));
	__asm__ volatile ("dsb ish" ::: "memory");
}

static void
icache_invalidate_all(void)
{
	__asm__ volatile ("mcr p15, 0, %0, c7, c5, 0" :: "r" (0));
	__asm__ volatile ("dsb ish; isb" ::: "memory");
}

/*
 * Fase B/C user task bookkeeping.  One struct per launched program
 * (Fase C: init, ucat, uls, uecho, umon — sequential).
 */
struct user_task {
	task_t		task;
	vm_offset_t	heap_base;	/* VA: start of sbrk region */
	vm_offset_t	brk;		/* current break */
	vm_offset_t	heap_end;	/* VA: hard limit */
	vm_offset_t	va_top;		/* Q2b: VA tertinggi valid (layout dinamis) */
};

/* Task user yang sedang berjalan (di-set sebelum user_enter_test).
 * user_syscall memakai ini untuk sbrk + file syscalls. */
static task_t	cur_utask = TASK_NULL;
static struct user_task *cur_udesc = 0;

/* Helper asm (userasm.s): baca/tulis banked sp/lr USR dari SVC. */
extern void	ctx_save_usr(unsigned int *sp_out, unsigned int *lr_out);
extern void	ctx_restore_usr(unsigned int sp, unsigned int lr);

/* Identitas program pemanggil (App A2): -1=oneshot, 0=face, 1=uiapp. */
static int	cur_progid = -1;

/*
 * Scheduler kooperatif App A2: daemon user (face, uiapp) jalan
 * bergantian di atas boot thread.  Setiap SYS_YIELD menyimpan konteks
 * user (register + banked sp/lr + pc/spsr) dan memuat milik daemon
 * berikutnya; pmap + bookkeeping ikut diganti.  Net dipump tiap yield.
 */
struct user_ctx {
	unsigned int	r[13];	/* r0-r12 */
	unsigned int	sp_usr;
	unsigned int	lr_usr;
	unsigned int	pc;	/* frame->lr */
	unsigned int	spsr;
};

#define	NDAEMON		8u	/* face, uiapp, qabotd, uipcld (IPC-4), linkd (C6L-5), ulink5d (C6L-5), sh (Q9), + slot spawn (Q9) */
#define	PROG_FACE	0
#define	PROG_UIAPP	1
#define	PROG_NTP	2	/* App A4: sinkron jam (tak pegang display) */

struct daemon {
	const char	*name;
	task_t		task;
	pmap_t		pmap;
	struct user_task *udesc;
	struct user_ctx	ctx;
	int		state;	/* 0=mati, 1=runnable, 2=menunggu (Q9: parent spawn; IPC-2: blocking recv -- ipc_wait_active membedakan) */
	int		progid;
	unsigned	uprog_idx;	/* indeks di uprog_names/states */
	/* FND-3 health supervisor (see the wd_* block below
	 * sched_run). All ticks are arm_timer_ticks() units
	 * (100 Hz). */
	unsigned	wd_last_progress; /* tick of last non-YIELD syscall */
	unsigned	wd_warn_at;	/* age (ticks) that arms the warn */
	unsigned	wd_hang_at;	/* age (ticks) that declares a hang */
	unsigned	wd_warned;	/* tick the current warn fired, 0=none */
	unsigned	wd_hangs;	/* hangs declared this boot */
	unsigned	wd_respawns;	/* respawns this boot (persistent) */
	int		wd_imgidx;	/* daemon_images[] index, -1 = not
				 * respawnable (spawn slot) */
	/* IPC-2: blocking-receive wait record (see the
	 * IPC-2 block below sched_activate). Meaningful
	 * while ipc_wait_active: the daemon is parked in
	 * state 2 on this port, or was woken and has not
	 * yet re-entered SYS_RECV to collect the verdict.
	 * ipc_wait_port is the destination port OBJECT
	 * (ipc_port_t identity, opaque void * here) that
	 * the glue matches sends against;
	 * ipc_wait_deadline is an arm_timer_ticks()
	 * deadline, 0 = no deadline (the bootlogged -1
	 * form). */
	void		*ipc_wait_port;
	unsigned	ipc_wait_deadline;
	unsigned char	ipc_wait_active;
	/* IPC-3: peer annotation. When this daemon sends a
	 * reply-carrying COPY_SEND, the dispatch records
	 * the pending peer here: the reply port NAME in
	 * this daemon's space plus a held reference on the
	 * destination port OBJECT. At park-open the
	 * pending peer is adopted into the wait record
	 * ONLY if the parked port name is exactly that
	 * reply name (the RPC shape); a second request
	 * replaces the pending peer (old ref released).
	 * ipc_wait_peer_died is set by the reclaim scan
	 * when the annotated peer port is destroyed; the
	 * re-dispatch verdict is then UIPC_EPEER. The
	 * wait-record ref and the pending ref are never
	 * both held for the same wait (transfer). */
	void		*ipc_peer_next_obj;
	unsigned	ipc_peer_next_reply;
	void		*ipc_wait_peer_obj;
	unsigned char	ipc_wait_peer_died;
};

static struct daemon	daemons[NDAEMON];
static unsigned		ndaemon_reg = 0;

/* Boot UX: deferred GUI launch. sh asks via SYS_GUI_START; the
 * scheduler performs the launch from its own context. */
static volatile unsigned gui_pending = 0;
static unsigned		gui_launched = 0;
static int		sched_cur = -1;
static int		sched_active = 0;
/* IPC-2: return value of the daemon RESUMED by a
 * switch. trap.c writes user_syscall's return over the
 * (possibly switched) frame's r0, so a switch's caller
 * must hand back the resumed daemon's parked r0: 0 for
 * yield/exit parks (ctx_save forces it) and UIPC_EPARK
 * for IPC parks (ctx_save_wait). Every switch function
 * sets this right after its ctx_load; the no-switch
 * paths leave it 0. */
static unsigned		sched_resume_ret = 0;

/*
 * FND-3: health supervisor state (design + evidence:
 * docs/rencana-fondasi-tanpa-board/DEVLOG-FND3.md).
 *
 * Liveness model: a daemon proves progress by making
 * syscalls OTHER than SYS_YIELD -- every product daemon's
 * main loop polls something every iteration (face polls
 * events, uiapp polls acquire/events, qabotd and sh poll
 * the console), so a healthy daemon stamps wd_last_progress
 * many times per second. Two hang classes:
 *  (a) yields-but-wedged: yields continue (switch points
 *      run) but progress froze -- caught by wd_supervise()
 *      at the switch points;
 *  (b) never-yields: no switch point ever runs again --
 *      caught by wd_irq_tick() from the timer IRQ: if no
 *      scheduler switch happened for WD_STARVE_TICKS while
 *      a user-mode frame was running, the runner is
 *      spinning and only the IRQ path can still act.
 * WD_ENABLED exists so the pre-fix FAIL evidence could be
 * produced with the supervisor's call sites compiled out
 * and everything else identical (see the dev log).
 */
#define	WD_ENABLED	1
#define	WD_MAX_RESPAWN	2u	/* per daemon per boot */
#define	WD_MAX_CHILD_KILLS 3u	/* consecutive spawn-child kills */
#define	WD_STARVE_TICKS	3000u	/* class (b): 30 s at 100 Hz */
/* Boot log events (mirror bootstate.c BLE_WD_*). */
#define	WD_EV_WARN	13u
#define	WD_EV_HANG	14u
#define	WD_EV_RESPAWN	15u
#define	WD_EV_RESET	16u
/* Exit code a spawn parent sees when the supervisor killed
 * its child (decimal 53405 in sh's `run` output). */
#define	USER_EXIT_WDOG	0xD09Du

static volatile unsigned wd_last_activity; /* last switch tick */
static unsigned		wd_child_kills;	/* consecutive child kills */
static volatile int	wd_irq_tripped;

static int	wd_supervise(void);
static void	wd_fatal_reset(const char *who);
static int	wd_respawn(struct daemon *d);	/* K4: used by sched_exit_switch */
static void	wd_note(char *buf, unsigned int max, const char *kind,
    const char *name);	/* K4: used by sched_exit_switch */

/* Q9: state spawn. Slot NDAEMON-1 dipakai untuk child yang di-spawn.
 * spawn_parent = indeks daemon parent (yang menunggu), -1 bila tidak ada.
 * spawn_exitcode = exit code child; spawn_done = 1 bila child sudah exit. */
static int		spawn_parent = -1;
static unsigned		spawn_exitcode = 0;
static int		spawn_done = 0;

/* Q9: console owner (indeks daemon). -1 = semua boleh (default boot).
 * sh memanggil SYS_CONSOLE_TAKEOVER agar eksklusif baca console. */
static int		console_owner = -1;

/* Forward decl untuk SYS_SPAWN (definisi di bawah). */
static int	setup_uprog_task(const char *name, unsigned char *img,
				 unsigned int img_len,
				 unsigned int stack_pages,
				 struct user_task *udesc, task_t *out_task,
				 pmap_t *out_pmap,
				 unsigned int *out_stack_top);

/* Arbiter display (App A2): token dipegang face (0) atau uiapp (1). */
static int		dpy_holder = PROG_FACE;
static int		dpy_sleep_req = 0;

/* Permintaan ekspresi face (Q4): program mana pun boleh set; face
 * mengambil via poll (a0=0xFFFFFFFF) lalu flag di-clear. */
#define	FACE_TEXT_MAX	24u
static unsigned		face_req_expr = 0u;
static char		face_req_text[FACE_TEXT_MAX];
static int		face_req_pending = 0;
/* FND-2: daemon index that posted the pending face request
 * (-1 = none / not a daemon). Lets the death-reclaim path
 * drop a request whose poster is gone. */
static int		face_req_owner = -1;

/* Jam dinding (App A4): di-set program ntp via SYS_TIME_SET (detik
 * Unix UTC).  SYS_TIME_GET = set + (ticks_berlalu / 100). */
static unsigned		time_unix_set;
static unsigned		time_tick_set;
static int		time_valid = 0;
static int		time_synced = 0;	/* B3: set only via NTP */

/* Antrean event input untuk pemegang token. */
#define	EVQ_LEN		16u
static int		evq[EVQ_LEN];
static unsigned		evq_head = 0, evq_tail = 0;

static void
evq_clear(void)
{
	evq_head = evq_tail = 0;
}

static void
evq_push(int ev)
{
	unsigned nxt = (evq_tail + 1u) % EVQ_LEN;
	if (nxt == evq_head)
		return;	/* penuh: buang yang terbaru */
	evq[evq_tail] = ev;
	evq_tail = nxt;
}

static int
evq_pop(void)
{
	int ev;
	if (evq_head == evq_tail)
		return -1;
	ev = evq[evq_head];
	evq_head = (evq_head + 1u) % EVQ_LEN;
	return ev;
}

/* FND-2: daemon index that opened the single TCP client
 * connection (-1 = no daemon owner: closed, or opened by a
 * pre-scheduler one-shot). The connection is exclusive
 * (tcp_client_connect fails while one is open), so it is
 * per-daemon state: if the owner dies, the reclaim path
 * closes it instead of leaving every later connect to fail
 * until the connection timeout. */
static int		tcp_client_owner = -1;

/*
 * Penerjemah console -> EV_*: keyboard QEMU (WASD + Enter + Esc + M),
 * plus escape sequence panah (ESC [ A/B/C/D).  Non-blocking; -2 =
 * abaikan, -3 = butuh byte lanjutan (tak dipakai di sini).
 */
static int	con_pushback[4];
static int	con_npush = 0;

static int
con_getc(void)
{
	if (con_npush > 0)
		return con_pushback[--con_npush];
	return cnmaygetc();
}

static void
con_ungetc(int b)
{
	if (con_npush < 4 && b >= 0)
		con_pushback[con_npush++] = b;
}

static int
translate_key(int b)
{
	int b1, b2, ev;
	switch (b) {
	case 'w': case 'W': return EV_UP;
	case 's': case 'S': return EV_DOWN;
	case 'a': case 'A': return EV_LEFT;
	case 'd': case 'D': return EV_RIGHT;
	case '\r': case '\n': return EV_OK;
	case 'm': case 'M': return EV_MENU;
	case 0x1b:
		b1 = con_getc();
		if (b1 == '[') {
			b2 = con_getc();
			ev = -2;
			if (b2 == 'A') ev = EV_UP;
			else if (b2 == 'B') ev = EV_DOWN;
			else if (b2 == 'C') ev = EV_RIGHT;
			else if (b2 == 'D') ev = EV_LEFT;
			if (ev != -2)
				return ev;
			con_ungetc(b2);
			con_ungetc(b1);
		} else
			con_ungetc(b1);
		return EV_BACK;
	default:
		return -2;
	}
}

/* Daftar program user untuk SYS_TLIST (Fase C). App A1: 8 -> 16. */
#define	UPROG_MAX	16u
static const char	*uprog_names[UPROG_MAX];
static unsigned		uprog_states[UPROG_MAX];	/* 0=RUNNABLE, 2=EXITED */
static unsigned		nuprog = 0;

/*
 * Validasi range buffer user [va, va+len).  1 = valid.
 */
static int
user_range_ok(unsigned int va, unsigned int len)
{
	unsigned int va_top = USER_VA_TOP;
	/* Q2b: pakai batas dinamis per-task (image besar >192KB). */
	if (cur_udesc != 0 && cur_udesc->va_top > USER_VA_BASE)
		va_top = cur_udesc->va_top;
	if (va < USER_VA_BASE)
		return 0;
	if (len > va_top - USER_VA_BASE)
		return 0;
	if (va + len > va_top)
		return 0;
	if (va + len < va)	/* wraparound */
		return 0;
	return 1;
}

/*
 * Salin path NUL-terminated dari user ke buffer kernel.
 * 0 = ok, -1 = path tak valid / tak muat.
 */
static int
copy_path_user(unsigned int va, char *kpath, unsigned max)
{
	unsigned i;

	if (!user_range_ok(va, max))
		return -1;
	for (i = 0; i < max - 1u; i++) {
		char c = ((const char *)va)[i];
		kpath[i] = c;
		if (c == 0)
			return 0;
	}
	kpath[max - 1u] = 0;
	return -1;	/* tidak NUL-terminated dalam batas */
}

/*
 * user_syscall: dispatch a syscall from user mode.
 * Called from trap.c's TRAP_SVC path with the trap frame.  The return
 * value is stored to frame->r[0] by the caller.  SYS_EXIT (and only it)
 * redirects the frame to user_exit_trampoline instead of returning to
 * user mode.
 *
 * ABI: number in r7, args in r0-r2, return in r0; svc #0.
 */
/* Scheduler + arbiter App A2 (definisi setelah user_syscall). */
static void	sched_yield_switch(struct arm_trap_frame *frame);
static void	sched_exit_switch(struct arm_trap_frame *frame, unsigned int code);
/* IPC-2: blocking-receive engine (definisi di blok IPC-2). */
static unsigned int sys_recv_block(struct arm_trap_frame *frame,
				   unsigned int port, unsigned int ubuf,
				   unsigned int max, int timeout_ms);
/* IPC-4: RPC engine (definisi di blok IPC-4, setelah
 * sys_recv_block). */
static unsigned int sys_rpc_call(struct arm_trap_frame *frame,
				 unsigned int dest, unsigned int ubuf,
				 unsigned int len, int timeout_ms);
extern void	net_pump(void);

/* IPC-4: shared peer-note for SYS_SEND and the SYS_RPC
 * engine. On a successful reply-carrying COPY_SEND,
 * (re)set the pending peer: the old ref is released
 * first, a ref on the destination object is taken and
 * stored. A failed send, a send-once reply, or a
 * no-reply send stages nothing. */
static void
rpc_peer_note(void *dest_obj, unsigned int reply, unsigned int flags)
{
	struct daemon *sd;

	if (!sched_active || sched_cur < 0 ||
	    (flags & UIPC_F_SEND_ONCE) || reply == 0u ||
	    dest_obj == (void *)0)
		return;
	sd = &daemons[sched_cur];
	if (sd->ipc_peer_next_obj != (void *)0)
		ipc_glue_release_ref(sd->ipc_peer_next_obj);
	ipc_glue_take_ref(dest_obj);
	sd->ipc_peer_next_obj = dest_obj;
	sd->ipc_peer_next_reply = reply;
}

/* #7 debug: current syscall number (for panic diagnosis). */
unsigned int	dbg_cur_syscall;

unsigned int
user_syscall(struct arm_trap_frame *frame)
{
	unsigned int num = frame->r[7];
	unsigned int a0 = frame->r[0];
	unsigned int a1 = frame->r[1];
	unsigned int a2 = frame->r[2];
	unsigned int a3 = frame->r[3];

	/* #7 debug: track current syscall for panic diagnosis. */
	extern unsigned int dbg_cur_syscall;
	dbg_cur_syscall = num;
	/* FND-3: progress stamp. Any syscall other than
	 * SYS_YIELD is work (polling counts: the daemon loops
	 * are poll-shaped), so it proves the caller is not
	 * wedged in a bare yield loop. */
	if (sched_active && sched_cur >= 0 && num != SYS_YIELD)
		daemons[sched_cur].wd_last_progress =
		    arm_timer_ticks();
	unsigned int a4 = frame->r[4];
	unsigned int a5 = frame->r[5];

	switch (num) {
	case SYS_WRITE: {
		const unsigned char *p;
		unsigned int i;

		/* Fase B validation: buffer must lie in the user VA
		 * window.  (Per-page table walk = future work; Fase B
		 * trusts the range.  All Fase B user mappings live in
		 * [USER_VA_BASE, USER_VA_TOP).) */
		if (!user_range_ok(a1, a2))
			return (unsigned int)-1;
		if (a0 == 1u || a0 == 2u) {
			p = (const unsigned char *)a1;
			for (i = 0; i < a2; i++)
				uart_putc((char)p[i]);
			return a2;
		}
		if (a0 >= 3u && cur_utask != TASK_NULL)
			return (unsigned int)ramfs_write(cur_utask, a0,
							 (const unsigned char *)a1,
							 a2);
		return (unsigned int)-1;
	}
	case SYS_YIELD:
		/* App A2: bila scheduler daemon aktif, yield = pindah ke
		 * daemon runnable berikutnya (round-robin) + pump net.
		 * Konteks pemanggil disimpan; saat kembali nanti r0=0.
		 * Fase one-shot (belum aktif): nop.
		 * IPC-2: the return is sched_resume_ret -- the
		 * RESUMED daemon's parked r0 (0 for yield/exit
		 * parks, UIPC_EPARK for IPC parks), because
		 * trap.c overwrites the switched frame's r0
		 * with this value. */
		if (sched_active) {
			sched_yield_switch(frame);
			return sched_resume_ret;
		}
		return 0;
	case SYS_SBRK: {
		/* Heap pages are pre-allocated at task creation (no
		 * allocator calls in trap context); sbrk just bumps the
		 * break.  Returns old brk, or -1 on failure. */
		vm_offset_t new_brk;

		if (cur_udesc == 0)
			return (unsigned int)-1;
		if (a0 == 0)
			return cur_udesc->brk;
		if (a0 > USER_VA_TOP - cur_udesc->brk)
			return (unsigned int)-1;
		new_brk = cur_udesc->brk + a0;
		if (new_brk > cur_udesc->heap_end)
			return (unsigned int)-1;
		a0 = cur_udesc->brk;	/* old brk = return value */
		cur_udesc->brk = new_brk;
		return a0;
	}
	case SYS_OPEN: {
		char kpath[RAMFS_PATH_MAX];

		if (cur_utask == TASK_NULL)
			return (unsigned int)-1;
		if (copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)ramfs_open(cur_utask, kpath, a1);
	}
	case SYS_READ: {
		/* fd 0 (stdin): tidak ada input di bring-up -> 0 (EOF).
		 * fd 1/2: tak valid untuk read.  fd >= 3: ramfs. */
		if (a0 == 0u)
			return 0;
		if (a0 == 1u || a0 == 2u)
			return (unsigned int)-1;
		if (!user_range_ok(a1, a2))
			return (unsigned int)-1;
		if (cur_utask == TASK_NULL)
			return (unsigned int)-1;
		return (unsigned int)ramfs_read(cur_utask, a0,
						(unsigned char *)a1, a2);
	}
	case SYS_CLOSE:
		if (cur_utask == TASK_NULL)
			return (unsigned int)-1;
		return (unsigned int)ramfs_close(cur_utask, a0);
	case SYS_LS:
		if (!user_range_ok(a0, a1))
			return (unsigned int)-1;
		return (unsigned int)ramfs_list((char *)a0, a1);
	case SYS_DELETE: {
		char kpath[RAMFS_PATH_MAX];

		if (copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)ramfs_delete(kpath);
	}
	case SYS_STAT: {
		struct qaon_stat *s;
		unsigned int total_pages;

		if (a1 < sizeof(struct qaon_stat))
			return (unsigned int)-1;
		if (!user_range_ok(a0, sizeof(struct qaon_stat)))
			return (unsigned int)-1;
		s = (struct qaon_stat *)a0;
		s->uptime_ms = arm_timer_ticks() * 10u;
		s->cpu_pct = QAON_UNKNOWN;	/* belum ada idle accounting */
		total_pages = (unsigned int)(mem_size / 4096u);
		s->mem_total_kb = (unsigned int)(mem_size / 1024u);
		s->mem_used_kb = (total_pages - (unsigned int)vm_page_free_count)
				 * 4u;
		s->blk_total_sec = blk_total_sectors();
		s->blk_used_sec = QAON_UNKNOWN;	/* belum ada FS di disk */
		s->net_rx_kb = 0;	/* belum ada driver net (Fase D) */
		s->net_tx_kb = 0;
		s->nthreads = nuprog;
		return 0;
	}
	case SYS_TLIST: {
		struct qaon_tentry *e;
		unsigned i, n;

		if (a1 == 0u || a1 > 64u)
			return (unsigned int)-1;
		if (!user_range_ok(a0,
				   a1 * (unsigned)sizeof(struct qaon_tentry)))
			return (unsigned int)-1;
		e = (struct qaon_tentry *)a0;
		n = (nuprog < a1) ? nuprog : a1;
		for (i = 0; i < n; i++) {
			e[i].id = i;
			e[i].state = uprog_states[i];
			e[i].user = 1u;
		}
		return n;
	}
	case SYS_READ_CONSOLE:
		/* Q9: hanya console owner yang boleh baca (bila owner diset). */
		if (console_owner != -1 && sched_cur != console_owner)
			return (unsigned int)-1;
		return (unsigned int)cnmaygetc();
	case SYS_GPIO_SET:
		/* Bank di-fix 0 (seperti kernel lama Fase 14). */
		return (unsigned int)gpio_set(0u, a0, a1);
	case SYS_GPIO_GET:
		return (unsigned int)gpio_get(0u, a0);
	case SYS_SD_READ:
		if (!sd_present() || !user_range_ok(a1, 512u))
			return (unsigned int)-1;
		return (unsigned int)sd_read(a0, (unsigned char *)a1);
	case SYS_SD_WRITE:
		if (!sd_present() || !user_range_ok(a1, 512u))
			return (unsigned int)-1;
		return (unsigned int)sd_write(a0, (unsigned char *)a1);
	case SYS_MKDIR: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)fat32_mkdir(kpath);
	}
	case SYS_FAT_WRITE: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)fat32_write_file(kpath,
						      (unsigned char *)a1, a2);
	}
	case SYS_FAT_READ: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)fat32_read_file(kpath,
						     (unsigned char *)a1, a2);
	}
	case SYS_FAT_DELETE: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;
		return (unsigned int)fat32_delete(kpath);
	}
	case SYS_READDIR: {
		static char kpath[128];
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)fat32_listdir(kpath, (char *)a1, a2);
	}
	case SYS_DISPLAY_INFO: {
		struct qaon_display *d;
		if (!gpu_available_p() || !user_range_ok(a0, a1) ||
		    a1 < sizeof(struct qaon_display))
			return (unsigned int)-1;
		d = (struct qaon_display *)a0;
		d->width = QAON_DISP_W;
		d->height = QAON_DISP_H;
		d->format = 0u;
		return 0u;
	}
	case SYS_DISPLAY_FLUSH: {
		/* a0=x a1=y a2=w a3=h a4=buf(RGB565) a5=len */
		/* App A2: hanya pemegang token display boleh flush. */
		if (cur_progid != dpy_holder)
			return (unsigned int)-1;
		if (!gpu_available_p() ||
		    a2 == 0u || a3 == 0u ||
		    a0 + a2 > QAON_DISP_W || a1 + a3 > QAON_DISP_H ||
		    a5 < a2 * a3 * 2u ||
		    !user_range_ok(a4, a5))
			return (unsigned int)-1;
		return (unsigned int)gpu_flush_strip(a0, a1, a2, a3,
						    (const unsigned short *)a4);
	}
	case SYS_EXIT:
		if (sched_active) {
			sched_exit_switch(frame, a0);
			/* IPC-2: hand the RESUMED daemon its
			 * parked r0 (see SYS_YIELD). */
			return sched_resume_ret;
		}
		/* Leave user mode for good: resume at the trampoline
		 * in SVC; r0 carries the exit code. */
		frame->r[0] = a0;
		frame->lr = (unsigned int)user_exit_trampoline;
		frame->spsr = ARM_MODE_SVC;
		return a0;
	case SYS_DISPLAY_GRANT: {
		/* Hanya face boleh memberi token ke uiapp. */
		if (cur_progid != PROG_FACE)
			return (unsigned int)-1;
		dpy_holder = PROG_UIAPP;
		evq_clear();
		printf("[dpy] GRANT -> uiapp\n");
		return 0;
	}
	case SYS_DISPLAY_ACQUIRE:
		return (dpy_holder == cur_progid) ? 1u : 0u;
	case SYS_DISPLAY_RELEASE: {
		if (cur_progid != dpy_holder)
			return (unsigned int)-1;
		dpy_holder = PROG_FACE;
		/* JANGAN clear dpy_sleep_req di sini: alur Sleep Now =
		 * sleep(1) DULU baru release; flag harus sampai ke face. */
		evq_clear();
		printf("[dpy] RELEASE -> face\n");
		return 0;
	}
	case SYS_DISPLAY_GET_EVENT: {
		int b, ev;
		/* Hanya pemegang token yang dapat antrean event. */
		if (cur_progid != dpy_holder)
			return (unsigned int)-1;
		/* #7: jangan kuras console bila dimiliki pihak lain
		 * (mis. shell via sys_console_takeover). */
		if (console_owner != -1 && cur_progid != console_owner)
			return (unsigned int)evq_pop();
		/* Kuras console -> terjemahkan -> antrekan. */
		for (;;) {
			b = con_getc();
			if (b < 0)
				break;
			ev = translate_key(b);
			if (ev >= 0)
				evq_push(ev);
		}
		return (unsigned int)evq_pop();
	}
	case SYS_DISPLAY_STATUS:
		return (unsigned int)dpy_holder;
	case SYS_UPTIME:
		return arm_timer_ticks() * 10u;
	case SYS_DISPLAY_SLEEP: {
		int ev;
		/* r0=1: pemegang token minta sleep; r0=0: face ambil+clear. */
		if (a0 == 0) {
			if (cur_progid != PROG_FACE)
				return (unsigned int)-1;
			ev = dpy_sleep_req;
			dpy_sleep_req = 0;
			return (unsigned int)ev;
		} else {
			if (cur_progid != dpy_holder)
				return (unsigned int)-1;
			dpy_sleep_req = 1;
			return 0;
		}
	}
	case SYS_TIME_SET:
		/* a0 = detik Unix (UTC). */
		time_unix_set = a0;
		time_tick_set = arm_timer_ticks();
		time_valid = 1;
		return 0;
	case SYS_TIME_GET: {
		unsigned dt;
		if (!time_valid)
			return 0;
		dt = arm_timer_ticks() - time_tick_set;
		return time_unix_set + dt / 100u;
	}
	case SYS_UDP_SEND: {
		/* a0=ip dst, a1=port dst, a2=buf, a3=len -> 0/-1 */
		int r;
		if (a3 > 512u || !user_range_ok(a2, a3))
			return (unsigned int)-1;
		r = netstack_udp_send(a0, (unsigned short)a1,
				      (const unsigned char *)a2, a3);
		return (unsigned int)(r < 0 ? -1 : 0);
	}
	case SYS_UDP_RECV: {
		/* a0=buf, a1=maxlen, a2=&src_ip, a3=&src_port */
		int n;
		unsigned int sip;
		unsigned short sport;
		if (a1 == 0u || a1 > 512u || !user_range_ok(a0, a1) ||
		    !user_range_ok(a2, sizeof(unsigned int)) ||
		    !user_range_ok(a3, sizeof(unsigned short)))
			return (unsigned int)-1;
		n = netstack_udp_recv((unsigned char *)a0, a1, &sip, &sport);
		if (n > 0) {
			*(unsigned int *)a2 = sip;
			*(unsigned short *)a3 = sport;
		}
		return (unsigned int)n;
	}
	case SYS_TCP_CONNECT: {
		/* a0=ip dst, a1=port dst -> 0/-1 */
		int rc;
		if (a1 > 65535u)
			return (unsigned int)-1;
		rc = tcp_client_connect(a0, (unsigned short)a1);
		if (rc == 0)	/* FND-2: record the owning daemon */
			tcp_client_owner = sched_active ? sched_cur : -1;
		return (unsigned int)rc;
	}
	case SYS_TCP_STATUS:
		return (unsigned int)tcp_client_status();
	case SYS_TCP_SEND: {
		/* a0=buf, a1=len -> n/0/-1 */
		int n;
		if (a1 == 0u || a1 > 1200u || !user_range_ok(a0, a1))
			return (unsigned int)-1;
		n = tcp_client_send((const unsigned char *)a0, a1);
		return (unsigned int)n;
	}
	case SYS_TCP_RECV: {
		/* a0=buf, a1=maxlen -> n/0/-1 */
		int n;
		if (a1 == 0u || a1 > 4096u || !user_range_ok(a0, a1))
			return (unsigned int)-1;
		n = tcp_client_recv((unsigned char *)a0, a1);
		return (unsigned int)n;
	}
	case SYS_TCP_CLOSE:
		tcp_client_close();
		tcp_client_owner = -1;	/* FND-2 */
		return 0;
	case SYS_FACE_EXPR: {
		/* Q4: a0=expr(0..7) set, a0=0xFFFFFFFF poll.
		 * set: a1=ptr teks status (0=biarkan default face).
		 * poll (oleh face): a1=ptr unsigned expr_out, a2=ptr char[24].
		 *   -> 0 bila ada permintaan (flag clear), -1 bila tak ada. */
		if (a0 == 0xFFFFFFFFu) {
			unsigned i;
			unsigned *eo = (unsigned *)a1;
			char *to = (char *)a2;
			if (!face_req_pending)
				return (unsigned int)-1;
			if (!user_range_ok(a1, sizeof(unsigned)) ||
			    !user_range_ok(a2, FACE_TEXT_MAX))
				return (unsigned int)-1;
			*eo = face_req_expr;
			for (i = 0; i < FACE_TEXT_MAX; i++)
				to[i] = face_req_text[i];
			face_req_pending = 0;
			face_req_owner = -1;
			return 0;
		}
		if (a0 > 7u)
			return (unsigned int)-1;
		face_req_expr = a0;
		if (a1 != 0u) {
			unsigned i;
			const char *s = (const char *)a1;
			/* salin aman: cari NUL dalam batas wajar */
			if (!user_range_ok(a1, FACE_TEXT_MAX))
				return (unsigned int)-1;
			for (i = 0; i < FACE_TEXT_MAX - 1u; i++) {
				face_req_text[i] = s[i];
				if (s[i] == '\0')
					break;
			}
			face_req_text[FACE_TEXT_MAX - 1u] = '\0';
		} else {
			face_req_text[0] = '\0';	/* pakai default face */
		}
		face_req_pending = 1;
		face_req_owner = sched_active ? sched_cur : -1;	/* FND-2 */
		return 0;
	}
	case SYS_SPAWN: {
		/* Q9: a0=path program (mis. "/bin/hello"). Setup child di
		 * slot NDAEMON-1, tandai caller WAITING. Return 0 bila
		 * spawn dimulai, -1 bila gagal. Parent tunggu via
		 * SYS_SPAWN_WAIT. */
		static char kpath[128];
		static unsigned char imgbuf[65536]; /* 64KB batas v1 */
		struct daemon *child;
		struct user_task *udesc;
		task_t utask;
		pmap_t upmap;
		unsigned int stack_top;
		int len;
		int embed_progid = -1;

		if (!sched_active || sched_cur < 0)
			return (unsigned int)-1;
		if (spawn_parent != -1)
			return (unsigned int)-1; /* sudah ada spawn aktif */
		if (!fat32_mounted() ||
		    copy_path_user(a0, kpath, sizeof(kpath)) != 0)
			return (unsigned int)-1;

		len = fat32_read_file(kpath, imgbuf, sizeof(imgbuf));
		if (len <= 0) {
			/* Test hooks: "embed:<name>" spawns an embedded
			 * (UI-8: the IPC-2/4/5 and C6L test hooks below exist
			 * only in QA builds (QA=1); production images omit
			 * them, so those spawn names fail there by design.)
			 * image without depending on FAT (Q9 pattern).
			 *   embed:uls     Q9 spawn self-test program.
			 *   embed:uipkpark (65 s deadline park)
			 *     and embed:uipknd (no-deadline park)
			 *     are IPC-2's park probes (kept: they
			 *     do not use the deleted rendezvous).
			 *   embed:uiptoolssrv  IPC-4 mock `tools`
			 *     server. Its progid override (2, the
			 *     qabotd daemon_images index) exists
			 *     ONLY in this hook: the registry's
			 *     reserved-name policy is progid-keyed,
			 *     so a test server that must claim
			 *     `tools` has to carry the owner progid
			 *     (same precedent as ufaultd's
			 *     PROG_UIAPP override below).
			 *   embed:uipcregexit / embed:uipcregfull
			 *     IPC-4 registry legs (anti-rendezvous
			 *     + table-full probes).
			 *   embed:ufault  FND-2: takes the console, faults.
			 *   embed:ufaultd FND-2: becomes the display
			 *     holder, then faults. Its progid override
			 *     (PROG_UIAPP) exists ONLY in this hook: the
			 *     display token is progid-keyed, so a test
			 *     child that must die holding it has to carry
			 *     the holder progid. Product spawns (FAT)
			 *     always get progid -1. */
				extern unsigned char	uls_img[], ufault_img[],
						ufaultd_img[], uyield_img[],
						uhangd_img[], uspin_img[],
						uhangc_img[], ufpu_img[]
#ifdef QA_TIER_A
						, uipkpark_img[],
						uipknd_img[],
						uiptoolssrv_img[],
						uipcregexit_img[],
						uipcregfull_img[],
						uipcaud_img[]
#endif /* QA_TIER_A */
						;
			extern unsigned int	uls_img_len, ufault_img_len,
						ufaultd_img_len, uyield_img_len,
						uhangd_img_len, uspin_img_len,
						uhangc_img_len, ufpu_img_len
#ifdef QA_TIER_A
						, uipkpark_img_len,
						uipknd_img_len,
						uiptoolssrv_img_len,
						uipcregexit_img_len,
						uipcregfull_img_len,
						uipcaud_img_len
#endif /* QA_TIER_A */
						;
			static const struct {
				const char	*name;
				unsigned char	*img;
				unsigned int	*lenp;
				int		progid;
			} hooks[] = {
				{ "embed:uls", uls_img, &uls_img_len, -1 },
				{ "embed:ufault", ufault_img,
				  &ufault_img_len, -1 },
				{ "embed:ufaultd", ufaultd_img,
				  &ufaultd_img_len, PROG_UIAPP },
				/* FND-3 hang injection: yields forever
				 * without progress; holds the display
				 * (PROG_UIAPP) then yields forever;
				 * spins without ever yielding. */
				{ "embed:uyield", uyield_img,
				  &uyield_img_len, -1 },
				{ "embed:uhangd", uhangd_img,
				  &uhangd_img_len, PROG_UIAPP },
				{ "embed:uspin", uspin_img,
				  &uspin_img_len, -1 },
				{ "embed:uhangc", uhangc_img,
				  &uhangc_img_len, -1 },
				/* K1: user-mode VFP pattern vs IPC park. */
				{ "embed:ufpu", ufpu_img,
				  &ufpu_img_len, -1 },
#ifdef QA_TIER_A
				/* IPC-2: park probes (see docs/rencana-ipc). */
				{ "embed:uipkpark", uipkpark_img,
				  &uipkpark_img_len, -1 },
				{ "embed:uipknd", uipknd_img,
				  &uipknd_img_len, -1 },
				/* IPC-4: mock tools server (progid 2,
				 * the qabotd index, so it may claim
				 * the reserved name `tools`) +
				 * registry legs (see docs/rencana-ipc). */
				{ "embed:uiptoolssrv", uiptoolssrv_img,
				  &uiptoolssrv_img_len, 2 },
				{ "embed:uipcregexit", uipcregexit_img,
				  &uipcregexit_img_len, -1 },
				{ "embed:uipcregfull", uipcregfull_img,
				  &uipcregfull_img_len, -1 },
				/* IPC-5: AUD-rate queue sizing probe
				 * (see docs/rencana-ipc/DEVLOG-IPC5). */
				{ "embed:uipcaud", uipcaud_img,
				  &uipcaud_img_len, -1 },
				/* C6L-2: bridge backend + host peer
				 * test (see docs/rencana-komunikasi-
				 * c6/DEVLOG-C6L2). Spawn-only on
				 * purpose: a boot one-shot runs
				 * with the scheduler inactive, where
				 * SYS_YIELD is a nop and net_pump
				 * never runs, so the bridge could
				 * never complete a handshake there. */
				{ "embed:ulinkbr", ulinkbr_img,
				  &ulinkbr_img_len, -1 },
				/* C6L-3: v2 completion test
				 * (TOOL.DISPATCH/EVT/NET.WS;
				 * see docs/rencana-komunikasi-
				 * c6/DEVLOG-C6L3). Spawn-only
				 * like ulinkbr: the bridge leg
				 * needs the scheduler active. */
				{ "embed:ulink3", ulink3_img,
				  &ulink3_img_len, -1 },
				/* C6L-4: AUD streaming integrity
				 * + error semantics (see
				 * docs/rencana-komunikasi-
				 * c6/DEVLOG-C6L4). Spawn-only
				 * like ulinkbr/ulink3: the
				 * bridge leg needs the
				 * scheduler active. */
				{ "embed:ulinkaud", ulinkaud_img,
				  &ulinkaud_img_len, -1 },
				/* C6L-5: linkd client test
				 * (see docs/rencana-komunikasi-
				 * c6/DEVLOG-C6L5). Spawn-only
				 * like ulinkbr/ulink3/ulinkaud. */
				{ "embed:ulink5", ulink5_img,
				  &ulink5_img_len, -1 },
				/* C6L-5: stub tools server
				 * (progid 2, the qabotd index,
				 * so it may claim the reserved
				 * name `tools`; must never run
				 * together with uiptoolssrv). */
				{ "embed:ulinktools", ulinktools_img,
				  &ulinktools_img_len, 2 },
				/* C6L-5: client-death probe
				 * (raw SEND + immediate exit). */
				{ "embed:ulink5die", ulink5die_img,
				  &ulink5die_img_len, -1 },
#endif /* QA_TIER_A */
			};
			unsigned int hi, ei;
			int matched = 0;

			for (hi = 0; hi < sizeof(hooks) / sizeof(hooks[0]);
			     hi++) {
				const char *want = hooks[hi].name;
				for (ei = 0; want[ei]; ei++)
					if (kpath[ei] != want[ei])
						break;
				if (want[ei] == '\0' && kpath[ei] == '\0') {
					matched = 1;
					break;
				}
			}
			if (!matched ||
			    *hooks[hi].lenp > sizeof(imgbuf))
				return (unsigned int)-1;
			for (ei = 0; ei < *hooks[hi].lenp; ei++)
				imgbuf[ei] = hooks[hi].img[ei];
			len = (int)*hooks[hi].lenp;
			embed_progid = hooks[hi].progid;
		}

		/* Setup task child di slot terakhir. */
		child = &daemons[NDAEMON - 1];
		/* udesc sederhana: pakai static (satu spawn aktif v1). */
		{
			static struct user_task spawn_udesc;
			udesc = &spawn_udesc;
		}
		if (setup_uprog_task(kpath, imgbuf, (unsigned int)len,
				     1u, udesc, &utask, &upmap,
				     &stack_top) != 0)
			return (unsigned int)-1;

		child->name = "spawned";
		child->task = utask;
		child->pmap = upmap;
		child->udesc = udesc;
		{
			unsigned int ri;
			for (ri = 0; ri < 13u; ri++)
				child->ctx.r[ri] = 0;
		}
		child->ctx.sp_usr = stack_top;
		child->ctx.lr_usr = 0;
		child->ctx.pc = INIT_CODE_VA;
		child->ctx.spsr = 0x10u;	/* USR, IRQ on */
		child->state = 1; /* runnable */
		child->progid = embed_progid;	/* -1; hook override utk test */
		child->uprog_idx = 0;
		/* FND-3: the spawn slot is supervised too (kill +
		 * wake the parent; never respawned -- the parent
		 * decides what runs next). */
		child->wd_last_progress = arm_timer_ticks();
		child->wd_warn_at = 1500u;	/* 15 s */
		child->wd_hang_at = 3000u;	/* 30 s */
		child->wd_warned = 0;
		child->wd_hangs = 0;
		child->wd_respawns = 0;
		child->wd_imgidx = -1;
		/* IPC-3: a fresh slot starts with no peer
		 * state (defense in depth: reclaim clearing
		 * covers the funnel, setup clearing covers
		 * any future path that skips reclaim). */
		child->ipc_wait_active = 0;
		child->ipc_wait_port = (void *)0;
		child->ipc_wait_deadline = 0u;
		child->ipc_peer_next_obj = (void *)0;
		child->ipc_peer_next_reply = 0u;
		child->ipc_wait_peer_obj = (void *)0;
		child->ipc_wait_peer_died = 0;

		spawn_parent = sched_cur;
		spawn_done = 0;
		daemons[sched_cur].state = 2; /* WAITING */
		printf("spawn: '%s' (%d byte), parent=%d menunggu\n",
		       kpath, len, sched_cur);
		return 0;
	}
	case SYS_SPAWN_WAIT: {
		/* Q9: parent cek status spawn. Return -1 bila child masih
		 * jalan, exit code (0..255) bila sudah selesai. */
		if (spawn_parent != sched_cur)
			return (unsigned int)-1;
		if (!spawn_done)
			return (unsigned int)-1;
		spawn_parent = -1;
		daemons[sched_cur].state = 1; /* kembali runnable */
		return spawn_exitcode;
	}
	case SYS_CONSOLE_TAKEOVER: {
		/* Q9: caller jadi eksklusif pembaca console. */
		if (!sched_active || sched_cur < 0)
			return (unsigned int)-1;
		console_owner = sched_cur;
		printf("console: owner -> '%s' (%d)\n",
		       daemons[sched_cur].name, sched_cur);
		return 0;
	}
	case SYS_AUDIO_INIT:
		/* D1: init ACODEC -> 0. */
		return (unsigned int)acodec_init();
	case SYS_AUDIO_REG:
		/* D1: a0=offset byte -> nilai register. */
		return acodec_reg_read(a0);
	case SYS_AUDIO_START:
		/* D2: a0=rate, a1=bits, a2=ch -> 0/-1. */
		return (unsigned int)audio_start(a0, a1, a2);
	case SYS_AUDIO_READ: {
		/* D2: a0=buf, a1=len -> n/0(belum ada)/-1. */
		int n;
		if (a1 == 0u || a1 > 8192u || !user_range_ok(a0, a1))
			return (unsigned int)-1;
		n = audio_read((unsigned char *)a0, a1);
		return (unsigned int)n;
	}
	case SYS_AUDIO_STOP:
		/* D2: stop I2S RX -> 0. */
		audio_stop();
		return 0;
	case SYS_AUDIO_GAIN:
		/* D3: a0=level(0..2) -> 0/-1. */
		return (unsigned int)audio_set_mic_gain(a0);
	case SYS_AUDIO_OVERRUN:
		/* D3: -> overrun counter. */
		return audio_overrun_count();
	case SYS_PL330_INIT:
		/* D2b: init PL330 -> 0/-1. */
		return (unsigned int)pl330_init();
	case SYS_PL330_RX: {
		/* D2b: a0=dst, a1=nwords -> 0/-1. */
		if ((a0 & 3u) || a1 == 0u || a1 > 256u ||
		    !user_range_ok(a0, a1 * 4u))
			return (unsigned int)-1;
		return (unsigned int)pl330_audio_rx(a0, a1);
	}
	case SYS_PL330_UCLEN:
		/* D2b: -> panjang microcode. */
		return pl330_ucode_len();
	case SYS_PL330_UCBYTE: {
		/* D2b: a0=idx -> byte, atau verifikasi (a0=0xFFFFFFFF). */
		unsigned int fails = 0u;
		if (a0 == 0xffffffffu) {
			/* Verifikasi struktur: kembalikan bitmask fail. */
			if (pl330_ucode_buf[0] != 0xbcu ||
			    pl330_ucode_buf[1] != 0x00u)
				fails |= 1u;	/* DMAMOV SAR */
			if (pl330_ucode_buf[6] != 0xbcu ||
			    pl330_ucode_buf[7] != 0x02u)
				fails |= 2u;	/* DMAMOV DAR */
			if (pl330_ucode_buf[2] != 0x28u ||
			    pl330_ucode_buf[5] != 0xffu)
				fails |= 4u;	/* SAR addr */
			if (pl330_ucode_buf[pl330_ucode_buflen - 1u] != 0x00u)
				fails |= 8u;	/* DMAEND */
			return fails;
		}
		return pl330_ucode_byte(a0);
	}
	case SYS_LFS_RUNTEST:
		/* B1: -> bitmask tahap uji littlefs (0x1f = semua lulus). */
		return (unsigned int)lfs_run_test();
	case SYS_LFS_MOUNT:
		return (unsigned int)lfs_port_mount();
	case SYS_LFS_LIST: {
		if (!user_range_ok(a0, a1))
			return (unsigned int)-1;
		return (unsigned int)lfs_port_list((char *)a0, a1);
	}
	case SYS_LFS_READ: {
		static char kpath[128];
		if (copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)lfs_port_read(kpath,
		    (unsigned char *)a1, a2);
	}
	case SYS_GUI_START: {
		/* Boot UX: request the GUI daemons (face/uiapp/
		 * qabotd). The actual launch happens in
		 * sched_yield_switch -- creating tasks from the
		 * requester's syscall frame is not safe. */
		if (bootstate_safe_mode())
			return (unsigned int)-1;
		if (gui_launched)
			return 1u;
		gui_pending = 1;
		return 0u;
	}
	case SYS_WEBUI: {
		/* ONB-1 web UI bridge. a0 sub:
		 * 0 take web-shell command (a1 buf, a2 max) -> len/-1
		 * 1 post web-shell result (a1 buf, a2 len) -> 0/-1
		 * 2 set device PIN (a1 buf, a2 len) -> 0/-1
		 * 3 PIN present -> 1/0
		 * 4 verify device PIN (a1 buf, a2 len) -> 1/0/-1
		 *   (UI-3: device Settings screen; same
		 *   webui_pin_verify as the web login, so the
		 *   constant-time compare + throttle apply) */
		if (a0 == 0u) {
			static char kcmd[192];
			int n;
			if (!user_range_ok(a1, a2))
				return (unsigned int)-1;
			n = webui_shell_take(kcmd, sizeof(kcmd));
			if (n < 0)
				return (unsigned int)-1;
			{
				unsigned int i;
				char *u = (char *)a1;
				for (i = 0; i <= (unsigned int)n; i++)
					u[i] = kcmd[i];
			}
			return (unsigned int)n;
		}
		if (a0 == 1u) {
			if (!user_range_ok(a1, a2) || a2 > 8192u)
				return (unsigned int)-1;
			return (unsigned int)webui_shell_post(
			    (const char *)a1, a2);
		}
		if (a0 == 2u) {
			static char kpin[20];
			unsigned int i;
			if (a2 < 4u || a2 > 16u ||
			    !user_range_ok(a1, a2))
				return (unsigned int)-1;
			for (i = 0; i < a2; i++)
				kpin[i] = ((const char *)a1)[i];
			kpin[a2] = 0;
			return (unsigned int)webui_pin_set(kpin, a2);
		}
		if (a0 == 3u)
			return (unsigned int)webui_pin_present();
		if (a0 == 4u) {
			/* UI-3: same bounds as sub 2; the return
			 * is tri-state (1 ok, 0 wrong, -1 locked)
			 * and must stay signed for the caller. */
			static char kpin[20];
			unsigned int i;
			if (a2 < 4u || a2 > 16u ||
			    !user_range_ok(a1, a2))
				return (unsigned int)-1;
			for (i = 0; i < a2; i++)
				kpin[i] = ((const char *)a1)[i];
			kpin[a2] = 0;
			return (unsigned int)webui_pin_verify(kpin, a2);
		}
		return (unsigned int)-1;
	}
	case SYS_CRED: {
		/* B5.5 credential store. a1 = name (NUL-term string),
		 * a2 = value buf, a3 = len (set) / max (get):
		 * 0 set -> 0/-1; 1 get -> len/-1; 2 present -> 1/0;
		 * 3 clear -> 0/-1; 9 self-test -> pass bitmask. */
		static char kname[20];
		static unsigned char kval[512];
		unsigned int i;

		if (a0 == 9u)
			return (unsigned int)credstore_selftest();
		if (!user_range_ok(a1, 1u))
			return (unsigned int)-1;
		for (i = 0; i < sizeof(kname) - 1u; i++) {
			kname[i] = ((const char *)a1)[i];
			if (!kname[i])
				break;
		}
		kname[sizeof(kname) - 1u] = 0;
		/* credstore validates the name charset/length. */
		if (a0 == 0u) {
			if (a3 > 512u || !user_range_ok(a2, a3))
				return (unsigned int)-1;
			for (i = 0; i < a3; i++)
				kval[i] = ((const unsigned char *)a2)[i];
			return (unsigned int)credstore_set(kname, kval, a3);
		}
		if (a0 == 1u) {
			int n;
			/* a3 is the USER buffer size; the value
			 * itself is capped by kval (512). */
			if (a3 == 0u || a3 > 8192u ||
			    !user_range_ok(a2, a3))
				return (unsigned int)-1;
			n = credstore_get(kname, kval, sizeof(kval));
			if (n < 0 || (unsigned int)n > a3)
				return (unsigned int)-1;
			{
				char *u = (char *)a2;
				for (i = 0; i < (unsigned int)n; i++)
					u[i] = (char)kval[i];
			}
			return (unsigned int)n;
		}
		if (a0 == 2u)
			return (unsigned int)credstore_present(kname);
		if (a0 == 3u)
			return (unsigned int)credstore_clear(kname);
		return (unsigned int)-1;
	}
	case SYS_EVENT_INJECT: {
		/* Dev/test button injection: the same queue the
		 * GPIO buttons will feed on hardware. Needed in
		 * QEMU because sh owns the console exclusively
		 * (Q9), so translated console keys never reach
		 * the display holder's event poll. a0 = EV_*. */
		if (a0 > 6u)
			return (unsigned int)-1;
		evq_push((int)a0);
		return 0;
	}
	case SYS_RESET: {
		/* FND-1: record the reason (bootstate reason 4)
		 * BEFORE acting, so the next boot's bootlog
		 * tells the truth. psci_system_reset() only
		 * returns if the platform refused. */
		bootstate_note_software_reset(
		    arm_timer_ticks() * 10u, 0);
		printf("sys: SYS_RESET (software request)\n");
		(void)psci_system_reset();
		return (unsigned int)-1;
	}
	case SYS_POWEROFF: {
		/* FND-1: same contract as SYS_RESET, PSCI
		 * SYSTEM_OFF instead of SYSTEM_RESET. */
		bootstate_note_software_reset(
		    arm_timer_ticks() * 10u, 1);
		printf("sys: SYS_POWEROFF (software request)\n");
		(void)psci_system_off();
		return (unsigned int)-1;
	}
	case SYS_BOOTSTATE: {
		if (a0 == 0u) {
			unsigned int *u = (unsigned int *)a1;
			if (!user_range_ok(a1, 16u))
				return (unsigned int)-1;
			bootstate_info(&u[0], &u[1], &u[2], &u[3]);
			return 0u;
		}
		if (a0 == 1u) {
			bootstate_healthy(arm_timer_ticks() * 10u);
			return 0u;
		}
		if (a0 == 2u) {
			bootstate_crash_sim();
			return 0u;
		}
		if (a0 == 3u)
			return bootstate_flags();
		if (a0 == 4u) {
			bootstate_set_provisioned(1);
			return 0u;
		}
		if (a0 == 5u) {
			bootstate_set_provisioned(0);
			return 0u;
		}
		if (a0 == 6u) {
			unsigned int *u = (unsigned int *)a1;
			if (!user_range_ok(a1, 16u))
				return (unsigned int)-1;
			bootstate_update_info(&u[0], &u[1], &u[2], &u[3]);
			return 0u;
		}
		if (a0 == 7u)
			return (unsigned int)bootstate_update_stage(a1);
		if (a0 == 8u) {
			bootstate_sabotage_arm();
			return 0u;
		}
		if (a0 == 9u) {
			/* B5.3: stage a SIGNED package. a1=buf,
			 * a2=len -> 0 staged, -1 busy, -2 downgrade,
			 * -3 bad signature/format. The package is
			 * copied to a static buffer and verified
			 * BEFORE anything is staged: a bad package
			 * never reaches a trial boot. (Production
			 * streams the hash over the inactive NAND
			 * slot; the QEMU cap is 8192 bytes.) */
			static unsigned char pkgbuf[8192];
			unsigned int version = 0;
			unsigned int i;

			if (a2 == 0u || a2 > 8192u ||
			    !user_range_ok(a1, a2))
				return (unsigned int)-3;
			for (i = 0; i < a2; i++)
				pkgbuf[i] = ((const unsigned char *)a1)[i];
			if (upd_verify_package(pkgbuf, a2, &version) != 0)
				return (unsigned int)-3;
			return (unsigned int)
			    bootstate_update_stage(version);
		}
		if (a0 == 10u)
			return (unsigned int)bootstate_update_cancel();
		if (a0 == 11u) {
			/* Boot UX: set/clear user (GUI) mode. */
			bootstate_set_gui_mode(a1);
			return 0u;
		}
		return (unsigned int)-1;
	}
	case SYS_TIME_QUALITY:
		/* B3: 1 only if the NTP daemon marked the clock. */
		return time_synced ? 1u : 0u;
	case SYS_TIME_SYNC:
		/* a0=1: mark synced (NTP daemon). a0=0: clear (tests). */
		time_synced = a0 ? 1 : 0;
		return 0;
	case SYS_BOOTLOG: {
		if (a0 == 0xffffffffu)
			return bootstate_log_count();
		if (!user_range_ok(a1, 64u))
			return (unsigned int)-1;
		return (unsigned int)bootstate_log_get(a0, (void *)a1);
	}
	case SYS_LFS_WRITE: {
		static char kpath[128];
		if (copy_path_user(a0, kpath, sizeof(kpath)) != 0 ||
		    !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return (unsigned int)lfs_port_write(kpath,
		    (const unsigned char *)a1, a2);
	}
	case SYS_SEND: {
		/* IPC-1: a0=dest name a1=buf a2=len a3=flags.
		 * user_range_ok is window-based, so an
		 * oversize probe length still validates here
		 * and the glue answers its documented -6.
		 * IPC-3: on a successful reply-carrying
		 * COPY_SEND (not a send-once reply), the
		 * pending peer is (re)set by rpc_peer_note
		 * (shared with the SYS_RPC engine): the old
		 * ref is released first, a ref on the
		 * destination object is taken and stored.
		 * A failed send stages nothing and touches
		 * nothing; a no-reply send leaves a pending
		 * peer untouched for a later park on its
		 * reply port. v1 tracks only the most recent
		 * outstanding request: parking on an older
		 * reply port after a newer request is
		 * unannotated (its deadline still bounds
		 * it). */
		void *dest_obj = (void *)0;
		unsigned int reply = 0u, rc;

		if (cur_utask == TASK_NULL || !user_range_ok(a1, a2))
			return (unsigned int)-1;
		rc = ipc_glue_send_ex(cur_utask, a0,
				      (const void *)a1, a2, a3,
				      &dest_obj, &reply);
		if (rc == 0u)
			rpc_peer_note(dest_obj, reply, a3);
		return rc;
	}
	case SYS_RPC:
		/* IPC-4: a0=dest name a1=buf a2=len
		 * a3=timeout_ms (SYS_RECV encoding). The
		 * request header's reply field names the
		 * reply port the caller holds (0 = the
		 * caller used the uipc_rpc wrapper, which
		 * always sets one; a raw 0 here is a
		 * no-reply request whose wait can only end
		 * by deadline -- the engine treats it as
		 * EINVAL, since an RPC without a reply
		 * port is a caller bug, not a wait). */
		if (cur_utask == TASK_NULL || !user_range_ok(a1, a2))
			return (unsigned int)-1;
		return sys_rpc_call(frame, a0, a1, a2, (int)a3);
	case SYS_RPC_USER: {
		/* IPC-4: a0=name string, a1=buf, a2=len,
		 * a3=timeout_ms. Registry lookup first,
		 * then the SYS_RPC engine. */
		static char kname[16];
		unsigned int dest;

		if (cur_utask == TASK_NULL || !user_range_ok(a1, a2))
			return (unsigned int)-1;
		if (copy_path_user(a0, kname, sizeof(kname)) != 0)
			return (unsigned int)-1;
		dest = ipc_glue_registry(cur_utask, cur_progid,
					 IPCP_LOOKUP, kname, 0u);
		if ((int)dest < 0)
			return dest;
		return sys_rpc_call(frame, dest, a1, a2, (int)a3);
	}
	case SYS_RECV:
		/* IPC-1: a0=port name a1=buf a2=max, a3=0
		 * (poll). IPC-2: a3 != 0 is the blocking
		 * form -- a3 > 0 a deadline in ms, a3 = -1
		 * no deadline (bootlogged; see
		 * sys_recv_block). One-shot callers (no
		 * scheduler yet) always get poll semantics:
		 * there is nobody to switch to. */
		if (cur_utask == TASK_NULL || !user_range_ok(a1, a2))
			return (unsigned int)-1;
		if (a3 == 0u || !sched_active || sched_cur < 0)
			return ipc_glue_recv(cur_utask, a0,
					     (void *)a1, a2);
		return sys_recv_block(frame, a0, a1, a2, (int)a3);
	case SYS_IPC_PORT: {
		/* IPC-1: a0=sub. Registry subs (>= 3) take
		 * the name string in a1 and the port name
		 * in a2; the plain subs take name in a1,
		 * qlimit in a2. The caller's progid is the
		 * kernel-assigned spawn identity (see
		 * ipc_glue.h); it is passed, never trusted
		 * from the caller. */
		static char kname[16];
		if (a0 >= IPCP_REGISTER) {
			if (copy_path_user(a1, kname, sizeof(kname)) != 0)
				return (unsigned int)-1;
			return ipc_glue_port(cur_utask, cur_progid,
					     a0, kname, a2, 0u);
		}
		return ipc_glue_port(cur_utask, cur_progid, a0,
				     (const char *)0, a1, a2);
	}
	default:
		return (unsigned int)-2;	/* ENOSYS */
	}
}

/*
 * Scheduler kooperatif App A2 (implementasi).
 *
 * Semua daemon berjalan di atas boot thread: trap SVC tidak pernah
 * kembali ke user_launch_init sampai semua daemon mati.  Setiap
 * SYS_YIELD menyimpan konteks user penuh (r0-r12, banked sp/lr USR,
 * pc, spsr) lalu memuat milik daemon berikutnya + ganti pmap.
 */
static void
ctx_save(struct user_ctx *c, struct arm_trap_frame *f)
{
	unsigned int i;
	for (i = 0; i < 13u; i++)
		c->r[i] = f->r[i];
	c->pc = f->lr;
	c->spsr = f->spsr;
	ctx_save_usr(&c->sp_usr, &c->lr_usr);
	c->r[0] = 0;	/* SYS_YIELD mengembalikan 0 saat thread kembali */
}

static void
ctx_load(struct arm_trap_frame *f, struct user_ctx *c)
{
	unsigned int i;
	for (i = 0; i < 13u; i++)
		f->r[i] = c->r[i];
	f->lr = c->pc;
	f->spsr = c->spsr;
	ctx_restore_usr(c->sp_usr, c->lr_usr);
}

static struct daemon *
pick_next(void)
{
	unsigned int i;
	for (i = 1; i <= NDAEMON; i++) {
		unsigned int j = (unsigned int)(sched_cur + (int)i) % NDAEMON;
		if (daemons[j].state == 1)	/* Q9: hanya runnable, bukan WAITING */
			return &daemons[j];
	}
	return 0;
}

static void	gui_maybe_launch(void);

static void
sched_activate(struct daemon *d)
{
	cur_utask = d->task;
	cur_udesc = d->udesc;
	cur_progid = d->progid;
	arm_pmap_activate_user(d->pmap);
}

/*
 * IPC-2: blocking receive -- the third scheduler switch
 * point (D-IPC-1; plan: docs/rencana-ipc, as-built:
 * DEVLOG-IPC2.md).
 *
 * State 2 (WAITING) is now general: a daemon is in it
 * either as a spawn parent (Q9: spawn_done collects it)
 * or as an IPC waiter (ipc_wait_active set; the wait
 * record in struct daemon collects it). wd_supervise()
 * only ever judges state 1, so BOTH kinds of waiter are
 * exempt from hang verdicts while parked -- the
 * spawn-parent rule of FND-3, generalised.
 *
 * A wait completes in exactly three ways:
 *  (1) a send stages a message on the port -- the glue
 *      calls sched_ipc_wake_port() with the destination
 *      port OBJECT; matching waiters become runnable;
 *  (2) the deadline expires -- ipc_wait_expire() runs
 *      in every switch prologue and once per tick in
 *      the all-parked idle loop, and makes the waiter
 *      runnable;
 *  (3) never, for the no-deadline (-1) form -- allowed,
 *      but bootlogged (event 17) so a parked-forever
 *      daemon is visible in post-mortem, per D-IPC-1's
 *      "never an *unobserved* infinite park". IPC-3
 *      closes the remaining strand: a wait annotated
 *      with a peer (see the IPC-3 block below) also
 *      completes when that peer's port is destroyed
 *      (daemon death, or a live owner's DESTROY),
 *      with UIPC_EPEER -- so no park is ever stranded
 *      by a dead peer, with or without a deadline.
 * Wakes only flip state + stamp wd_last_progress
 * (D-IPC-1: the wake restarts the progress clock, the
 * Q9 spawn-wake precedent); the wait record stays open
 * until the woken daemon re-enters SYS_RECV, where the
 * verdict is decided in ITS OWN context: poll first (a
 * queued message beats an expired deadline), then the
 * deadline, then park again. That keeps every copyout
 * in the receiver's own pmap -- no completion work is
 * ever done in a waker's context.
 *
 * The resume channel: ctx_save_wait() parks the waiter
 * with r0 = UIPC_EPARK in its saved ctx. Whichever
 * switch later resumes it loads that ctx and the
 * switch's caller returns sched_resume_ret, which
 * trap.c writes over the frame's r0 -- so the waiter's
 * SYS_RECV returns UIPC_EPARK and the ulib wrapper
 * re-issues the call, which resolves the wait as
 * above. No pc is ever rewound and no syscall is
 * re-executed by the kernel.
 */
#define	IPC_EV_PARK_NODEADLINE	17u	/* bootlog: ipc park, no deadline */

static void
ctx_save_wait(struct user_ctx *c, struct arm_trap_frame *f)
{
	unsigned int i;
	for (i = 0; i < 13u; i++)
		c->r[i] = f->r[i];
	c->pc = f->lr;
	c->spsr = f->spsr;
	ctx_save_usr(&c->sp_usr, &c->lr_usr);
	c->r[0] = (unsigned int)UIPC_EPARK; /* resume verdict */
}

static void
ipc_wait_wake_idx(unsigned int idx)
{
	struct daemon *d = &daemons[idx];

	if (d->state != 2 || !d->ipc_wait_active)
		return;
	d->state = 1;	/* runnable again */
	/* D-IPC-1: a wake is progress. */
	d->wd_last_progress = arm_timer_ticks();
}

/* Called by the glue (ipc_glue.c) after a message has
 * been staged: wake every waiter parked on that port
 * object (at most one task can hold a receive right,
 * but the scan does not rely on it). */
void
sched_ipc_wake_port(void *port_obj)
{
	unsigned int i;

	if (port_obj == (void *)0)
		return;
	for (i = 0; i < NDAEMON; i++)
		if (daemons[i].ipc_wait_active &&
		    daemons[i].ipc_wait_port == port_obj)
			ipc_wait_wake_idx(i);
}

/*
 * IPC-3: peer annotation + peer-death wake.
 *
 * A waiter parks on its OWN receive port, so "the
 * port whose owner dies" can only mean the RPC
 * counterparty: a client parked on its reply port
 * waiting for a server that dies. Plain own-port
 * waits need no completion (the waiter's own death
 * is impossible while it waits -- it is not running).
 * The annotation is therefore (reply port NAME,
 * peer port OBJECT + held ref), recorded at send
 * time and adopted at park-open only for the exact
 * RPC shape (see sys_recv_block). The held ref is
 * what makes every dereference below safe: the
 * object cannot be freed while the waiter holds it
 * (destruction clears ACTIVE but does not free
 * while refs remain).
 *
 * Refs are released at wait resolution (message,
 * deadline, peer-died), at pending replacement, and
 * at slot cleanup in daemon_reclaim -- all three
 * paths must hold or refs leak.
 */
static void
ipc_peer_release(struct daemon *d)
{
	if (d->ipc_wait_peer_obj != (void *)0) {
		ipc_glue_release_ref(d->ipc_wait_peer_obj);
		d->ipc_wait_peer_obj = (void *)0;
	}
	d->ipc_wait_peer_died = 0;
}

static void
ipc_peer_clear_slot(struct daemon *d)
{
	if (d->ipc_peer_next_obj != (void *)0) {
		ipc_glue_release_ref(d->ipc_peer_next_obj);
		d->ipc_peer_next_obj = (void *)0;
	}
	d->ipc_peer_next_reply = 0u;
	ipc_peer_release(d);
	d->ipc_wait_active = 0;
	d->ipc_wait_port = (void *)0;
	d->ipc_wait_deadline = 0u;
}

/* Called by the glue (ipc_glue.c) after a port OBJECT
 * has been destroyed (teardown pass 1, or a live
 * owner's DESTROY): every waiter peer-annotated with
 * that object is woken with the peer-died flag. The
 * verdict itself is decided in the waiter's own
 * context on re-dispatch (peer-died beats deadline,
 * queued message beats both -- see sys_recv_block),
 * so no copyout ever happens in a waker's context
 * (the IPC-2 rule, preserved). */
void
sched_ipc_wake_peer(void *port_obj)
{
	unsigned int i;

	if (port_obj == (void *)0)
		return;
	for (i = 0; i < NDAEMON; i++) {
		struct daemon *d = &daemons[i];

		if (d->ipc_wait_active &&
		    d->ipc_wait_peer_obj == port_obj) {
			d->ipc_wait_peer_died = 1;
			ipc_wait_wake_idx(i);
		}
	}
}

static void
ipc_wait_expire(unsigned int now)
{
	unsigned int i;

	for (i = 0; i < NDAEMON; i++) {
		struct daemon *d = &daemons[i];

		if (d->ipc_wait_active && d->ipc_wait_deadline != 0u &&
		    now >= d->ipc_wait_deadline)
			ipc_wait_wake_idx(i);
	}
}

static void
ipc_park_log_nodeadline(struct daemon *d)
{
	char note[40];
	unsigned int i = 0, j;
	const char *pre = "ipc park-nodeadline ";

	for (j = 0; pre[j] && i < sizeof(note) - 1u; j++)
		note[i++] = pre[j];
	for (j = 0; d->name[j] && i < sizeof(note) - 1u; j++)
		note[i++] = d->name[j];
	note[i] = 0;
	bootstate_log_event(IPC_EV_PARK_NODEADLINE,
			    arm_timer_ticks() * 10u, note);
	printf("ipc: '%s' parked with no deadline (bootlogged ev=17)\n",
	       d->name);
}

/*
 * The wait switch itself. Parks the current daemon
 * (already recorded as an IPC waiter by sys_recv_block)
 * and resumes the next runnable daemon. Returns 1 when
 * the CURRENT daemon can continue in place -- its own
 * deadline expired during the prologue, or the
 * all-parked idle loop ended with the current daemon
 * runnable again -- and 0 when the frame now belongs to
 * another daemon (sched_resume_ret set for the caller).
 *
 * All-parked idle: nobody is runnable, so instead of
 * returning to a frame, spin here one timer tick at a
 * time. SVC trap context in this port runs with IRQs
 * MASKED (CPSR.I=1 -- measured: cpsr=0x...93 in the
 * trap, spsr=0x...10 in user mode), so the tick
 * counter (incremented in the IRQ handler) is frozen
 * while a trap handler runs; the idle therefore
 * unmasks IRQs for the raw tick wait itself and
 * re-masks before doing any other kernel work, so
 * every other line of this function runs in exactly
 * the environment the other switch prologues run in.
 * The nested timer IRQs are safe: they take their own
 * frame on the SVC stack, and wd_irq_tick() only ever
 * judges USER-mode frames ("kernel/WFI stretches are
 * not hangs"), so class (b) cannot fire from here;
 * stamping wd_last_activity per tick keeps its
 * precondition false regardless. Per tick: pump the
 * network (a TCP arrival can be the event a waiter
 * indirectly needs, and HTTP stays alive) and expire
 * deadlines. The first wake -- a deadline here, since
 * no userspace runs to send -- ends the idle.
 */
static int
sched_ipc_wait_switch(struct arm_trap_frame *f)
{
	struct daemon *cur, *next;

	sched_resume_ret = 0;
	wd_last_activity = arm_timer_ticks();	/* FND-3 */
	net_pump();
	gui_maybe_launch();	/* Boot UX: deferred GUI launch */
	cur = &daemons[sched_cur];
	ctx_save_wait(&cur->ctx, f);
	cur->state = 2;	/* WAITING (IPC) */
	ipc_wait_expire(arm_timer_ticks());
#if WD_ENABLED
	(void)wd_supervise();
#endif
	next = pick_next();
	if (next == cur)
		return 1;	/* own deadline already due */
	if (next != 0) {
		sched_cur = (int)(next - daemons);
		sched_activate(next);
		ctx_load(f, &next->ctx);
		sched_resume_ret = next->ctx.r[0];
		return 0;	/* switched away */
	}
	for (;;) {
		unsigned int t0;

		/* One tick with IRQs live (see the block
		 * comment): the ONLY unmasked stretch in
		 * the switch, bracketing a pure wait. */
		__asm__ volatile ("cpsie i" ::: "memory");
		t0 = arm_timer_ticks();
		while (arm_timer_ticks() == t0) {
			/* spin one tick */
		}
		__asm__ volatile ("cpsid i" ::: "memory");
		wd_last_activity = arm_timer_ticks();
		net_pump();
		ipc_wait_expire(arm_timer_ticks());
		next = pick_next();
		if (next == 0)
			continue;
		if (next == cur)
			return 1;	/* completed in place */
		sched_cur = (int)(next - daemons);
		sched_activate(next);
		ctx_load(f, &next->ctx);
		sched_resume_ret = next->ctx.r[0];
		return 0;	/* switched away */
	}
}

/*
 * SYS_RECV, blocking form (a3 != 0). The poll always
 * runs first: a message already queued is delivered
 * without parking, and a message that raced a deadline
 * beats it. Only the empty-queue path opens the wait
 * record and parks. Re-dispatch after a wake keeps the
 * ORIGINAL record (and deadline): the wait is one
 * logical operation however many parks it took.
 */
static unsigned int
sys_recv_block(struct arm_trap_frame *frame, unsigned int port,
	       unsigned int ubuf, unsigned int max, int timeout_ms)
{
	struct daemon *d = &daemons[sched_cur];

	for (;;) {
		void *obj = (void *)0;
		unsigned int rc = ipc_glue_recv_ex(cur_utask, port,
		    (void *)ubuf, max, &obj);
		unsigned int now;

		if (rc != (unsigned int)UIPC_EEMPTY) {
			/* Message delivered (or a hard error):
			 * the wait resolves; the peer ref ends
			 * with it. Poll-first is what makes a
			 * message racing a wake beat the wake. */
			ipc_peer_release(d);
			d->ipc_wait_active = 0;
			return rc;
		}
		now = arm_timer_ticks();
		if (d->ipc_wait_active && d->ipc_wait_port == obj) {
			/* Re-dispatch of an open wait. Still
			 * empty: verdict order is peer-died
			 * first (if both happened, EPEER is the
			 * truthful verdict -- the wait can never
			 * succeed), then deadline, else park
			 * again under the same record. */
			if (d->ipc_wait_peer_died) {
				ipc_peer_release(d);
				d->ipc_wait_active = 0;
				return (unsigned int)UIPC_EPEER;
			}
			if (d->ipc_wait_deadline != 0u &&
			    now >= d->ipc_wait_deadline) {
				ipc_peer_release(d);
				d->ipc_wait_active = 0;
				return (unsigned int)UIPC_ETIME;
			}
		} else {
			/* Open the wait record. Adopt the
			 * pending peer ONLY for the exact RPC
			 * shape: the parked port name is the
			 * pending reply name (generation bits
			 * included -- a stale name never
			 * matches). Parking on any other port
			 * leaves the pending peer untouched
			 * for a later park on the reply port.
			 * Park-open liveness: if the peer is
			 * already dead, return EPEER without
			 * parking -- otherwise a no-deadline
			 * wait on a dead peer would strand
			 * forever. */
			d->ipc_wait_active = 1;
			d->ipc_wait_port = obj;
			d->ipc_wait_peer_died = 0;
			if (d->ipc_peer_next_obj != (void *)0 &&
			    port == d->ipc_peer_next_reply) {
				d->ipc_wait_peer_obj =
				    d->ipc_peer_next_obj;
				d->ipc_peer_next_obj = (void *)0;
				d->ipc_peer_next_reply = 0u;
				if (!ipc_glue_port_alive(
					d->ipc_wait_peer_obj)) {
					ipc_peer_release(d);
					d->ipc_wait_active = 0;
					return (unsigned int)UIPC_EPEER;
				}
			} else
				d->ipc_wait_peer_obj = (void *)0;
			if (timeout_ms < 0) {
				d->ipc_wait_deadline = 0u;
				ipc_park_log_nodeadline(d);
			} else {
				/* 100 Hz ticks, rounded up: a
				 * deadline never fires early. */
				unsigned int ticks =
				    ((unsigned int)timeout_ms + 9u) / 10u;
				if (ticks == 0u)
					ticks = 1u;
				d->ipc_wait_deadline = now + ticks;
			}
		}
		if (sched_ipc_wait_switch(frame))
			continue;	/* completed in place */
		return sched_resume_ret;	/* switched away */
	}
}

/* IPC-4 RPC engine (SYS_RPC / SYS_RPC_USER share it;
 * the USER form resolves the name first, then calls
 * here with the send name). The request is in ubuf
 * with hdr.reply naming a reply port the caller holds
 * (the uipc_rpc wrapper always sets one; a raw call
 * with reply 0 is EINVAL -- an RPC without a reply
 * port is a caller bug, not a wait). Steps:
 *   1. send via the glue (COPY_SEND, never
 *      SEND_ONCE for a request); on success the peer
 *      note is set exactly as SYS_SEND sets it, so
 *      the IPC-3 EPEER path covers a server that
 *      dies mid-RPC.
 *   2. one-shot callers (no scheduler) get a single
 *      poll: EEMPTY if no reply yet.
 *   3. daemon callers wait with sys_recv_block on
 *      the reply port, same buffer, same timeout
 *      encoding (0 = poll, >0 = deadline ms, -1 =
 *      no deadline). If the park switches away, the
 *      syscall returns UIPC_EPARK and the ulib
 *      wrapper continues the wait with a plain
 *      uipc_recv on the reply port -- it NEVER
 *      re-issues SYS_RPC, which would duplicate the
 *      request. The wait record's original deadline
 *      governs the re-dispatch, so continuing with
 *      the original timeout is safe.
 * A reply larger than the request buffer returns
 * UIPC_E2BIG and the message stays queued (Mach
 * semantics); the wrapper destroys its reply port
 * on E2BIG, discarding that queued reply. */
static unsigned int
sys_rpc_call(struct arm_trap_frame *frame, unsigned int dest,
	     unsigned int ubuf, unsigned int len, int timeout_ms)
{
	void *dest_obj = (void *)0;
	unsigned int reply = 0u, rc;
	unsigned int hdr_reply;

	if (len < 24u)
		return (unsigned int)UIPC_EINVAL;
	hdr_reply = ((const unsigned int *)ubuf)[0];
	if (hdr_reply == 0u)
		return (unsigned int)UIPC_EINVAL;
	rc = ipc_glue_send_ex(cur_utask, dest, (const void *)ubuf,
			      len, 0u, &dest_obj, &reply);
	if (rc != 0u)
		return rc;
	rpc_peer_note(dest_obj, reply, 0u);
	if (!sched_active || sched_cur < 0)
		return ipc_glue_recv(cur_utask, reply, (void *)ubuf,
				     len);
	return sys_recv_block(frame, reply, ubuf, len, timeout_ms);
}

static void
sched_yield_switch(struct arm_trap_frame *f)
{
	struct daemon *cur, *next;
	/* Pump network sekali tiap yield: HTTP tetap hidup di sela
	 * daemon.  Driver net polling murni (tanpa IRQ) jadi aman
	 * dari konteks trap SVC. */
	sched_resume_ret = 0;	/* IPC-2 */
	wd_last_activity = arm_timer_ticks();	/* FND-3 */
	net_pump();
	gui_maybe_launch();	/* Boot UX: deferred GUI launch */
	/* IPC-2: a parked waiter's deadline may have come
	 * due since the last switch; expiring it here (and
	 * in the other switch prologues) is what guarantees
	 * a parked daemon is re-examined at least once per
	 * switch anywhere in the system. The wake stamps
	 * wd_last_progress, so the supervisor below never
	 * judges a just-expired waiter on its park age. */
	ipc_wait_expire(arm_timer_ticks());
	cur = &daemons[sched_cur];
	ctx_save(&cur->ctx, f);
#if WD_ENABLED
	if (wd_supervise()) {
		/* FND-3: the supervisor killed + respawned the
		 * CURRENT daemon (it yielded but never
		 * progressed): ctx_save stored the dead
		 * instance's ctx and wd_respawn replaced it
		 * with a fresh one -- resume the new instance
		 * directly instead of the saved ctx. */
		sched_activate(cur);
		ctx_load(f, &cur->ctx);
		sched_resume_ret = cur->ctx.r[0];	/* IPC-2 */
		return;
	}
#endif
	next = pick_next();
	if (next == cur || next == 0)
		return;	/* sendirian: frame tak diubah, r0=0 via caller */
	sched_cur = (int)(next - daemons);
	sched_activate(next);
	ctx_load(f, &next->ctx);
	sched_resume_ret = next->ctx.r[0];	/* IPC-2 */
}

/*
 * FND-2: daemon_reclaim -- hand back everything a dying
 * daemon held, so a daemon death cannot wedge the system
 * until the next power cycle ("screen/console dead until
 * power cycle" was a whole field-failure class). Called
 * from sched_exit_switch(), the single funnel for BOTH
 * death routes (clean SYS_EXIT and user_fault isolation),
 * before the slot is marked dead. Keyed by daemon index;
 * display ownership is progid-keyed, console and TCP
 * ownership are index-keyed. Full state inventory:
 * docs/rencana-fondasi-tanpa-board/DEVLOG-FND2.md.
 */
#define	IPC_EV_DEAD_NOTE	18u	/* bootlog: ipc port teardown */

static void
ipc_dead_note(struct daemon *d, unsigned int ports,
	      unsigned int msgs)
{
	char note[40];
	unsigned int i = 0, j;
	const char *pre = "ipc dead ";

	for (j = 0; pre[j] && i < sizeof(note) - 1u; j++)
		note[i++] = pre[j];
	for (j = 0; d->name[j] && i < sizeof(note) - 1u; j++)
		note[i++] = d->name[j];
	if (i < sizeof(note) - 1u)
		note[i++] = ' ';
	/* Ports, decimal (no %u into a note string). */
	{
		char rev[10];
		unsigned int n = 0u, v = ports;

		do {
			rev[n++] = (char)('0' + (int)(v % 10u));
			v /= 10u;
		} while (v != 0u && n < sizeof(rev));
		while (n > 0u && i < sizeof(note) - 1u)
			note[i++] = rev[--n];
	}
	if (i < sizeof(note) - 2u) {
		note[i++] = 'p';
		note[i++] = ' ';
	}
	{
		char rev[10];
		unsigned int n = 0u, v = msgs;

		do {
			rev[n++] = (char)('0' + (int)(v % 10u));
			v /= 10u;
		} while (v != 0u && n < sizeof(rev));
		while (n > 0u && i < sizeof(note) - 1u)
			note[i++] = rev[--n];
	}
	if (i < sizeof(note) - 1u)
		note[i++] = 'm';
	note[i] = 0;
	bootstate_log_event(IPC_EV_DEAD_NOTE,
			    arm_timer_ticks() * 10u, note);
}

static void
daemon_reclaim(int idx)
{
	struct daemon *d = &daemons[idx];

	/* IPC-3: lifecycle. FIRST, before anything that
	 * could wake waiters: the dying daemon's own
	 * peer refs and wait record are released and
	 * cleared, so it cannot be woken by its own
	 * port deaths (the woken-but-not-redispatched
	 * case: state 1 with the record still open).
	 * THEN the space is torn down: every receive
	 * port destroyed (queues discarded, other
	 * waiters annotated with those ports woken
	 * peer-died), then every remaining right.
	 * Reclaim runs BEFORE the slot is marked dead
	 * in both sched_exit_switch and wd_supervise,
	 * and BEFORE wd_respawn replaces d->task, so
	 * teardown always sees the dying task and the
	 * waiter completion lands before/while the
	 * respawn lands. A respawned incarnation gets
	 * a fresh task (hence a fresh space) from
	 * setup_uprog_task, so it starts with a clean
	 * port table by construction. The rendezvous
	 * table is deliberately NOT touched: slots are
	 * owned by the table, not the publisher (a
	 * publish-then-exit flow requires slot survival
	 * across the publisher's death); the
	 * respawn-republish case is IPC-4's registry
	 * problem. */
	{
		unsigned int ports = 0u, msgs = 0u;

		ipc_peer_clear_slot(d);
		if (d->task != TASK_NULL)
			ipc_glue_teardown(d->task, &ports, &msgs);
		if (ports != 0u || msgs != 0u) {
			ipc_dead_note(d, ports, msgs);
			printf("reclaim: '%s' ipc teardown %up "
			    "%um\n", d->name, ports, msgs);
		}
	}

	/* Display token. */
	if (d->progid >= 0 && d->progid == dpy_holder) {
		if (d->progid == PROG_UIAPP) {
			/* Mirror SYS_DISPLAY_RELEASE: token home,
			 * queued input dropped. A pending sleep
			 * request is NOT cleared: the Sleep Now
			 * flow sets it immediately before release,
			 * so a holder dying in between must not
			 * lose the user's request (same rule as
			 * the RELEASE path). */
			dpy_holder = PROG_FACE;
			evq_clear();
			printf("reclaim: '%s' died holding display "
			    "-> token back to face\n", d->name);
		} else {
			/* The face itself died holding the token.
			 * The token is PROG_FACE-keyed by design
			 * and already home; what must not survive
			 * is state aimed AT the face: a pending
			 * sleep request (its only consumer is
			 * gone) and queued input typed for a dead
			 * reader. A future face instance starts
			 * clean. */
			dpy_sleep_req = 0;
			evq_clear();
			if (face_req_pending) {
				face_req_pending = 0;
				face_req_owner = -1;
			}
			printf("reclaim: face died holding display "
			    "-> sleep/queue/face-request state cleared\n");
		}
	}

	/* K4: a pending face request must not survive ANY face
	 * death for the next incarnation to consume: the
	 * request was aimed at a dead instance (face consumes
	 * only while it holds the token, and a respawned face
	 * starts clean). The holder branch above clears it
	 * only when face held the token; this covers the
	 * face-died-while-uiapp-holds case too. The request
	 * is one-way polled (no waiter blocks on it), so the
	 * requester re-posts if still relevant. */
	if (d->progid == PROG_FACE && face_req_pending) {
		face_req_pending = 0;
		face_req_owner = -1;
		printf("reclaim: dropped pending face request "
		    "(face died)\n");
	}

	/* A pending face request posted BY the dying daemon:
	 * its author is gone; the face must not act on it. */
	if (face_req_pending && face_req_owner == idx) {
		face_req_pending = 0;
		face_req_owner = -1;
		printf("reclaim: dropped face request from dead '%s'\n",
		    d->name);
	}

	/* Console ownership: back to the boot default (anyone
	 * may read). The web-shell executor IS the console
	 * owner (sh is the only SYS_WEBUI taker), so its death
	 * also settles the webui bridge: a queued or running
	 * command must not wedge the bridge forever. */
	if (console_owner == idx) {
		console_owner = -1;
		printf("reclaim: '%s' died owning console "
		    "-> console open\n", d->name);
		webui_shell_executor_died();
	}

	/* The single TCP client connection, if this daemon
	 * opened it: close gracefully (FIN). A connect still
	 * in SYN_SENT is left to the existing client timeout. */
	if (tcp_client_owner == idx) {
		tcp_client_owner = -1;
		tcp_client_close();
		printf("reclaim: closed TCP client connection of "
		    "dead '%s'\n", d->name);
	}
}

static void
sched_exit_switch(struct arm_trap_frame *f, unsigned int code)
{
	struct daemon *next;
	sched_resume_ret = 0;	/* IPC-2 */
	net_pump();
	ipc_wait_expire(arm_timer_ticks());	/* IPC-2 prologue */
	daemon_reclaim(sched_cur);	/* FND-2 */
	printf("sched: '%s' exit 0x%x\n", daemons[sched_cur].name, code);
	/* Q9: bila yang exit adalah child spawn, bangunkan parent. */
	if (sched_cur == (int)(NDAEMON - 1) && spawn_parent != -1) {
		spawn_exitcode = code;
		spawn_done = 1;
		daemons[sched_cur].state = 0;
		daemons[spawn_parent].state = 1; /* parent runnable lagi */
		/* FND-3: the parent was WAITING, not hung: restart
		 * its progress clock. A child that exits on its
		 * own also clears the consecutive-kill counter. */
		daemons[spawn_parent].wd_last_progress =
		    arm_timer_ticks();
		wd_child_kills = 0;
		printf("spawn: child exit 0x%x, parent %d dibangunkan\n",
		       code, spawn_parent);
	} else {
		daemons[sched_cur].state = 0;
	}
	uprog_states[daemons[sched_cur].uprog_idx] = 2u;	/* EXITED */
	wd_last_activity = arm_timer_ticks();	/* FND-3 */
#if WD_ENABLED
	/* K4: face death-respawn. FND-3 respawned only HUNG
	 * daemons (the supervisor scans runnable slots); a
	 * face that EXITS or FAULTS stayed dead forever --
	 * and face is the only display granter, so a dead
	 * face wedges the whole GUI. Respawn a dead face
	 * under the same scheme: same 2-per-boot budget
	 * (shared with the hang path), same bootlog ev 15,
	 * same fatal rung on exhaustion (wd_fatal_reset
	 * converges to safe mode, never a reset loop).
	 * Scoped to face only (progid, never slot: slots
	 * follow registration order, progids don't); other
	 * daemons keep their exact FND-3 contract. The
	 * spawn child can never match (its progid is -1).
	 * The fresh face joins normal rotation via
	 * pick_next below -- no ctx surgery: the exiting
	 * frame must not resume the new instance. */
	if (daemons[sched_cur].progid == PROG_FACE) {
		struct daemon *fd = &daemons[sched_cur];
		char note[40];

		if (fd->wd_respawns >= WD_MAX_RESPAWN) {
			printf("watchdog: 'face' death-respawn "
			    "budget exhausted (%u)\n",
			    fd->wd_respawns);
			wd_fatal_reset("face");
			fd->wd_respawns = 0;	/* refused: reladder */
		} else {
			fd->wd_respawns++;
			if (wd_respawn(fd) != 0) {
				printf("watchdog: 'face' death-respawn "
				    "setup failed\n");
				wd_fatal_reset("face");
			} else {
				wd_note(note, sizeof(note), "respawn",
				    "face");
				bootstate_log_event(WD_EV_RESPAWN,
				    arm_timer_ticks() * 10u, note);
				printf("watchdog: 'face' death-respawned "
				    "(respawn %u)\n", fd->wd_respawns);
			}
		}
	}
	(void)wd_supervise();
#endif
	next = pick_next();
	if (next == 0) {
		/* Semua mati: trampoline terakhir. */
		f->r[0] = code;
		f->lr = (unsigned int)user_exit_trampoline;
		f->spsr = ARM_MODE_SVC;
		return;
	}
	sched_cur = (int)(next - daemons);
	sched_activate(next);
	ctx_load(f, &next->ctx);
	sched_resume_ret = next->ctx.r[0];	/* IPC-2 */
}

/* Daemon didaftarkan dari user_launch_init (setelah one-shot). */
static void
sched_register_daemon(const char *name, task_t task, pmap_t pmap,
		      struct user_task *udesc, unsigned int stack_top,
		      unsigned int progid)
{
	struct daemon *d;
	unsigned int i;
	if (ndaemon_reg >= NDAEMON) {
		printf("sched: FAIL (daemon table penuh)\n");
		return;
	}
	d = &daemons[ndaemon_reg];
	d->name = name;
	d->task = task;
	d->pmap = pmap;
	d->udesc = udesc;
	for (i = 0; i < 13u; i++)
		d->ctx.r[i] = 0;
	d->ctx.sp_usr = stack_top;
	d->ctx.lr_usr = 0;
	d->ctx.pc = INIT_CODE_VA;
	d->ctx.spsr = 0x10u;	/* USR, IRQ on */
	d->state = 1;
	d->progid = (int)progid;
	d->uprog_idx = nuprog;
	/* FND-3: supervisor registration. progid here is the
	 * daemon_images[] index (launch_daemon passes it), so
	 * it doubles as the respawn identity. Deadlines derive
	 * from the role (PLAN FND-3): GUI daemons poll events
	 * at FRAME_MS cadence and get 20 s; qabotd may sit in
	 * provider waits and gets 30 s; sh blocks on input --
	 * though it polls the console, so this is a pure
	 * backstop -- and gets 120 s. Warn at half the hang
	 * deadline. */
	d->wd_last_progress = arm_timer_ticks();
	d->wd_warned = 0;
	d->wd_hangs = 0;
	d->wd_respawns = 0;
	d->wd_imgidx = (int)progid;
	switch (progid) {
	case 0:	/* face */
	case 1:	/* uiapp */
		d->wd_warn_at = 1000u; d->wd_hang_at = 2000u; break;
	case 6:	/* sh (last daemon_images entry; see below) */
		d->wd_warn_at = 6000u; d->wd_hang_at = 12000u; break;
	default: /* qabotd + uipcld + linkd + ulink5d + any future daemon */
		d->wd_warn_at = 1500u; d->wd_hang_at = 3000u; break;
	}
	if (nuprog < UPROG_MAX) {
		uprog_names[nuprog] = name;
		uprog_states[nuprog] = 0u;	/* RUNNABLE */
		nuprog++;
	}
	ndaemon_reg++;
	printf("sched: daemon '%s' terdaftar (progid %u)\n", name, progid);
}

/* Loop utama daemon: masuk user mode; kembali hanya bila semua mati. */
void	machine_halt(void);

static void
sched_run(void)
{
	struct daemon *d;
	unsigned int rc;
	sched_active = 1;
	wd_last_activity = arm_timer_ticks();	/* FND-3 */
	d = pick_next();
	if (d == 0) {
		printf("sched: tidak ada daemon\n");
		return;
	}
	sched_cur = (int)(d - daemons);
	sched_activate(d);
	printf("sched: masuk user mode ('%s')...\n", d->name);
	rc = user_enter_test(d->ctx.sp_usr, d->ctx.pc);
	arm_pmap_activate_kernel();
	printf("sched: semua daemon mati (rc=0x%x)\n", rc);
	machine_halt();
}

/*
 * user_fault: a user-mode abort/undef becomes thread death, not a
 * kernel panic.  Redirect the trap frame to user_exit_trampoline with
 * a fault code in r0.
 */
void
user_fault(struct arm_trap_frame *frame, unsigned int code)
{
	/* App A2: fault di daemon = kematian thread itu saja, yang
	 * lain lanjut (crash isolation). */
	if (sched_active && sched_cur >= 0 && daemons[sched_cur].state) {
		printf("sched: '%s' FAULT 0x%x (diisolasi)\n",
		       daemons[sched_cur].name, code);
		sched_exit_switch(frame, code);
		return;
	}
	frame->r[0] = code;
	frame->lr = (unsigned int)user_exit_trampoline;
	frame->spsr = ARM_MODE_SVC;
}

/*
 * user_selftest
 */
void
user_selftest(void)
{
	task_t		utask;
	thread_t	uthread;
	pmap_t		upmap;
	kern_return_t	kr;
	vm_offset_t	mem, code_pa, stack_pa;
	unsigned int	code_len, rc, i;
	unsigned char	*dst;
	spl_t		s;

	printf("user_selftest: creating user task...\n");

	s = spl0();
	kr = task_create(kernel_task, FALSE, &utask);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_selftest: FAIL (task_create kr=%d)\n", kr);
		return;
	}
	/* A thread object in the task (scheduler dispatch is future work). */
	kr = thread_create(utask, &uthread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_selftest: FAIL (thread_create kr=%d)\n", kr);
		return;
	}
	upmap = utask->map->pmap;
	if (upmap == PMAP_NULL) {
		(void) splx(s);
		printf("user_selftest: FAIL (task has no pmap)\n");
		return;
	}

	/* Two physical pages (identity): code + user stack. */
	kr = kmem_alloc(kernel_map, &mem, 3 * USER_PGBYTES);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("user_selftest: FAIL (kmem_alloc kr=%d)\n", kr);
		return;
	}
	code_pa = trunc_page(mem);
	stack_pa = code_pa + USER_PGBYTES;
	(void) splx(s);

	/* ---- Phase A: clean write + exit ---- */
	code_len = (unsigned int)(_uprog_end - _uprog_start);
	dst = (unsigned char *)code_pa;
	for (i = 0; i < code_len; i++)
		dst[i] = _uprog_start[i];
	/* Fresh code: clean D-cache, invalidate I-cache before execute. */
	dcache_clean_range(code_pa, USER_PGBYTES);
	icache_invalidate_all();
	/* Zero the user stack. */
	dst = (unsigned char *)stack_pa;
	for (i = 0; i < USER_PGBYTES; i++)
		dst[i] = 0;

	pmap_enter(upmap, USER_CODE_VA, code_pa,
		   VM_PROT_READ | VM_PROT_WRITE, FALSE);
	pmap_enter(upmap, USER_STACK_VA, stack_pa,
		   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	printf("user_selftest: entering user mode (clean)...\n");
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(USER_STACK_VA + USER_PGBYTES, USER_CODE_VA);
	arm_pmap_activate_kernel();

	if (rc != 0) {
		printf("user_selftest: FAIL (clean exit code 0x%x)\n", rc);
		return;
	}
	printf("user_selftest: clean exit ok\n");

	/* ---- Phase B: fault isolation ---- */
	code_len = (unsigned int)(_uprog_end - _uprog_fault);
	dst = (unsigned char *)code_pa;
	for (i = 0; i < code_len; i++)
		dst[i] = _uprog_fault[i];
	dcache_clean_range(code_pa, USER_PGBYTES);
	icache_invalidate_all();

	printf("user_selftest: entering user mode (fault)...\n");
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(USER_STACK_VA + USER_PGBYTES, USER_CODE_VA);
	arm_pmap_activate_kernel();

	if (rc != USER_EXIT_DABT) {
		printf("user_selftest: FAIL (fault code 0x%x, want 0x%x)\n",
		       rc, USER_EXIT_DABT);
		return;
	}
	printf("user_selftest: PASS (user mode + syscall + fault isolation)\n");
}

/*
 * machine_halt -- Fase C: clean halt.  The CPU idles on wfi; the boot
 * log ends with the HALT marker for automated verification.
 */
void
machine_halt(void)
{
	printf("\nHALT: QaonicOS Fase C shutdown complete.\n");
	for (;;)
		__asm__ volatile ("wfi");
}

/*
 * setup_uprog_task -- App A2: bangun Mach task untuk satu program user
 * (task + thread + pmap + code/stack/heap) TANPA masuk user mode.
 * Dipakai launch_uprog (one-shot) dan launch_daemon (persisten).
 * Mengembalikan 0 bila sukses.
 */
static int
setup_uprog_task(const char *name, unsigned char *img, unsigned int img_len,
		 unsigned int stack_pages,
		 struct user_task *udesc, task_t *out_task, pmap_t *out_pmap,
		 unsigned int *out_stack_top)
{
	task_t		utask;
	thread_t	uthread;
	pmap_t		upmap;
	kern_return_t	kr;
	vm_offset_t	mem, pa, va;
	unsigned int	i, npages, ntotal;
	unsigned char	*dst;
	spl_t		s;
	unsigned int	stack_va, heap_va;

	/* Boot bersih: hanya tampil bila gagal. */
#if 0
	printf("setup_uprog_task: '%s'...\n", name);
#endif

	if (img_len == 0 || img_len > 256 * USER_PGBYTES) {	/* Q2b: utlscli butuh >512KB */
		printf("setup_uprog_task: FAIL (bad image size %u)\n", img_len);
		return -1;
	}

	s = spl0();
	kr = task_create(kernel_task, FALSE, &utask);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (task_create kr=%d)\n", kr);
		return -1;
	}
	kr = thread_create(utask, &uthread);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (thread_create kr=%d)\n", kr);
		return -1;
	}
	upmap = utask->map->pmap;
	if (upmap == PMAP_NULL) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (task has no pmap)\n");
		return -1;
	}

	/* code pages + stack pages + heap pages (contiguous, identity).
	 * Q2b: layout dinamis — stack diletakkan SETELAH code (bukan di
	 * alamat tetap), agar image besar (TLS ~80 page) tak menimpa stack. */
	if (stack_pages == 0)
		stack_pages = 1;
	npages = (img_len + USER_PGBYTES - 1) / USER_PGBYTES;
	ntotal = npages + stack_pages + INIT_HEAP_PAGES;
	kr = kmem_alloc(kernel_map, &mem, ntotal * USER_PGBYTES);
	if (kr != KERN_SUCCESS) {
		(void) splx(s);
		printf("setup_uprog_task: FAIL (kmem_alloc kr=%d)\n", kr);
		return -1;
	}
	(void) splx(s);

	/* code */
	pa = trunc_page(mem);
	dst = (unsigned char *)pa;
	for (i = 0; i < img_len; i++)
		dst[i] = img[i];
	dcache_clean_range(pa, npages * USER_PGBYTES);
	icache_invalidate_all();
	va = INIT_CODE_VA;
	for (i = 0; i < npages; i++, va += USER_PGBYTES, pa += USER_PGBYTES)
		pmap_enter(upmap, va, pa,
			   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	/* stack (zeroed), tepat setelah code */
	stack_va = INIT_CODE_VA + npages * USER_PGBYTES;
	dst = (unsigned char *)pa;
	for (i = 0; i < stack_pages * USER_PGBYTES; i++)
		dst[i] = 0;
	for (i = 0, va = stack_va; i < stack_pages;
	     i++, va += USER_PGBYTES, pa += USER_PGBYTES)
		pmap_enter(upmap, va, pa,
			   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	/* heap (zeroed sbrk region), tepat setelah stack */
	heap_va = stack_va + stack_pages * USER_PGBYTES;
	dst = (unsigned char *)pa;
	for (i = 0; i < INIT_HEAP_PAGES * USER_PGBYTES; i++)
		dst[i] = 0;
	va = heap_va;
	for (i = 0; i < INIT_HEAP_PAGES; i++,
	     va += USER_PGBYTES, pa += USER_PGBYTES)
		pmap_enter(upmap, va, pa,
			   VM_PROT_READ | VM_PROT_WRITE, FALSE);

	udesc->task = utask;
	udesc->heap_base = heap_va;
	udesc->brk = heap_va;
	udesc->heap_end = heap_va + INIT_HEAP_PAGES * USER_PGBYTES;
	udesc->va_top = udesc->heap_end;	/* Q2b: valid s/d akhir heap */

	*out_task = utask;
	*out_pmap = upmap;
	*out_stack_top = stack_va + stack_pages * USER_PGBYTES;
	return 0;
}

/*
 * launch_uprog -- Fase C: launch one user program as its own Mach task.
 *
 * Program berjalan sampai SYS_EXIT (one-shot, sequential); lalu
 * berikutnya.  Koordinasi antar program via ramfs sentinel files.
 *
 * Returns the program's exit code, or ~0u on launch failure.
 */
static unsigned int
launch_uprog(const char *name, unsigned char *img, unsigned int img_len,
	     unsigned int stack_pages, struct user_task *udesc)
{
	task_t		utask;
	pmap_t		upmap;
	unsigned int	rc, stack_top;

	/* Boot bersih: hanya tampil bila gagal. */
#if 0
	printf("launch_uprog: creating task for '%s'...\n", name);
#endif
	if (setup_uprog_task(name, img, img_len, stack_pages, udesc,
			     &utask, &upmap, &stack_top) != 0)
		return ~0u;

	/* Register for SYS_TLIST + set as current for syscalls. */
	if (nuprog < UPROG_MAX) {
		uprog_names[nuprog] = name;
		uprog_states[nuprog] = 0u;	/* RUNNABLE */
		nuprog++;
	}
	cur_utask = utask;
	cur_udesc = udesc;
	cur_progid = -1;	/* one-shot: bukan pemegang token */

#if 0
	printf("launch_uprog: entering user mode ('%s')...\n", name);
#endif
	arm_pmap_activate_user(upmap);
	rc = user_enter_test(stack_top, INIT_CODE_VA);
	arm_pmap_activate_kernel();

	if (nuprog > 0)
		uprog_states[nuprog - 1u] = 2u;	/* EXITED (DEAD) */
	cur_utask = TASK_NULL;
	cur_udesc = 0;
	cur_progid = -1;

	/* Boot bersih: hanya tampil bila gagal. */
	if (rc != 0)
		printf("launch_uprog: '%s' FAIL (code 0x%x)\n", name, rc);
	return rc;
}

/*
 * launch_daemon -- App A2: daftarkan program user persisten (face,
 * uiapp) ke scheduler kooperatif.  Tidak masuk user mode di sini;
 * sched_run() yang menjalankannya bergantian nanti.
 */
static void
launch_daemon(const char *name, unsigned char *img, unsigned int img_len,
	      unsigned int stack_pages,
	      struct user_task *udesc, unsigned int progid)
{
	task_t	utask;
	pmap_t	upmap;
	unsigned int stack_top;

	if (setup_uprog_task(name, img, img_len, stack_pages, udesc,
			     &utask, &upmap, &stack_top) != 0) {
		printf("launch_daemon: FAIL ('%s')\n", name);
		return;
	}
	sched_register_daemon(name, utask, upmap, udesc, stack_top, progid);
}

/* Program user Fase C: diluncurkan berurutan. */
struct uprog_image {
	const char	*name;
	unsigned char	*img;
	unsigned int	*lenp;
	unsigned int	stack_pages;	/* Q2b: halaman stack (default 1) */
};

/* Program one-shot Fase C/D: jalan berurutan sampai SYS_EXIT. */
static struct uprog_image uprogs[] = {
	{ "init",  init_img,  &init_img_len,  1u },
	{ "ucat",  ucat_img,  &ucat_img_len,  1u },
	{ "uls",   uls_img,   &uls_img_len,   1u },
	{ "uecho", uecho_img, &uecho_img_len, 1u },
	{ "umon",  umon_img,  &umon_img_len,  1u },
	{ "ugpio", ugpio_img, &ugpio_img_len, 1u },
	{ "usd",   usd_img,   &usd_img_len,   1u },
	{ "ufs",   ufs_img,   &ufs_img_len,   1u },
	{ "utimeset", utimeset_img, &utimeset_img_len, 1u },	/* F2: time regression */
	{ "uaudiotest", uaudiotest_img, &uaudiotest_img_len, 1u },	/* D1: ACODEC */
	{ "uaudiocap", uaudiocap_img, &uaudiocap_img_len, 1u },	/* D2: I2S RX */
	{ "uaudiogain", uaudiogain_img, &uaudiogain_img_len, 1u },	/* D3: gain */
	{ "uvoice", uvoice_img, &uvoice_img_len, 1u },	/* D4: VOICE uplink */
#ifdef QA_TIER_A
	{ "ulinktest", ulinktest_img, &ulinktest_img_len, 1u },	/* C6L-1: link test */
#endif /* QA_TIER_A */
	{ "upl330", upl330_img, &upl330_img_len, 1u },	/* D2b: PL330 */
	{ "ulfs", ulfs_img, &ulfs_img_len, 1u },	/* B1: littlefs */
	{ "ublogtest", ublogtest_img, &ublogtest_img_len, 1u },	/* B2 diag */
	{ "ufirst", ufirst_img, &ufirst_img_len, 1u },	/* B3: first boot */
	{ "utimegate", utimegate_img, &utimegate_img_len, 1u },	/* B3 */
	{ "uupdsig", uupdsig_img, &uupdsig_img_len, 1u },	/* B5.3 */
	{ "ucred", ucred_img, &ucred_img_len, 1u },	/* B5.5 */
	{ "qabot", qabot_img, &qabot_img_len, 1u },	/* Q1: harness, one-shot */
};
#define	NUPROGS	(sizeof(uprogs) / sizeof(uprogs[0]))

/* Daemon persisten App A2/A4: jalan di scheduler kooperatif. */
static struct uprog_image daemon_images[] = {
	{ "face",  face_img,  &face_img_len,  1u },	/* progid 0: Qabot server */
	{ "uiapp", uiapp_img, &uiapp_img_len, 1u },	/* progid 1: menu */
	/* { "ntp",   ntp_img,   &ntp_img_len,   1u }, */	/* Nonaktif sementara Q2b: UDP sandbox blokir, ganggu timing */
	/* { "utcpcli", utcpcli_img, &utcpcli_img_len, 1u }, */	/* Nonaktif: tcc hanya 1 koneksi (race dengan utlscli). Aktifkan untuk uji Q2a saja. */
	/* { "utlscli", utlscli_img, &utlscli_img_len, 8u }, */	/* Nonaktif Q2c/F2: tcc 1 koneksi; aktifkan sementara untuk uji TLS */
	{ "qabotd", qabotd_img, &qabotd_img_len, 8u },	/* progid 2: Qabot daemon persisten (Q4) */
	{ "uipcld", uipcld_img, &uipcld_img_len, 1u },	/* progid 3: IPC-4 concurrent client daemon (test-only,
							 * marker-gated: dormant without /data/iptoolsc-test
							 * == "1"; INSERTED BEFORE sh -- sh must stay last,
							 * see SH_IMAGE_IDX below; the 15/30 s default
							 * watchdog class above applies to it) */
	{ "linkd", linkd_img, &linkd_img_len, 2u },	/* progid 4: C6L-5 link daemon -- single transport
							 * owner, IPC server on reserved name `link`;
							 * INSERTED BEFORE sh like uipcld (default
							 * 15/30 s watchdog class; it parks/yields
							 * every turn, see user/linkd.c) */
	{ "ulink5d", ulink5d_img, &ulink5d_img_len, 1u },	/* progid 5: C6L-5 second client daemon (test-only,
							 * marker-gated: dormant without /data/link5-test
							 * == "1"; INSERTED BEFORE sh; default watchdog
							 * class) */
	{ "sh", sh_img, &sh_img_len, 1u },	/* progid 6: shell qaon> (Q9) */
	/* { "utcpkeep", utcpkeep_img, &utcpkeep_img_len, 1u }, */	/* F1: nonaktif (tcc 1 koneksi) */
	/* { "utlskeep", utlskeep_img, &utlskeep_img_len, 1u }, */	/* F1: nonaktif (investigasi selesai) */

};
#define	NDAEMON_IMAGES	(sizeof(daemon_images) / sizeof(daemon_images[0]))

/* Boot UX: index of the shell in daemon_images[] (the GUI set is
 * every other entry). The table order above is load-bearing. */
#define	SH_IMAGE_IDX	(NDAEMON_IMAGES - 1u)

/* One udesc per one-shot (NUPROGS) + per daemon image. This
 * array MUST cover both index ranges: it was sized UPROG_MAX=16
 * while NUPROGS grew past it, so daemon udesc writes (index
 * NUPROGS+i) silently corrupted neighbouring statics -- latent
 * until NUPROGS reached 20 (uupdsig), when the write landed on
 * scheduler state and the shell daemon failed to register
 * ("daemon table penuh", no qaon> prompt; found by soak-test
 * regression 2026-10-20). */
static struct user_task	uprog_udesc[NUPROGS + NDAEMON_IMAGES];

/*
 * FND-3: health supervisor implementation. The model and
 * the ladder are described with the state block above
 * sched_cur; the full design + evidence live in
 * docs/rencana-fondasi-tanpa-board/DEVLOG-FND3.md. Fixed
 * table, integer math, no allocation in the supervisor
 * itself (respawn reuses setup_uprog_task, the same
 * machinery gui_maybe_launch already runs from the switch
 * path). ARCH: the supervisor is the principled R6
 * exception (docs/ARSITEKTUR.md S5): policy in kernel
 * because it must act exactly when userspace is what hung.
 */

/* Compose a bootlog note "wd <kind> <name>" (36-char cap
 * in struct bs_rec; buf is 40). */
static void
wd_note(char *buf, unsigned int max, const char *kind,
    const char *name)
{
	unsigned int i = 0, j;

	for (j = 0; "wd "[j] && i < max - 1u; j++)
		buf[i++] = "wd "[j];
	for (j = 0; kind[j] && i < max - 1u; j++)
		buf[i++] = kind[j];
	if (i < max - 1u)
		buf[i++] = ' ';
	for (j = 0; name[j] && i < max - 1u; j++)
		buf[i++] = name[j];
	buf[i] = 0;
}

/*
 * wdt_kick -- the hardware watchdog seam. On the board,
 * the software supervisor feeds the RV1103 dw-wdt
 * (0xff5a0000, two-stage: interrupt first, reset on a
 * second uncleared timeout -- docs/riset-soc-rv1103/
 * RINGKASAN.md S2 item 3) from exactly here, so the
 * hardware only ever extends a verdict the supervisor
 * has already reached. The kick must mean "the
 * supervisor ran and nothing was overdue", never "a
 * timer fired": wd_supervise() calls it only after a
 * scan that found every daemon within its deadline.
 * The dw-wdt clock rate is unknown (CRU table), so the
 * hardware timeout programming waits for the driver;
 * supervisor deadlines stay in 100 Hz ticks meanwhile.
 * In QEMU this is a no-op. TODO(HW).
 */
static void
wdt_kick(void)
{
}

/* Terminal rung: mark the reset as a watchdog crash
 * through the bootstate path (reason 3 + fail_count armed
 * so the next boot lands in safe mode), log it, reset.
 * Returns only if the platform refuses the reset. */
static void
wd_fatal_reset(const char *who)
{
	char note[40];
	unsigned int rc;

	wd_note(note, sizeof(note), "reset", who);
	printf("watchdog: FATAL (%s) -- bootstate watchdog "
	    "mark + reset\n", who);
	bootstate_note_watchdog(arm_timer_ticks() * 10u, note);
	rc = psci_system_reset();
	printf("watchdog: platform refused reset (PSCI rc=0x%x)\n",
	    rc);
}

/* Rebuild a persistent daemon in its own slot from its
 * embedded image: fresh Mach task/pmap/context, same
 * udesc (setup_uprog_task resets its heap bookkeeping).
 * The dead instance's task/memory are NOT destroyed --
 * Mach task teardown in the switch path is a bigger
 * change than this track owns; the leak is bounded by
 * WD_MAX_RESPAWN per daemon per boot (the FND-2 dev log
 * already deferred VM destruction to this design).
 * State a respawned daemon loses: everything it held in
 * RAM (face: expression/idle timers; uiapp: menu position
 * -- it restarts in the acquire-wait loop; qabotd: any
 * in-flight provider session + the TCP connection, which
 * daemon_reclaim already closed; sh: the session -- it
 * re-takes the console and re-runs its startup, including
 * its self-test). On-disk state (littlefs config,
 * credentials, bootstate) is untouched. */
static int
wd_respawn(struct daemon *d)
{
	struct uprog_image *im;
	task_t utask;
	pmap_t upmap;
	unsigned int stack_top, i;

	if (d->wd_imgidx < 0 ||
	    (unsigned)d->wd_imgidx >= NDAEMON_IMAGES)
		return -1;
	im = &daemon_images[d->wd_imgidx];
	if (setup_uprog_task(im->name, im->img, *im->lenp,
	    im->stack_pages, d->udesc, &utask, &upmap,
	    &stack_top) != 0)
		return -1;
	d->task = utask;
	d->pmap = upmap;
	for (i = 0; i < 13u; i++)
		d->ctx.r[i] = 0;
	d->ctx.sp_usr = stack_top;
	d->ctx.lr_usr = 0;
	d->ctx.pc = INIT_CODE_VA;
	d->ctx.spsr = 0x10u;	/* USR, IRQ on */
	d->state = 1;
	d->wd_last_progress = arm_timer_ticks();
	d->wd_warned = 0;
	uprog_states[d->uprog_idx] = 0u;	/* RUNNABLE again */
	return 0;
}

/*
 * wd_supervise -- class (a) scan, run at the scheduler
 * switch points. Returns 1 iff it killed + respawned the
 * CURRENT daemon (the caller must resume the fresh ctx
 * instead of the saved one).
 *
 * Ladder per daemon: progress age > warn deadline ->
 * bootlog BLE_WD_WARN + print (once per episode; re-arms
 * when progress resumes). Age > hang deadline -> bootlog
 * BLE_WD_HANG, then: spawn child -> reclaim + wake the
 * parent with USER_EXIT_WDOG (WD_MAX_CHILD_KILLS
 * consecutive child kills -> fatal reset: three wedged
 * children in a row with no clean run in between is a
 * system problem, not a program problem); persistent
 * daemon -> reclaim (FND-2) + respawn while the
 * per-boot budget lasts, else fatal reset via
 * wd_fatal_reset() -- the bootstate mark arms safe mode
 * for the next boot.
 */
static int
wd_supervise(void)
{
	unsigned int now = arm_timer_ticks();
	unsigned int idx;
	int cur_respawned = 0;
	int any_overdue = 0;
	char note[40];

	for (idx = 0; idx < NDAEMON; idx++) {
		struct daemon *d = &daemons[idx];
		unsigned int age;

		if (d->state != 1)
			continue;	/* dead, or WAITING (spawn parent Q9 / IPC waiter IPC-2): parked is not hung */
		if (d->wd_hang_at == 0)
			continue;	/* not supervised */
		age = now - d->wd_last_progress;
		if (age <= d->wd_warn_at) {
			if (d->wd_warned) {
				d->wd_warned = 0;
				printf("watchdog: '%s' progressing "
				    "again\n", d->name);
			}
			continue;
		}
		any_overdue = 1;
		if (!d->wd_warned) {
			d->wd_warned = now;
			wd_note(note, sizeof(note), "warn", d->name);
			bootstate_log_event(WD_EV_WARN, now * 10u,
			    note);
			printf("watchdog: '%s' no progress for %u ms"
			    " -- warn\n", d->name, age * 10u);
			continue;
		}
		if (age <= d->wd_hang_at)
			continue;	/* warned; grace running */

		d->wd_hangs++;
		wd_note(note, sizeof(note), "hang", d->name);
		bootstate_log_event(WD_EV_HANG, now * 10u, note);
		printf("watchdog: '%s' HANG (no progress %u ms)\n",
		    d->name, age * 10u);

		if (idx == NDAEMON - 1u) {
			/* Spawn child: no image identity to
			 * respawn; kill it and let the parent
			 * decide what runs next. */
			wd_child_kills++;
			if (wd_child_kills >= WD_MAX_CHILD_KILLS)
				wd_fatal_reset(d->name);
			daemon_reclaim((int)idx);
			spawn_exitcode = USER_EXIT_WDOG;
			spawn_done = 1;
			d->state = 0;
			if (spawn_parent >= 0) {
				daemons[spawn_parent].state = 1;
				daemons[spawn_parent].
				    wd_last_progress = now;
			}
			printf("watchdog: spawn child killed "
			    "(kill %u), parent woken with 0x%x\n",
			    wd_child_kills, USER_EXIT_WDOG);
			continue;
		}

		daemon_reclaim((int)idx);
		d->state = 0;
		if (d->wd_respawns >= WD_MAX_RESPAWN) {
			printf("watchdog: '%s' respawn budget "
			    "exhausted (%u)\n", d->name,
			    d->wd_respawns);
			wd_fatal_reset(d->name);
			d->wd_respawns = 0;	/* refused: reladder */
		}
		d->wd_respawns++;
		if (wd_respawn(d) != 0) {
			printf("watchdog: '%s' respawn setup "
			    "failed\n", d->name);
			wd_fatal_reset(d->name);
			continue;
		}
		wd_note(note, sizeof(note), "respawn", d->name);
		bootstate_log_event(WD_EV_RESPAWN, now * 10u, note);
		printf("watchdog: '%s' respawned (respawn %u)\n",
		    d->name, d->wd_respawns);
		if ((int)idx == sched_cur)
			cur_respawned = 1;
	}
	if (!any_overdue)
		wdt_kick();
	return cur_respawned;
}

/*
 * wd_irq_tick -- class (b): called from the timer IRQ
 * (trap.c) on every tick with usermode=1 when the
 * interrupted frame was running user code. If the
 * cooperative scheduler has not run for WD_STARVE_TICKS
 * while user code holds the CPU, the running daemon is
 * spinning: switch points will never run again, so this
 * IRQ path is the only mechanism left. Its action is
 * deliberately the terminal rung only -- bootstate
 * watchdog mark + platform reset -- because nothing in
 * IRQ context can safely do the softer rungs (no task
 * setup, no switch surgery). Safety of the mark from
 * IRQ: the gate (sched active + interrupted frame is
 * user mode) means the boot thread is entirely out of
 * the kernel, so the shared bootstate buffer is free and
 * blk I/O is polled. If the platform refuses the reset,
 * the window re-arms and the trip retries (and re-logs)
 * instead of wedging silently.
 */
void
wd_irq_tick(unsigned int usermode)
{
	unsigned int now;
	const char *who;
	char note[40];

#if !WD_ENABLED
	(void)usermode;
	return;
#endif
	if (!sched_active || sched_cur < 0 || wd_irq_tripped)
		return;
	if (!usermode)
		return;	/* kernel/WFI stretches are not hangs */
	now = arm_timer_ticks();
	if (now - wd_last_activity <= WD_STARVE_TICKS)
		return;
	wd_irq_tripped = 1;
	who = daemons[sched_cur].name;
	wd_note(note, sizeof(note), "reset", who);
	printf("watchdog: no scheduler activity for %u ms while"
	    " '%s' runs -- watchdog reset\n",
	    (now - wd_last_activity) * 10u, who);
	bootstate_note_watchdog(now * 10u, note);
	(void)psci_system_reset();
	wd_irq_tripped = 0;
	wd_last_activity = now;
}

/*
 * user_launch_init -- Fase C/D + App A2.
 *
 * Called from startrtclock() after the self-tests.
 *  1. Program one-shot (init..ufs) jalan berurutan sampai SYS_EXIT;
 *     artefak ramfs diverifikasi.
 *  2. Network di-init (net_init_all); setelah ini net dipump dari
 *     scheduler tiap SYS_YIELD.
 *  3. Daemon persisten (face, uiapp) didaftarkan lalu sched_run()
 *     menjalankannya bergantian (tak kembali).
 */
extern void	net_init_all(void);

/* Boot UX: launch one daemon image by table index. */
static void
launch_daemon_idx(unsigned int i)
{
	launch_daemon(daemon_images[i].name, daemon_images[i].img,
		      *daemon_images[i].lenp,
		      daemon_images[i].stack_pages,
		      &uprog_udesc[NUPROGS + i], i);
}

/* Boot UX: deferred GUI launch, run from the scheduler switch
 * path (never from a syscall frame). Launches every daemon
 * image except the shell. */
static void
gui_maybe_launch(void)
{
	unsigned int i;

	if (!gui_pending || gui_launched)
		return;
	gui_launched = 1;
	gui_pending = 0;
	for (i = 0; i < NDAEMON_IMAGES; i++) {
		if (i == SH_IMAGE_IDX)
			continue;
		launch_daemon_idx(i);
	}
	printf("gui: dashboard daemons launched\n");
}

void
user_launch_init(void)
{
	unsigned i, fails = 0;
	static const char *want_files[] = {
		"/motd.txt", "/echo.txt", "/umon.out",
		"/gpio.out", "/sd.out", "/fat.out",
		"/.motd_ready", "/.ucat_done", "/.uls_done",
		"/.uecho_done", "/.umon_done",
		"/.gpio_cmd_ready", "/.ugpio_done",
		"/.sd_cmd_ready", "/.usd_done",
		"/.fat_cmd_ready", "/.ufs_done",
	};

	ramfs_init();

	/* B2: persistent boot state (increments fail counter; arms
	 * safe mode at >=3 consecutive unconfirmed boots). */
	bootstate_init();

	/* The cooperative sched test disables the timer; user programs
	 * need ticks for SYS_UPTIME.  Enable it here, after all task setup
	 * (an immediate IRQ during task_create trips MI thread_select). */
	arm_timer_enable();

	for (i = 0; i < NUPROGS; i++) {
		unsigned int rc = launch_uprog(uprogs[i].name, uprogs[i].img,
					       *uprogs[i].lenp,
					       uprogs[i].stack_pages,
					       &uprog_udesc[i]);
		if (rc != 0) {
			printf("user_launch_init: FAIL ('%s' exit 0x%x)\n",
			       uprogs[i].name, rc);
			fails++;
		}
	}

	/* Verifikasi artefak koordinasi ramfs. */
	for (i = 0; i < sizeof(want_files) / sizeof(want_files[0]); i++) {
		if (!ramfs_exists(want_files[i])) {
			printf("user_launch_init: FAIL (missing %s)\n",
			       want_files[i]);
			fails++;
		}
	}

	if (fails == 0)
		printf("user_launch_init: PASS (%u/%u programs, %u/%u files)\n",
		       NUPROGS, NUPROGS,
		       (unsigned)(sizeof(want_files) / sizeof(want_files[0])),
		       (unsigned)(sizeof(want_files) / sizeof(want_files[0])));
	else
		printf("user_launch_init: %u FAILs\n", fails);
	gpu_splash(60u, 1u);	/* Boot UX: one-shots done */

	/* B2: a boot whose one-shots all passed is a healthy boot:
	 * reset the consecutive-failure counter.
	 * B4: an armed sabotage (bad-update simulation) suppresses
	 * exactly one healthy mark. */
	if (bootstate_sabotage_take()) {
		printf("bootstate: sabotage armed -- healthy mark skipped\n");
	} else if (fails == 0) {
		bootstate_healthy(arm_timer_ticks() * 10u);
	}

	/* Fase D: init network (blocking); pump-nya dari scheduler. */
	net_init_all();
	gpu_splash(85u, 2u);	/* Boot UX: network up */
	gpu_splash(100u, 3u);	/* Boot UX: ready (holds as idle screen) */

	/* App A2: daftarkan daemon persisten, lalu jalan.
	 * B2 safe mode: only the shell daemon is launched.
	 * Boot UX: in dev mode only the shell launches at boot;
	 * the GUI daemons (all images except sh, the last entry)
	 * start on demand via SYS_GUI_START. In user mode
	 * (bootstate flag bit2) everything launches as before. */
	for (i = 0; i < NDAEMON_IMAGES; i++) {
		if (bootstate_safe_mode() && i != SH_IMAGE_IDX) {
			printf("bootstate: skipping daemon '%s' (safe mode)\n",
			    daemon_images[i].name);
			continue;
		}
		if (!bootstate_safe_mode() && !bootstate_gui_mode() &&
		    i != SH_IMAGE_IDX)
			continue;	/* dev mode: GUI on demand */
		launch_daemon_idx(i);
		if (i != SH_IMAGE_IDX)
			gui_launched = 1;
	}
	if (!bootstate_safe_mode() && !bootstate_gui_mode())
		printf("bootux: dev mode -- shell only; "
		    "'gui' starts the dashboard\n");
	sched_run();
	/* NOTREACHED */
}
