/*
 * ramfs.c - ramfs flat dalam RAM untuk kernel Mach 3 (Fase C migrasi).
 *
 * Port dari archive/kernel-scratch/kernel/src/fs.c (Fase 9):
 * satu tabel file global (16 file x 64 KB pool BSS statis = 1 MB;
 * deterministik, tanpa fragmentasi).  Tiap file punya generation
 * counter: delete menaikkan gen sehingga fd yang masih terbuka ke
 * slot itu terdeteksi basi.
 *
 * Adaptasi Mach 3:
 *   - Seksi kritis via splhigh()/splx() (pola MI), bukan cpsid mentah.
 *   - Tabel fd per-task: diindeks pointer Mach task (task_t), maks 8
 *     task user bersamaan.  fd 0/1/2 dicadangkan untuk console dan
 *     tidak dikelola di sini (ditolak).
 *   - Tanpa dependensi libc: semua copy pakai loop manual.
 *
 * Keterbatasan bring-up (diwarisi, disengaja): flat tanpa
 * subdirektori, tanpa truncate, tanpa rename, tanpa permission bits.
 */
#include "ramfs.h"
#include <machine/machspl.h>

#define	RAMFS_MAX_FILES	16u
#define	RAMFS_NAME_MAX	32u
#define	RAMFS_FILE_MAX	(64u * 1024u)
#define	RAMFS_MAX_TASKS	8u

struct ramfs_file {
	char		name[RAMFS_NAME_MAX];
	unsigned char	data[RAMFS_FILE_MAX];
	unsigned	size;
	unsigned	gen;
	int		used;
};

struct ramfs_fd {
	int		used;
	unsigned	fidx;
	unsigned	gen;
	unsigned	off;
	unsigned	flags;
};

struct ramfs_taskfds {
	task_t		task;	/* kunci; TASK_NULL = slot kosong */
	struct ramfs_fd	fds[RAMFS_MAX_FD];
};

static struct ramfs_file	rfs_files[RAMFS_MAX_FILES];
static struct ramfs_taskfds	rfs_tasks[RAMFS_MAX_TASKS];

void
ramfs_init(void)
{
	/* BSS sudah nol: used=0, gen=0, size=0, task=NULL. */
}

/* Lewati '/' di depan: "/a.txt" -> "a.txt". */
static const char *
name_norm(const char *name)
{
	while (*name == '/')
		name++;
	return name;
}

/* 1 bila nama valid: 1..RAMFS_NAME_MAX-1 char, tanpa '/' di tengah. */
static int
name_ok(const char *name)
{
	unsigned i;

	if (!name || name[0] == 0)
		return 0;
	for (i = 0; i < RAMFS_NAME_MAX; i++) {
		char c = name[i];
		if (c == 0)
			return 1;
		if (c == '/')
			return 0;
	}
	return 0;	/* tidak NUL-terminated dalam batas */
}

static int
name_eq(const char *a, const char *b)
{
	unsigned i;

	for (i = 0; i < RAMFS_NAME_MAX; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == 0)
			return 1;
	}
	return 1;
}

/* Cari slot fd-table untuk task t (alokasi bila belum ada).
 * Versi tanpa lock: pemanggil sudah di splhigh(). */
static struct ramfs_taskfds *
taskfds_nolock(task_t t)
{
	unsigned i, free = RAMFS_MAX_TASKS;

	if (t == TASK_NULL)
		return 0;
	for (i = 0; i < RAMFS_MAX_TASKS; i++) {
		if (rfs_tasks[i].task == t)
			return &rfs_tasks[i];
		if (rfs_tasks[i].task == TASK_NULL)
			free = i;
	}
	if (free >= RAMFS_MAX_TASKS)
		return 0;
	/* Slot baru: BSS task lain tak tersentuh; inisialisasi eksplisit. */
	rfs_tasks[free].task = t;
	for (i = 0; i < RAMFS_MAX_FD; i++)
		rfs_tasks[free].fds[i].used = 0;
	return &rfs_tasks[free];
}

/* Versi tanpa lock (pemanggil sudah di splhigh()). */
static int
find_nolock(const char *name)
{
	unsigned i;

	for (i = 0; i < RAMFS_MAX_FILES; i++)
		if (rfs_files[i].used && name_eq(rfs_files[i].name, name))
			return (int)i;
	return -1;
}

int
ramfs_exists(const char *name)
{
	int r;
	spl_t s;

	if (!name)
		return 0;
	name = name_norm(name);
	if (!name_ok(name))
		return 0;
	s = splhigh();
	r = find_nolock(name);
	splx(s);
	return r >= 0;
}

int
ramfs_open(task_t t, const char *name, unsigned flags)
{
	unsigned i;
	int idx, fd = -1;
	spl_t s;
	struct ramfs_taskfds *tf;

	if (t == TASK_NULL || !name)
		return -1;
	name = name_norm(name);
	if (!name_ok(name))
		return -1;
	s = splhigh();
	tf = taskfds_nolock(t);
	if (!tf) {
		splx(s);
		return -1;
	}
	idx = find_nolock(name);
	if (idx < 0) {
		int ni = -1;
		unsigned j;
		if (!(flags & RFS_O_CREAT)) {
			splx(s);
			return -1;
		}
		for (i = 0; i < RAMFS_MAX_FILES; i++) {
			if (!rfs_files[i].used) {
				ni = (int)i;
				break;
			}
		}
		if (ni < 0) {
			splx(s);
			return -1;
		}
		for (j = 0; j < RAMFS_NAME_MAX; j++) {
			rfs_files[ni].name[j] = name[j];
			if (name[j] == 0)
				break;
		}
		rfs_files[ni].size = 0;
		rfs_files[ni].used = 1;
		/* gen dibiarkan (naik tiap delete); slot baru gen=0. */
		idx = ni;
	}
	for (i = 3u; i < RAMFS_MAX_FD; i++) {
		if (!tf->fds[i].used) {
			struct ramfs_fd *f = &tf->fds[i];
			f->used = 1;
			f->fidx = (unsigned)idx;
			f->gen = rfs_files[idx].gen;
			f->off = 0u;
			f->flags = flags;
			fd = (int)i;
			break;
		}
	}
	splx(s);
	return fd;
}

