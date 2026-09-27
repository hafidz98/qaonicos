/*
 * pager.h - External pager / memory object (Fase 7, bring-up).
 *
 * Model ala Mach: sebuah memory object didukung oleh pager eksternal
 * (thread di task lain). Task me-map rentang VA ke object; saat VA itu
 * diakses pertama kali, vm_page_fault meminta isi halaman ke pager via
 * IPC sinkron (data_request -> data_supply), lalu mem-mapnya.
 *
 * Protokol (w.id):
 *   MSG_DATA_REQUEST (0x100): w.data = { uint32_t obj_id;
 *                                        uint32_t offset; }
 *   MSG_DATA_SUPPLY  (0x101): w.data = { uint32_t obj_id;
 *                                        uint32_t offset;
 *                                        uint8_t page[4096]; }
 * Butuh IPC_MSG_DATA >= 4104 (sekarang 4128).
 *
 * Seluruh transaksi berjalan di ipc_space milik pager_task: kernel
 * (dari fault handler) mengirim request + menunggu reply di sana;
 * pager thread menerima request + mengirim supply di sana juga.
 * Thread yang fault tidak perlu tahu port pager.
 */
#ifndef _PAGER_H_
#define _PAGER_H_

#include <stdint.h>

struct task;
struct vm_space;

/* ID pesan protokol pager. */
#define MSG_DATA_REQUEST  0x100u
#define MSG_DATA_SUPPLY   0x101u

struct vm_object {
    unsigned id;
    struct task *owner;       /* task yang me-map object */
    struct task *pager_task;  /* task tempat pager thread berjalan */
    unsigned req_name;        /* port request di pager_task->ipc */
    unsigned rep_name;        /* port reply di pager_task->ipc */
    uint32_t size;            /* bytes */
};

/* Daftarkan object baru. Mengembalikan id (>0), 0 bila gagal. */
unsigned vm_object_create(struct task *owner, struct task *pager_task,
                          unsigned req_name, unsigned rep_name,
                          uint32_t size);

/* Cabut object + semua mapping yang menunjuknya. Minimal tapi benar. */
int vm_object_destroy(unsigned obj_id);

/* Catat [va, va+npages*4K) milik t sebagai jendela ke object (offset
 * obj_offset). Pemetaan halaman fisiknya malas: terjadi saat fault
 * via pager. va harus sejajar halaman dan di dalam demand range.
 * 0 = ok, -1 = gagal. */
int vm_map_object(struct task *t, uint32_t va, unsigned obj_id,
                  uint32_t obj_offset, unsigned npages, unsigned prot);

/* Cari mapping pager untuk (t, va). 1 = ketemu (o/obj_id/off/prot
 * diisi), 0 = va bukan object-backed. Dipakai vm_page_fault. */
int pager_lookup(struct task *t, uint32_t va, struct vm_object **o,
                 unsigned *obj_id, uint32_t *off, unsigned *prot);

/* Minta satu halaman (4096 byte) pada offset objek ke pager via
 * ipc_rpc (blocking, pola Fase 6); salin ke buf. 0 = ok, -1 = gagal.
 *
 * Buffer wire-nya statis di dalam: stack abort cuma 8KB sedangkan
 * struct ipc_wire kini ~4KB. Jangan panggil dari dua fault bersamaan
 * (abort handler memang tidak reentrant antar thread - lihat vm.c). */
int pager_fetch_page(struct vm_object *o, unsigned obj_id, uint32_t off,
                     uint8_t *buf);

#endif /* _PAGER_H_ */
