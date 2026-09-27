/*
 * task.h - Mach task sebagai protection domain (bring-up).
 *
 * Satu task = satu protection domain: punya vm_space sendiri (ruang
 * alamat virtual) + ipc_space sendiri (namespace port beserta rights).
 * Thread selalu milik tepat satu task; scheduler mengganti address
 * space setiap kali berpindah ke thread dari task yang berbeda.
 */
#ifndef _TASK_H_
#define _TASK_H_

#include <stdint.h>
#include "vm.h"
#include "ipc.h"
#include "zone.h"

struct task {
    unsigned id;
    struct vm_space vm;     /* ruang alamat milik task ini */
    struct ipc_space ipc;   /* namespace port milik task ini */
    uint32_t brk;           /* Fase 8: program break user (0 = belum init) */
    unsigned refs;
};

/* Bentuk task baru: satu L1 (salinan tabel kernel) + namespace IPC
 * kosong. Zone port/pesan dipakai bersama semua task (satu pool). */
void task_create(struct task *t, struct zone *port_zone,
                 struct zone *msg_zone);

unsigned task_id(const struct task *t);

#endif /* _TASK_H_ */
