# M2-REPORT — Mach 3 port: seluruh MI ter-compile dengan clang

Tanggal: 2026-09-28. Branch: `mach3-port`.

## 1. Hasil akhir

**94/94 file MI ter-compile menjadi object file, 0 error** (warning
diperbolehkan; default warning set hanya 28 warning di 94 file).

| Dir | .o | Isi |
|---|---|---|
| `kern` | 35 | task, thread, scheduler, lock, clock, bootstrap |
| `ipc` | 17 | IPC, port, MIG runtime |
| `vm` | 10 | VM, vm_object, pager interface |
| `device` | 11 | device interface |
| `ddb` | 21 | kernel debugger |
| **Total** | **94** | |

Perintah: `mach3/build-mi.sh` (make-less; idempoten; butuh `MACH3_SRC`
menunjuk ke `~/workspace/mach3-src`, default `$HOME/workspace/mach3-src`).
Toolchain: clang 18 `--target=arm-none-eabi`, `-O1 -fno-builtin`.

**Nol patch ke source MI** — `git status` di `mach3-src` menunjukkan
tidak ada file modified. Seluruh adaptasi via flag + header generate.
Rincian tiap keputusan ada di `mach3/MI-PATCHES.md`.

## 2. Dari mana header config di-generate (dan apa yang kita lakukan)

Build asli memakai tool BSD `config` yang membaca:

- `kernel/conf/MASTER` — daftar `options FOO` MI (27 opsi) +
  `pseudo-device cpus N` → tiap opsi jadi `OPTIONS/foo.h`
  berisi `#define FOO <0|1>`, dan `cpus.h` berisi `#define NCPUS N`;
- `kernel/conf/<arch>/MASTER` — `platform XXX` → `platforms.h`
  (`#define XXX 1`); opsi & pseudo-device khusus mesin;
- `kernel/conf/files` — daftar file otoritatif + marker `optional`
  (file hanya di-build bila opsi terkait on).

M2 mereplika ini dengan `mach3/tools/gen_config.py` → `build/gen/`:

- **39 header**: 31 header opsi (`mach_fixpri.h`, `simple_clock.h`,
  `stat_time.h`, `mach_kdb.h`, … — nama = lowercase nama opsi),
  `cpus.h` (`NCPUS 1`), `platforms.h` (`ARM`, `ARMV7`, `QEMU_VIRT`,
  `RV1103`), `bootstrap_symbols.h`, `cmucs_disk.h`, `dli.h`,
  `mach_mp_debug.h`/`mach_lock_mon.h`/`time_stamp.h` (untuk satu file
  legacy), `md_globals.h` (deklarasi global MD yg dipakai MI).
- **Nilai opsi** (single-core ARMv7, QEMU `-M virt`; tiap nilai
  didokumentasikan rasionalnya di script):
  ON: `MACH_ASSERT`, `MACH_FIXPRI`, `MACH_HOST`, `MACH_KDB`,
  `MACH_COUNTERS`, `MACH_PAGEMAP`, `SIMPLE_CLOCK`, `STAT_TIME`.
  OFF: sisanya, terutama semua `NORMA_*`, `MACH_DEBUG`,
  `MACH_PCSAMPLE`, `XPR_DEBUG`, `FAST_TAS` (butuh dukungan trap MD —
  M3), `POWER_SAVE` (butuh hook MD — M3), `MACH_MACHINE_ROUTINES`.
- **`sys/` compat** (build asli punya dir `sys/` berisi header BSD;
  kita sediakan versi minimal freestanding): `sys/varargs.h`
  (interface varargs lama di atas compiler builtin — **shim
  compile-only**, lihat §5), `sys/stdargs.h` (= `<stdarg.h>`),
  `sys/reboot.h` (flag `RB_*` untuk `kern/bootstrap.c`).

Daftar source dihitung `mach3/tools/mi_sources.py` yang mem-parse
`conf/files` dan menghormati `optional` (bukan sekadar `*.c`).

## 3. MIG: tidak tersedia → stub konsisten

Tidak ada implementasi MIG di environment ini (CMU MIG maupun GNU
MIG); mem-porting MIG adalah proyek tersendiri. Untuk target
compile-only M2, `mach3/tools/gen_mig_stubs.py` menulis stub yang
signaturenya diturunkan langsung dari `.defs`:

