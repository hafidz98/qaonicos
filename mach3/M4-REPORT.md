# M4 Report — Mach 3 ARM Port (QEMU virt)

Lanjutan dari M3 (boot stabil sampai idle loop).

## Item 1: Real `pmap_enter` dengan halaman 4KB — DONE

**Status:** Selesai, terverifikasi hardware via `pmap_selftest` (PASS).

### Yang diimplementasikan (`kernel/arm/pmap.c`, `kernel/arm/pmap.h`, `kernel/arm/pte.h`)

- **L1 per-pmap**: `pmap_create()` alokasi L1 privat 16KB-aligned (32KB alloc +
  round-up via `kmem_alloc`), diisi copy dari boot L1 agar mapping kernel
  (RAM sections, device window, vectors) terlihat. Field `l1_alloc` baru di
  `struct pmap` untuk free di `pmap_destroy()`.
- **L2 page tables**: pool 1KB tables (4 per halaman 4KB dari `kmem_alloc`),
  dilindungi `splhigh` (UP). Alokasi di luar critical section agar aman bila
  `kmem_alloc` block.
- **`pmap_enter()`**: untuk va di luar identity range, pasang L1 page-table
  descriptor (domain 0) bila perlu, lalu L2 small-page descriptor
  (TEX=001/C=1/B=1/S=1, AP dari prot) + TLB invalidate per halaman.
- **`pmap_remove()` / `pmap_protect()` / `pmap_extract()`**: jalan di atas L2.
- **`arm_pmap_activate_user()` / `_kernel()`**: switch TTBR0 (dengan bit outer-WB
  seperti locore.s) + TLBIALL + DSB/ISB.
- **`pmap_selftest()`** (dipanggil dari `machine_init`): buat pmap, map 1 halaman
  di VA 0x100000, verifikasi `pmap_extract`, switch TTBR0 ke user L1, tulis/baca
  magic value lewat mapping baru, switch balik, `pmap_remove`, `pmap_destroy`.
  Berjalan tiap boot sebagai regression test.

### Bug yang ditemukan saat implementasi

1. **Salah pakai `kmem_alloc`**: MI `kmem_alloc(map, &addr, size)` return
   `kern_return_t`, bukan alamat. (Fault `str [r8]` di dalam MI.)
2. **`L2_SP_XN` merusak type field**: bit 0 = 1 bikin descriptor 0b11 (invalid),
   bukan small page. ARMv7 short-descriptor small page tidak punya bit XN
   terpisah — define di `pte.h` dikoreksi, semua user page executable di M4.

### Verifikasi
- Build: MI 94/94, MD 16/16, LINK OK.
- Boot QEMU: `pmap_selftest: PASS`, stabil 12 detik tanpa panic.

## Item 2: IPC bring-up + task pertama — DONE (parsial)

**Status:** Selesai, terverifikasi hardware. IPC PASS penuh. Task: `task_create`
+ `thread_create` terverifikasi; scheduler dispatch belum (blocker
terdokumentasi di bawah).

### Yang diimplementasikan (`kernel/arm/ipc_test.c`, hook di `machdep.c`/`clock.c`)

- **`ipc_selftest()`** (dari `machine_init`, setelah `ipc_bootstrap`/`ipc_init`):
  alokasi port kernel via `ipc_port_alloc_kernel()`, verifikasi
  `ip_references == 1`, dealokasi. Print `ipc_selftest: PASS` tiap boot.
- **`task_selftest()`** (dari `startrtclock`, setelah `task_init`/`thread_init`):
  `task_create(kernel_task, FALSE, &new_task)` → `thread_create` →
  `thread_start` → `thread_doswapin`. Start routine dijalankan synchronous
  (verifikasi code path + print). Print `task_selftest: PASS` tiap boot.

### Temuan / bug

1. **`ipc_space_kernel` memang inactive** — `ipc_space_create_special`
   sengaja set `is_active = FALSE` (special space untuk disembodied rights).
   Test port-set alloc awal gagal dengan `KERN_INVALID_TASK` (16) — bukan
   bug, tapi ekspektasi test yang salah. Test diperbaiki: hanya port
   alloc/dealloc.
2. **`ipc_port_alloc_special` tidak cek `is_active`** dan tidak insert ke
   space table — "port alloc works" awal menyesatkan; verifikasi
   `ip_references` ditambahkan.
3. **Timer interrupt tidak fire di QEMU virt** (blocker scheduler dispatch):
   dicoba PPI 27/virtual-timer, PPI 30/physical-timer, GIC Group 0/1 —
   `ispendr0` tetap 0. `thread_setrun` butuh `current_thread()` valid
   (`active_threads[cpu]`), yang baru ada setelah `load_context()`.
   Defer via timer gagal karena IRQ tidak masuk. Full scheduler dispatch
   (run queue → context switch) = future work.

### Verifikasi
- Build: MI 94/94, MD 17/17 (file baru `ipc_test.c`), LINK OK. 0 patch MI.
- Boot QEMU 3x: `pmap_selftest: PASS`, `ipc_selftest: PASS`,
  `task_selftest: PASS` di semua run, stabil tanpa panic.

## Item 3: Driver virtio-blk — TODO
## Item 4: User mode + syscall interface — TODO