/* Ambil file dari fd; 0 bila fd basi/tak valid.  Tanpa lock. */
static struct ramfs_file *
fd_file(struct ramfs_taskfds *tf, unsigned fd, struct ramfs_fd **out)
{
	struct ramfs_fd *f;
	struct ramfs_file *rf;

	if (!tf || fd >= RAMFS_MAX_FD)
		return 0;
	f = &tf->fds[fd];
	if (!f->used || fd < 3u)
		return 0;
	if (f->fidx >= RAMFS_MAX_FILES)
		return 0;
	rf = &rfs_files[f->fidx];
	if (!rf->used || rf->gen != f->gen)
		return 0;	/* fd basi: file di-delete sesudah open */
	if (out)
		*out = f;
	return rf;
}

int
ramfs_read(task_t t, unsigned fd, unsigned char *dst, unsigned len)
{
	struct ramfs_file *rf;
	struct ramfs_fd *f;
	struct ramfs_taskfds *tf;
	spl_t s;
	unsigned acc, i, n;

	if (!dst)
		return -1;
	s = splhigh();
	tf = taskfds_nolock(t);
	rf = tf ? fd_file(tf, fd, &f) : 0;
	if (!rf) {
		splx(s);
		return -1;
	}
	acc = f->flags & RFS_O_ACCMODE;
	if (acc != RFS_O_RDONLY && acc != RFS_O_RDWR) {
		splx(s);
		return -1;
	}
	n = (f->off < rf->size) ? rf->size - f->off : 0u;
	if (n > len)
		n = len;
	for (i = 0; i < n; i++)
		dst[i] = rf->data[f->off + i];
	f->off += n;
	splx(s);
	return (int)n;
}

int
ramfs_write(task_t t, unsigned fd, const unsigned char *src, unsigned len)
{
	struct ramfs_file *rf;
	struct ramfs_fd *f;
	struct ramfs_taskfds *tf;
	spl_t s;
	unsigned acc, room, n, i;

	if (!src)
		return -1;
	s = splhigh();
	tf = taskfds_nolock(t);
	rf = tf ? fd_file(tf, fd, &f) : 0;
	if (!rf) {
		splx(s);
		return -1;
	}
	acc = f->flags & RFS_O_ACCMODE;
	if (acc != RFS_O_WRONLY && acc != RFS_O_RDWR) {
		splx(s);
		return -1;
	}
	/* Tulis parsial bila melebihi kapasitas (bukan error). */
	room = (f->off < RAMFS_FILE_MAX) ? RAMFS_FILE_MAX - f->off : 0u;
	n = (len < room) ? len : room;
	for (i = 0; i < n; i++)
		rf->data[f->off + i] = src[i];
	f->off += n;
	if (f->off > rf->size)
		rf->size = f->off;
	splx(s);
	return (int)n;
}

int
ramfs_close(task_t t, unsigned fd)
{
	struct ramfs_taskfds *tf;
	struct ramfs_fd *f;
	spl_t s;

	if (t == TASK_NULL || fd >= RAMFS_MAX_FD || fd < 3u)
		return -1;
	s = splhigh();
	tf = taskfds_nolock(t);
	if (!tf) {
		splx(s);
		return -1;
	}
	f = &tf->fds[fd];
	if (!f->used) {
		splx(s);
		return -1;
	}
	f->used = 0;
	splx(s);
	return 0;
}

int
ramfs_list(char *dst, unsigned max)
{
	unsigned i, pos = 0u;
	int cnt = 0;
	spl_t s;

	if (!dst)
		return -1;
	s = splhigh();
	for (i = 0; i < RAMFS_MAX_FILES; i++) {
		unsigned j = 0u;
		if (!rfs_files[i].used)
			continue;
		cnt++;
		while (j < RAMFS_NAME_MAX && rfs_files[i].name[j])
			j++;
		/* Tulis "nama\n" hanya bila muat utuh. */
		if (max > 0u && pos + j + 1u <= max) {
			unsigned k;
			for (k = 0; k < j; k++)
				dst[pos + k] = rfs_files[i].name[k];
			dst[pos + j] = '\n';
			pos += j + 1u;
		}
	}
	splx(s);
	return cnt;
}

int
ramfs_delete(const char *name)
{
	spl_t s;
	int idx, r = -1;

	if (!name)
		return -1;
	name = name_norm(name);
	if (!name_ok(name))
		return -1;
	s = splhigh();
	idx = find_nolock(name);
	if (idx >= 0) {
		rfs_files[idx].used = 0;
		rfs_files[idx].gen++;	/* fd terbuka ke file ini jadi basi */
		rfs_files[idx].size = 0;
		rfs_files[idx].name[0] = 0;
		r = 0;
	}
	splx(s);
	return r;
}
