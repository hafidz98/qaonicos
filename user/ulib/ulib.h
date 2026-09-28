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
