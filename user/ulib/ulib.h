/*
 * ulib.h - QaonicOS user library (Fase B/C, Mach 3 ABI).
 *
 * Bare-metal, tanpa libc.  Wrapper tipis di atas syscall via svc #0.
 * Nomor syscall kontinu dengan kernel lama; lihat docs/SYSCALL-ABI.md.
 *
 * Fase C: + wrapper file (30-34), stat/tlist/console (57-59), dan
 * lapisan kompatibel API lama Fase 10 (u_*) agar ucat/uls/uecho/umon
 * hasil port bisa dipakai hampir apa adanya.
 */
#ifndef QAON_ULIB_H
#define QAON_ULIB_H

/* Nomor syscall (r7). */
#define SYS_WRITE	20u	/* r0=fd r1=buf r2=len -> byte tertulis / -1 */
#define SYS_YIELD	21u	/* -> 0 (tunggu 1 tick) */
#define SYS_EXIT	22u	/* r0=code -> tidak kembali */
#define SYS_SBRK	24u	/* r0=inkremen -> brk lama / (void*)-1 gagal */
#define SYS_OPEN	30u	/* r0=path r1=flags -> fd / -1 */
#define SYS_READ	31u	/* r0=fd r1=buf r2=len -> byte / -1 */
#define SYS_CLOSE	32u	/* r0=fd -> 0 / -1 */
#define SYS_LS		33u	/* r0=buf r1=max -> jumlah file / -1 */
#define SYS_DELETE	34u	/* r0=path -> 0 / -1 */
#define SYS_STAT	57u	/* r0=buf r1=len -> 0 / -1 */
#define SYS_TLIST	58u	/* r0=buf r1=max -> jumlah entri / -1 */
#define SYS_READ_CONSOLE 59u	/* -> byte 0-255, atau -1 bila kosong */
#define SYS_GPIO_SET	40u	/* r0=pin r1=val(0/1) -> 0 / -1 */
#define SYS_GPIO_GET	41u	/* r0=pin -> 0/1 / -1 */
#define SYS_SD_READ	50u	/* r0=sector r1=buf512 -> 0 / -1 */
#define SYS_SD_WRITE	51u	/* r0=sector r1=buf512 -> 0 / -1 */
#define SYS_MKDIR	52u	/* r0=path -> 0 / -1 */
#define SYS_FAT_WRITE	53u	/* r0=path r1=buf r2=len -> bytes / -1 */
#define SYS_FAT_READ	54u	/* r0=path r1=buf r2=max -> bytes / -1 */
#define SYS_FAT_DELETE	55u	/* r0=path -> 0 / -1 */
#define SYS_READDIR	56u	/* r0=path r1=buf r2=max -> count / -1 */
#define SYS_DISPLAY_INFO 60u	/* r0=buf r1=len -> 0 / -1 */
#define SYS_DISPLAY_FLUSH 61u	/* r0=x r1=y r2=w r3=h r4=buf r5=len -> 0/-1 */
#define SYS_DISPLAY_GRANT 62u	/* face: beri token ke uiapp -> 0/-1 */
#define SYS_DISPLAY_ACQUIRE 63u	/* -> 1 bila pemegang token, else 0 */
#define SYS_DISPLAY_RELEASE 64u	/* pemegang kembalikan token -> 0/-1 */
#define SYS_DISPLAY_GET_EVENT 65u /* -> EV_* atau -1 */
#define SYS_DISPLAY_STATUS 66u	/* -> id pemegang (0=face,1=uiapp) */
#define SYS_UPTIME 67u		/* -> ms sejak boot */
#define SYS_DISPLAY_SLEEP 68u	/* r0: 1=minta sleep, 0=ambil+clear (face) */

/* Kode event input (App A2; sama dengan kernel). */
#define EV_UP		0
#define EV_DOWN		1
#define EV_LEFT		2
#define EV_RIGHT	3
#define EV_OK		4
#define EV_BACK		5
#define EV_MENU		6
#define EV_TICK		7
#define EV_NONE		(-1)

/* Flag open. */
#define O_RDONLY	0u
#define O_WRONLY	1u
#define O_RDWR		2u
#define O_CREAT		0x40u

/* Sentinel "data belum tersedia" di struct qaon_stat. */
#define QAON_UNKNOWN	0xFFFFFFFFu

/* Statistik sistem (Fase C).  Layout DISALIN MANUAL dari
 * kernel/kernel/arm/user.c — sizeof = 36. */
struct qaon_stat {
	unsigned int	uptime_ms;
	unsigned int	cpu_pct;
	unsigned int	mem_used_kb;
	unsigned int	mem_total_kb;
	unsigned int	blk_total_sec;
	unsigned int	blk_used_sec;
	unsigned int	net_rx_kb;
	unsigned int	net_tx_kb;
	unsigned int	nthreads;
};

/* Satu baris daftar thread (Fase C).  sizeof = 12. */
struct qaon_tentry {
	unsigned int	id;
	unsigned int	state;	/* 0=RUNNABLE, 2=EXITED */
	unsigned int	user;	/* 1 = user task */
};

