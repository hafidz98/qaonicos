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

## Item 2: IPC bring-up + task pertama — TODO
## Item 3: Driver virtio-blk — TODO
## Item 4: User mode + syscall interface — TODO
