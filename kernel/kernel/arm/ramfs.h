/*
 * ramfs.h - ramfs flat untuk kernel Mach 3 (Fase C migrasi).
 *
 * Port dari archive/kernel-scratch/kernel/src/fs.h (Fase 9).
 * Adaptasi Mach 3:
 *   - Sinkronisasi via splhigh()/splx() (machspl.h), bukan cpsid mentah.
 *   - Tabel fd per-task diindeks pointer Mach task (task_t); struct
 *     user_task di user.c tidak perlu diubah.
 *   - Tanpa struct task lama; tanpa memcpy libc (loop manual).
 */
#ifndef _ARM_RAMFS_H_
#define _ARM_RAMFS_H_

#include <kern/task.h>

/* Flag open (nilai kompatibel dengan kernel lama / ulib). */
#define	RFS_O_RDONLY	0u
#define	RFS_O_WRONLY	1u
#define	RFS_O_RDWR	2u
#define	RFS_O_ACCMODE	3u
#define	RFS_O_CREAT	0x40u

#define	RAMFS_MAX_FD	16u	/* fd 0/1/2 = console (bukan file) */
#define	RAMFS_PATH_MAX	64u

void	ramfs_init(void);
int	ramfs_open(task_t t, const char *name, unsigned flags);
int	ramfs_read(task_t t, unsigned fd, unsigned char *dst, unsigned len);
int	ramfs_write(task_t t, unsigned fd, const unsigned char *src,
		    unsigned len);
int	ramfs_close(task_t t, unsigned fd);
int	ramfs_list(char *dst, unsigned max);
int	ramfs_delete(const char *name);
int	ramfs_exists(const char *name);	/* 1 = ada, 0 = tidak */

#endif /* _ARM_RAMFS_H_ */