/* Wrapper syscall. */
int	sys_write(int fd, const void *buf, unsigned len);
int	sys_yield(void);
void	sys_exit(int code);		/* tidak kembali */
void	*sys_sbrk(int incr);		/* incr=0 -> query brk */
int	sys_open(const char *path, unsigned flags);
int	sys_read(int fd, void *buf, unsigned len);
int	sys_close(int fd);
int	sys_ls(char *buf, unsigned max);
int	sys_delete(const char *path);
int	sys_stat(struct qaon_stat *st);
int	sys_tlist(struct qaon_tentry *e, unsigned max);
int	sys_read_console(void);		/* byte 0-255, -1 bila kosong */
int	sys_gpio_set(unsigned pin, unsigned val);	/* -> 0 / -1 */
int	sys_gpio_get(unsigned pin);			/* -> 0/1 / -1 */
int	sys_sd_read(unsigned sector, void *buf);	/* -> 0 / -1 */
int	sys_sd_write(unsigned sector, const void *buf);	/* -> 0 / -1 */
int	sys_mkdir(const char *path);			/* -> 0 / -1 */
int	sys_fat_write(const char *path, const void *buf, unsigned len);
int	sys_fat_read(const char *path, void *buf, unsigned max);
int	sys_fat_delete(const char *path);		/* -> 0 / -1 */
int	sys_readdir(const char *path, char *buf, unsigned max);

/* Info display (App A1). Layout DISALIN MANUAL dari kernel/user.c. */
struct qaon_display {
	unsigned int	width;
	unsigned int	height;
	unsigned int	format;	/* 0 = input RGB565 (strip parsial) */
};

int	sys_display_info(struct qaon_display *di, unsigned len);
int	sys_display_flush(unsigned x, unsigned y, unsigned w, unsigned h,
			  const void *buf, unsigned len);
int	sys_display_grant(void);
int	sys_display_acquire(void);
int	sys_display_release(void);
int	sys_display_get_event(void);
int	sys_display_status(void);
unsigned	sys_uptime(void);
int	sys_display_sleep(int req);

/* Jam dinding + UDP (App A4): 69-72. */
int	sys_time_set(unsigned unix_sec);
unsigned	sys_time_get(void);
int	sys_udp_send(unsigned dst_ip, unsigned dst_port,
		       const void *buf, unsigned len);
int	sys_udp_recv(void *buf, unsigned maxlen,
		       unsigned *src_ip, unsigned short *src_port);

/* TCP client (Q2a): 73-77. */
int	sys_tcp_connect(unsigned dst_ip, unsigned dst_port);
int	sys_tcp_status(void);
int	sys_tcp_send(const void *buf, unsigned len);
int	sys_tcp_recv(void *buf, unsigned maxlen);
int	sys_tcp_close(void);

/* Helper kecil. */
int	puts(const char *s);
unsigned ustrlen(const char *s);

/*
 * Lapisan kompatibel API lama (Fase 10) — untuk program port.
 */
int	u_write(unsigned fd, const char *buf, unsigned len);
int	u_open(const char *path, unsigned flags);
int	u_read(unsigned fd, char *buf, unsigned len);
int	u_close(unsigned fd);
int	u_ls(char *buf, unsigned max);
void	u_yield(void);
void	u_exit(void);			/* tidak kembali */
void	u_put(const char *s);		/* string NUL -> console */
unsigned u_strlen(const char *s);
int	u_mcmp(const char *a, const char *b, unsigned n); /* 0 = sama */
int	u_stat(struct qaon_stat *s);
int	u_tlist(struct qaon_tentry *e, unsigned max);
int	u_console_getc(void);		/* byte 0-255, -1 bila kosong */
int	u_gpio_set(unsigned pin, unsigned val);	/* -> 0 / -1 */
int	u_gpio_get(unsigned pin);			/* -> 0/1 / -1 */
int	u_sd_read(unsigned sector, void *buf);	/* -> 0 / -1 */
int	u_sd_write(unsigned sector, const void *buf);	/* -> 0 / -1 */
int	u_mkdir(const char *path);			/* -> 0 / -1 */
int	u_fat_write(const char *path, const void *buf, unsigned len);
int	u_fat_read(const char *path, void *buf, unsigned max);
int	u_fat_delete(const char *path);			/* -> 0 / -1 */
int	u_readdir(const char *path, char *buf, unsigned max);

/* String yang ditulis uecho.c ke /echo.txt (satu definisi bersama). */
#define UECHO_STR "uecho: halo dari utilitas\n"

/* Batas iterasi u_wait_file (yield tiap 256 iterasi). */
#define ULIB_POLL_MAX 1000000u

/* Tunggu sampai `path` bisa di-open O_RDONLY (poll + yield).
 * Kembalikan 1 bila muncul, 0 bila timeout. */
int	u_wait_file(const char *path);

/* Buat file sentinel kosong (open O_CREAT|O_RDWR lalu close).
 * Kembalikan 1 bila sukses, 0 bila gagal. */
int	u_touch(const char *path);

#endif /* QAON_ULIB_H */
