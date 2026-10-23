# MI-PATCHES.md — catatan patch terhadap source Mach 3 asli (M2)

Prinsip M2: **flag compiler dulu, patch source hanya bila perlu**.
Hasil: **tidak ada satu pun file di `~/workspace/mach3-src` yang diubah**
(terverifikasi via `git status`: nol file modified).

Seluruh adaptasi dilakukan di luar tree source, lewat:

## 1. Flag compiler (build-mi.sh)

| Flag | Alasan |
|---|---|
| `-DKERNEL` | WAJIB (temuan M1): `mach/vm_param.h` sengaja `#error` tanpa ini |
| `-Wno-implicit-function-declaration` | Kode 1990-an memanggil fungsi tanpa prototipe (558× di -Wall); gcc 2.x memaklumi |
| `-Wno-implicit-int` | clang ≥16 menjadikannya hard error; pola `foo(a) { ... }` tanpa tipe tersebar di MI |
| `-Wno-int-conversion` | Konversi pointer↔int disengaja & pervasif (mis. kernel mengoper `ipc_port_t` ke stub MIG bertipe `mach_port_t`); gcc 2.x diam saja |
| `-Wno-deprecated-non-prototype` | Definisi K&R `void f()` tanpa prototipe |
| `-Wno-extra-tokens` | Gaya lama `#endif FOO` (~6300×) |
| `-fno-builtin` | MI mendefinisikan sendiri `bcopy`/`bzero`/dll; cegah konflik builtin clang |
| `-include build/gen/md_globals.h` | Deklarasi global MD yang dipakai MI (`boothowto`) tanpa header yang mendeklarasikannya (build 1993 mengandalkan deklarasi implisit) |
| `-DSERVER_NAME="mach_kernel"` | Khusus `kern/server_loop.c` (template generik; aslinya tiap server di-build dengan -D sendiri) |

## 2. Header hasil generate (bukan patch source)

- `tools/gen_config.py` → `build/gen/*.h`: ±39 header config
  (`cpus.h`, `mach_*.h`, `platforms.h`, `sys/{varargs,stdargs,reboot}.h`,
  `md_globals.h`). Mereplika output tool BSD `config` atas
  `kernel/conf/MASTER` dengan nilai untuk target ARMv7 UP/QEMU-virt
  (lihat M2-REPORT.md §2).
- `tools/gen_mig_stubs.py` → `build/gen/{mach,device}/*`: stub
  compile-only pengganti output MIG (`memory_object_user.h`,
  `memory_object_default.h`, `device_reply.h`,
  `device_pager_server.c`). Signature diturunkan dari `.defs`
  (aturan: ctype, argumen `polymorphic` → tambahan
  `mach_msg_type_name_t`, `pointer_t` → pasangan `data,dataCnt`,
  SEQNOS off). Diverifikasi terhadap call site aktual di
  `vm/*` dan `norma/xmm_server.c`.

## 3. Daftar source (tools/mi_sources.py)

Mem-parse `kernel/conf/files` (otoritatif upstream) dan menghormati
marker `optional`:

| File | Status | Alasan |
|---|---|---|
| `ipc/mach_debug.c` | skip | `optional mach_ipc_debug`, opsi off |
| `kern/pc_sample.c` | skip | `optional mach_pcsample`, opsi off |
| `kern/xpr.c` | skip | `optional xpr_debug`, opsi off |
| `vm/vm_debug.c` | skip | `optional mach_vm_debug`, opsi off |
| `kern/lock_mon.c` | skip | tidak di `conf/files`; include `<mach/i386/vm_types.h>` (dead code upstream) |
| `kern/profile.c` | skip | tidak di `conf/files`; merujuk field `struct thread` yang sudah dihapus (`profil_buffer`, `thread_profiled`) |
| `kern/server_loop.c` | **compile** | tidak di `conf/files`, tapi kompilabel dengan `-DSERVER_NAME` |

## 4. Tech debt / pekerjaan M3 (bukan patch M2)

1. **MIG asli belum ada.** Stub di §2 hanya deklarasi; marshalling IPC
   sesungguhnya butuh MIG yang jalan (atau server ditulis manual).
2. **`sys/varargs.h` adalah shim compile-only.** `va_start` didefinisikan
   sebagai no-op karena `__builtin_va_start` menolak fungsi non-variadic
   dan trik stack BSD lama tak valid di ARM/AAPCS. File pemakai
   (`kern/{printf,debug,sscanf,bootstrap}.c`, `ddb/db_output.c`) **harus**
   dikonversi ke ANSI `<stdarg.h>` sebelum bisa jalan benar di M3 —
   `printf` adalah jalur bring-up utama.
3. **Header `<machine/*.h>` masih stand-in mips** (sengaja, sesuai batas
   M2). Header ARM asli adalah inti pekerjaan M3.
