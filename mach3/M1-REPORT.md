# M1-REPORT — Mach 3 port: inventarisasi + kerangka MD ARM + probe compile

Tanggal: 2026-09-28. Branch: `mach3-port`. Commit menyusul di bawah.

## 1. Source Mach 3 asli

- `https://github.com/Prajna/mach` = **CMU Mach 3.0 asli**, ter-clone ke
  `~/workspace/mach3-src` (18MB, persisten — bukan /tmp).
- Referensi hardware RV1103 `~/workspace/luckfox-pico-mini-b`: **belum ada**
  saat M1 berjalan (clone oleh parent mungkin masih berjalan) — M1 tidak
  membutuhkannya; alamat hardware sudah ada di `hw-addrs.md` + `dts/`.

## 2. Inventarisasi MI (machine-independent)

| Direktori | File | ± Baris | Isi |
|---|---|---|---|
| `kernel/kern` | 72 | 30.811 | task, thread, scheduler, lock, clock, bootstrap |
| `kernel/ipc` | 37 | 23.278 | IPC, port, MIG runtime |
| `kernel/vm` | 21 | 20.800 | VM, vm_object, pmap API |
| `kernel/mach` | 49 | 5.654 | header publik + `.defs` (MIG) |
| `kernel/device` | 38 | 11.243 | device interface |
| `kernel/ddb` | 33 | 9.657 | kernel debugger |
| **Total MI** | **±250** | **±101k** | |

## 3. MD API surface (yang WAJIB disediakan tiap port)

Dipelajari dari `kernel/mips/` (port referensi). MI me-referensikan MD
lewat dua mapping include:

**`<machine/*.h>` → `mach3/kernel/arm/`** (23 header):
`asm.h`, `asm_linkage.h`, `ast.h`, `ast_types.h`, `cpu.h`, `cpu_number.h`,
`db_machdep.h`, `db_trace.h`, `kttd_machdep.h`, `lock.h`, `mach_param.h`,
`machine_routines.h`, `machspl.h`, `mp/mp.h`, `pmap.h`, `pte.h`, `regdef.h`,
`sched_param.h`, `setjmp.h`, `thread.h`, `time_stamp.h`, `timer.h`,
`vm_tuning.h`, `xpr.h`

**`<mach/machine/*.h>` → `mach3/kernel/mach/arm/`** (8 header):
`boolean.h`, `exception.h`, `kern_return.h`, `machine_types.defs`,
`syscall_sw.h`, `thread_status.h`, `vm_param.h`, `vm_types.h`

**File sumber MD** (dimodelkan dari `kernel/mips/`): `locore.s`
(vektor + `_start`), `trap.c`, `pmap.c`, `pcb.c`, `context.s`,
`arm_init.c` (startup), `clock.c`, `machdep.c`, `copy.s`
(bcopy/copyin/copyout), `fpu.c`, `gic.c`, `uart.c`.

Kerangka stub (44 file, tiap file menjelaskan apa yang wajib disediakan
+ file `rv1103-bringup/` mana yang diadaptasi) sudah ada di
`mach3/kernel/arm/` dan `mach3/kernel/mach/arm/`.

## 4. Probe compile (clang 18 `--target=arm-none-eabi`, MD stand-in = mips)

File yang dicoba: `kern/ast.c` (331 baris), `vm/vm_object.c` (3433 baris),
`ipc/ipc_kmsg.c`. Setup: `-I` ke `mach3-src/kernel`, symlink
`machine/→kernel/mips`, `mach/machine→kernel/mach/mips`, stub header
config di dir terpisah.

**Hasil: kode MI pada dasarnya ANSI C bersih — dengan 2 flag,
`vm_object.c` compile 0 error.** Pola masalah yang ditemukan:

1. **`-DKERNEL` WAJIB.** Tanpa ini, `mach/vm_param.h` sengaja meledak
   ("YOU HAVE MADE A MISTAKE BY INCLUDING THIS FILE"). Build M2 harus
   selalu pass `-DKERNEL`.
2. **Implicit function declarations = satu-satunya error kelas berat.**
   `ast.c`: 2 error (`panic`, `net_ast`); `vm_object.c`: 19 error
   (`panic` ×banyak, `bzero`, `printf`, `Debugger`, `memory_object_*`
   dari MIG); `ipc_kmsg.c`: 7 error (`bcopy`, `vm_allocate`, …).
   Semua hilang dengan `-Wno-implicit-function-declaration`.
   (Kode 1990-an ditulis sebelum C99; gcc 2.x memakluminya.)
3. **Header config hasil generate** (`cpus.h`, `mach_fixpri.h`,
   `norma_ipc.h`, `platforms.h`, `simple_clock.h`, `stat_time.h`,
   `mach_counters.h`, `mach_ldebug.h`, `mach_debug.h`, `mach_host.h`,
   `mach_kdb.h`, `power_save.h`, `xpr_debug.h`, … ±30 file): di-generate
   tool `conf/` dari MASTER files. M2: stub manual atau port tool config.
4. **Header MIG** (`mach/memory_object_user.h`,
   `mach/memory_object_default.h`, …): di-generate MIG dari `.defs`.
   M2 butuh MIG yang bisa jalan (atau stub + implementasi manual).
5. **Warning dominan (bukan error):** `-Wextra-tokens` (`#endif FOO`
   gaya lama, ~90×), `-Wdeprecated-non-prototype` (`void f()` tanpa
   prototipe, ~49×), `-Wformat` (4×). Tidak ada definisi fungsi K&R.
6. Beberapa header `machine/` tidak ada bahkan di mips
   (`sched_param.h`, `lock.h`, `cpu_number.h`, …) — port boleh
   menyediakannya sendiri; bukan masalah.

## 5. Rencana M2

1. Build system: Makefile clang dengan mapping include (§3), flag
   `-DKERNEL -Wno-implicit-function-declaration`, stub ±30 header config.
2. Selesaikan MIG: cari/port MIG (atau generate stub + tulis manual
   fungsi yang dipakai MI).
3. Tulis MD ARM nyata per file kerangka, adaptasi 1:1 dari
   `rv1103-bringup/`: `pmap.c` (sudah short-descriptor), `trap.c`,
   `switch.S`→`context.s`, `vectors.S`→`locore.s`, `fpu.c`, `gic.c`,
   `uart.c`, `timer.c`→`clock.c`, `pcb.c`.
4. Target: seluruh MI terkompilasi (belum link) dengan 0 error.
5. (M3) Link + boot QEMU `-M virt -cpu cortex-a7`.

## 6. Verifikasi M1

- [x] Clone `~/workspace/mach3-src` OK (18MB)
- [x] Inventarisasi MI (§2)
- [x] Kerangka MD ARM 44 file (§3)
- [x] Probe compile + pola error terdokumentasi (§4)
- [x] `mach3/README.md` — MI tidak di-copy ke repo (18MB), hanya path + cara pakai
