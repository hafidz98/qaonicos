/*
 * ulib.h - QaonicOS user library (Fase B, Mach 3 ABI).
 *
 * Bare-metal, tanpa libc.  Wrapper tipis di atas syscall via svc #0.
 * Nomor syscall kontinu dengan kernel lama; lihat docs/SYSCALL-ABI.md.
 */
#ifndef QAON_ULIB_H
#define QAON_ULIB_H

/* Nomor syscall (r7). */
#define SYS_WRITE	20u	/* r0=fd r1=buf r2=len -> byte tertulis / -1 */
#define SYS_YIELD	21u	/* -> 0 (tunggu 1 tick) */
#define SYS_EXIT	22u	/* r0=code -> tidak kembali */
#define SYS_SBRK	24u	/* r0=inkremen -> brk lama / (void*)-1 gagal */

/* Wrapper syscall. */
int	sys_write(int fd, const void *buf, unsigned len);
int	sys_yield(void);
void	sys_exit(int code);		/* tidak kembali */
void	*sys_sbrk(int incr);		/* incr=0 -> query brk */

/* Helper kecil. */
int	puts(const char *s);
unsigned ustrlen(const char *s);

#endif /* QAON_ULIB_H */
