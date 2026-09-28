# Analisis Peta Porting Mach → ARMv7 (RV1103 / Luckfox Pico Mini)

Tanggal: 2026-09-26. Task T6 — penutup Fase 1/2.

## 1. Ringkasan eksekutif

Porting kernel Mach ke RV1103 = **from-scratch**, tidak ada jalan pintas.
GNU Mach (1.8.x) hanya punya implementasi machine-dependent untuk **i386 dan
x86_64**; direktori `aarch64/` yang baru (2023–2024) **hanya berisi 15 file
header** — nol file `.c`/`.S`. Tidak ada pmap, trap, atau context switch ARM
yang bisa dipakai ulang. Estimasi realistis: **3–6 bulan engineer
berpengalaman**, konsisten dengan riset awal.

Kabar baiknya: **fondasi bring-up (T1–T5) sudah selesai** dan memetakan
langsung ke ~30% komponen machine-dependent yang dibutuhkan.

## 2. Sumber yang dianalisis

| Sumber | Hasil |
|---|---|
| GNU Mach 1.8.x (mirror jlledom/gnumach, Savannah 500) | Struktur MI/MD dipetakan di bawah. `aarch64/` = skeleton header saja. |
| openmach/openmach (link user) | Mach 4 Utah (1990an), i386-only, mangkrak sejak Des 2014. Tidak ada kode ARM. Nilai: build system autoconf modern + filosofi "machine-specific = master". |
| alvarorichard/FKernel (link user) | Kernel hobby x86-64 (GRUB, C++/Rust). Bukan basis port ARM. |
| darwin-on-arm | **Referensi ARM terbaik**: XNU (turunan Mach 3.0) pernah boot di Nokia N900 (ARMv7). Pola pmap/trap ARMv7-nya bisa dicontek. Dormant ~2017. |
| Prajna/mach (ref user) | Mach 3.0 CMU asli; punya port **alpha & mips** (RISC) — tidak ada ARM, tapi struktur MD-nya untuk RISC berguna sebagai referensi konsep/arsitektur (cara Mach memisahkan pmap/trap per arsitektur). |
| fitzgen/mach (ref user) | Rust binding ke API Mach macOS (**userspace**, bukan kernel) — hanya berguna memahami permukaan API Mach, tidak untuk porting kernel. |
| Device tree RV1103 (T0) | `~/workspace/qaonic_os/hw-addrs.md` — alamat GIC/UART/CRU/GRF/PMU/DRAM terkonfirmasi. |

## 3. Struktur GNU Mach: apa yang MI, apa yang MD

- **Machine-independent** (`kern/`, `ipc/`, `vm/` sebagian, `device/`): IPC,
  task/thread, scheduler, VM framework, exception handling — **dipakai apa
  adanya**, tidak perlu ditulis ulang.
- **Machine-dependent** (`i386/i386/`, 85 file): inilah yang harus ditulis
  ulang untuk ARMv7. Daftar inti + padanannya:

| Komponen i386 (MD) | Padanan ARMv7/RV1103 | Status kita | Estimasi |
|---|---|---|---|
| `com.c`, `kd.c` (serial/konsol) | Driver UART 8250 di `0xff4c0000` | ✅ T1 DONE | — |
| `pit.c`, `hardclock.c` (timer) | ARM generic timer (CP15, PPI 27) | ✅ T3 DONE | — |
| `pic.c`, `apic.c`, `irq.c` | GIC-400 (`0xff1f1000`/`0xff1f2000`) | ✅ T2 DONE | — |
| `locore.S` (boot, vektor) | `vectors.S` + `start.S`, VBAR | ✅ T4/T5 DONE | — |
| `spl.S`, `ipl.h` (intr priority) | Masking via GICC_PMR | ✅ pola ada di T2 | 1 mgg |
| `pmap.c/h` (**tersulit**) | MMU ARMv7: tabel L1 16KB, domain/seksi, TTBR0, DACR | ❌ belum | 4–8 mgg |
| `trap.c/h` | Exception ARM → trap Mach (SWI, prefetch/data abort, IRQ) | ❌ belum | 2–4 mgg |
| `pcb.c/h`, `cswitch.S` | PCB ARM + context switch (register bank, SP_svc/irq) | ❌ belum | 2–3 mgg |
| `fpu.c` | VFPv4/NEON Cortex-A7 (CPACR, FPEXC) | ❌ belum | 1–2 mgg |
| `gdt.c`, `ldt.c`, `idt.c`, `tss.c` | **Hapus** — konsep x86, tidak ada di ARM | n/a | — |
| `smp.c`, `apic` MP | **Hapus** — RV1103 single-core | n/a | — |
| `model_dep.c`, `phys.c` | Init board: baca CNTFRQ, map DRAM 64MB | ⚠️ sebagian (T5) | 1–2 mgg |
| `vm_param.h`, `cpu_number.h`, … | Header `armv7/` baru (mirip skeleton `aarch64/`) | ❌ belum | 1 mgg |
| Build system | `configfrag.ac` + `Makefrag.am` baru untuk `armv7` | ❌ belum | 1–2 mgg |
| Bootstrap & debug hingga boot | Iterasi di board asli (UART log) | ❌ belum | 4–8 mgg |

## 4. Temuan kunci

1. **`aarch64/` GNU Mach = header doang.** Jangan terkecoh — tetap from-scratch.
2. **RV1103 tidak punya timer MMIO** (hasil T0): pakai ARM generic timer —
   satu driver MD yang tidak perlu ditulis.
3. **Single-core**: seluruh kompleksitas SMP/APIC gugur.
4. **U-Boot sudah inisialisasi DRAM+clock**: bring-up kita mulai dari `go`,
   bukan dari MaskROM.
5. **darwin-on-arm** adalah satu-satunya contoh Mach-family yang pernah hidup
   di ARMv7 — pola `pmap` dan `trap`-nya referensi utama saat implementasi.

## 5. Rekomendasi jalur

1. **Urutan implementasi**: header `armv7/` → `pmap` (paling berisiko, kerjakan
   pertama) → `trap` → `pcb`/`cswitch` → `fpu` → integrasi build → debug di
   board via UART.
2. **Butuh hardware**: Pico Mini **A** (microSD). Tanpa board, pmap/trap tidak
   bisa divalidasi — QEMU tidak punya model RV1103.
3. **Strategi debug**: `printf` via UART (T1) + LED/pin GPIO sebagai "breakpoint
   buta" + `db_trace` ala `db_trace.c` i386.
4. **Jangan mulai dari openmach/FKernel** — keduanya tidak memberi kode ARM.
5. **LLVM/Clang (P6)**: setelah bring-up stabil di QEMU, bangun ulang kode
   dengan `clang --target=armv7` sebagai cross-check compiler (menangkap
   GCC-ism) — persiapan integrasi GNU Mach yang upstream-nya mendukung clang.

## 6. Status Fase 1 (T1–T5)

Semua di `~/workspace/qaonic_os/kernel/src/`, lolos compile/syntax check
`arm-none-eabi-gcc -mcpu=cortex-a7`:

- `uart.h/c`, `gic.h/c`, `timer.h/c`, `vectors.S`, `irq.h/c`,
  `rv1103.ld`, `start.S`, `main.c` → `rv1103-hello.elf` (5,9 KB, entry `0x0`).
- Uji QEMU (`-M virt -cpu cortex-a7`): pola boot + UART terbukti jalan.