| Stub | Sumber | Isi |
|---|---|---|
| `mach/memory_object_user.h` | `mach/memory_object.defs` | 10 routine (init/terminate/copy/data_request/…/change_completed) |
| `mach/memory_object_default.h` | `mach/memory_object_default.defs` | 2 routine (create, data_initialize) |
| `device/device_reply.h` | `device/device_reply.defs` | 5 routine prefix `ds_` (userprefix) |
| `device/device_pager_server.c` | — | server dispatcher placeholder (di-`#include` sebagai source oleh `dev_pager.c`) |

Aturan derivasi (terverifikasi terhadap call site di `vm/*.c` dan
`norma/xmm_server.c`): `simpleroutine` → `kern_return_t`; `ctype:`
dipakai verbatim; argumen `polymorphic` → tambahan
`mach_msg_type_name_t` setelah port; `data : pointer_t` →
pasangan `(pointer_t data, mach_msg_type_number_t dataCnt)`
(karena `std_types.defs`: `pointer_t = ^array[]`); SEQNOS off.

## 4. Kelas error yang ditemui & solusi (semua via flag/build)

1. `implicit-int` jadi hard error di clang ≥16 → `-Wno-implicit-int`.
2. Konversi pointer↔int pervasif & disengaja (1993) → `-Wno-int-conversion`.
3. Deklarasi fungsi implisit (558×) → `-Wno-implicit-function-declaration`
   (temuan M1, dikonfirmasi).
4. `<sys/varargs.h>`/`<sys/reboot.h>` tidak ada di glibc → header
   generate (§2).
5. `<device/device_pager_server.c>` & `<device/device_reply.h>` adalah
   output MIG → stub §3.
6. `boothowto` dipakai MI tapi didefinisikan di MD tanpa deklarasi
   (build 1993 mengandalkan deklarasi implisit) → `md_globals.h`
   via `-include`.
7. `kern/server_loop.c` butuh `-DSERVER_NAME` dari build → flag per-file.
8. Header `<machine/*.h>`/`<mach/machine/*.h>` = **stand-in mips**
   (sengaja; pola validasi M1). Header mips terverifikasi bebas
   inline-asm di header, jadi aman untuk compile-only ARM.

## 5. Blocker tersisa untuk M3 (tidak dikerjakan di M2)

1. **Header MD ARM asli** — `mach3/kernel/arm/` masih kerangka dok M1;
   23+8 header harus ditulis nyata (tipe `pmap_t`, `thread` MD fields,
   `spl`, …). Ini inti M3.
2. **MIG asli** — stub §3 hanya deklarasi; butuh MIG jalan atau
   server/marshalling tulis-manual sebelum link.
3. **Konversi varargs → `<stdarg.h>`** — `sys/varargs.h` adalah no-op
   compile-only; `kern/{printf,debug,sscanf,bootstrap}.c` +
   `ddb/db_output.c` harus dikonversi ke ANSI agar `printf` (jalur
   bring-up utama) benar-benar jalan di ARM/AAPCS.
4. **Implementasi MD** (`locore.s`, `trap.c`, `pmap.c`, `pcb.c`,
   `context.s`, `clock.c`, `gic.c`, `uart.c`, …) — adaptasi dari
   `rv1103-bringup/` per kerangka M1.
5. Link + boot QEMU `-M virt -cpu cortex-a7`.

## 6. File yang di-exclude (terdokumentasi, bukan kegagalan)

`ipc/mach_debug.c`, `kern/pc_sample.c`, `kern/xpr.c`, `vm/vm_debug.c`
(opsi terkait off — sesuai semantik `conf/files` upstream);
`kern/lock_mon.c` (include `<mach/i386/vm_types.h>`),
`kern/profile.c` (merujuk field `struct thread` yang telah dihapus) —
keduanya dead code bahkan di upstream (tidak ada di `conf/files`).

## 7. Verifikasi M2

- [x] `build-mi.sh` dari tree bersih: 94 passed, 0 failed
- [x] 94 file `.o` non-kosong di `build/obj/`
- [x] `git status` mach3-src: nol modifikasi (tidak ada patch MI)
- [x] Pass `-Wall` informasional: hanya pola khas kode 1993
      (`-Wextra-tokens` 6279× dari `#endif FOO`, implicit decl 558×)
- [x] `MI-PATCHES.md` + laporan ini ditulis
- [ ] Commit `mach3-port`: "Mach3 port M2: MI compiles with clang"
