# Migrasi QaonicOS ke kernel Mach 3 — catatan

## Fase B flake: panic non-deterministik di `user_selftest` (investigasi Fase C)

### Gejala (laporan Fase B)
- 1x panic pada run ke-7 (dari ~7 run): `data abort` di `user_selftest`,
  `dfar=0x10`, PC tidak dikenali. 5 run verifikasi final bersih.

### Reproduksi (Fase C)
- 20x boot QEMU normal: **0 panic**.
- 30x boot dengan kernel debug timer 1000Hz (10x IRQ pressure, build
  sementara di `/tmp`, lalu di-revert): **0 panic**.
- 100x soak test (kernel + hardening EOI): berjalan background saat
  penulisan — hasil menyusul.
- **Kesimpulan: tidak dapat direproduksi pada kode final Fase B
  (50 run, termasuk 30 dengan tekanan IRQ 10x lipat).**

### Analisis root cause (statik, mendalam)
Kandidat yang diperiksa dan disingkirkan:
1. `need_ast`/`ast_taken` (locore.s `_irq_handler`): `need_ast` tidak
   pernah di-set (hanya disebut di komentar `sched_test.c`), selalu 0
   selama `user_selftest` → `ast_taken` tak pernah terpanggil.
2. IRQ timer saat user mode: L1 user = salinan penuh L1 kernel
   (`pmap_create` bcopy 16KB) → GIC (0x08xxxxxx) dan kernel ter-map →
   handler IRQ aman.
3. `set_ttbr0`: sudah TLBIALL+DSB+ISB → tidak ada stale TLB.
4. `srsdb sp!,#0x13` di vector abort: memakai SP_svc (banked sesuai
   mode tujuan), bukan SP_abt yang tak diinisialisasi → aman.
5. `spl0`/`splx` (machspl.h): `msr cpsr_c` me-restore bit I/F dari
   nilai simpan, bit mode dari CPSR saat ini → benar untuk pemakaian
   se-mode.
6. `pmap_enter`: kritikal section di bawah `splhigh()` → aman dari IRQ.
7. Layout memori user (kode/stack/heap) tidak tumpang tindih;
   validasi range `SYS_WRITE` benar (termasuk anti-wraparound).
8. `uart_putc`, `gic_get_irq`/`gic_eoi`: alamat hardcoded, bersih.
9. Jalur `user_exit_trampoline` (IRQ aktif, TTBR0=user): semua yang
   disentuh handler (GIC, `timer_ticks` di .bss) ter-map di L1 user.

Hipotesis terkuat: flake terjadi pada **intermediate build** selama
sesi Fase B (VM sempat di-reset di tengah sesi; toolchain/env
diperbaiki bertahap: libfuse3, efi-virtio.rom, `-L` datadir). Run ke-7
terjadi sebelum finalisasi; 5 run final + 50 run Fase C bersih.

### Hardening yang diterapkan (Fase C, minimal & aman)
- `kernel/kernel/arm/trap.c` (`TRAP_IRQ`): jangan tulis `GICC_EOIR`
  untuk spurious interrupt (1023). Sebelumnya `gic_eoi(irq)` dipanggil
  tanpa syarat; menulis EOIR=1023 saat tak ada interupsi aktif adalah
  UNPREDICTABLE menurut spesifikasi GICv2. Path timer dan IRQ tak
  dikenal tetap EOI seperti semula.

### Tindak lanjut
- Soak 100x (hasil di bawah). Bila flake muncul lagi, tangkap log
  lengkap (`pc/dfar/dfsr`) dan prioritaskan hipotesis TCG quirk QEMU
  (pernah terjadi di M5: panic→Debugger loop; M4: FSR=0x5 anomali).
- Pertimbangkan watchdog: bila `user_selftest` gagal, boot tetap
  lanjut dengan status FAIL (tidak panic) — future work.

### Hasil soak 100x
**100/100 PASS, 0 panic, 0 fail** (2026-09-28, kernel Fase B final +
hardening EOI spurious). Flake tidak ter-reproduksi.

## Bug trampoline Fase C (ditemukan & difix saat porting userland)

### Gejala
Setelah program user pertama (`init`) exit dengan code 0, kernel
panic: `data abort: pc=... dfar=0x1f dfsr=0x1` (alignment fault di
`user_launch_init`, instruksi `ldr r0, [r4]` dengan `r4=0x1f`).

### Root cause
`user_exit_trampoline` (userasm.s) hanya me-restore `r4`/`lr`
(`pop {r4, lr}`), padahal `user_enter_test` dipanggil dari kode C
yang menaruh variabel live di register callee-saved lain (r5–r11).
Di Fase B kebetulan aman (compiler tidak menaruh variabel live di
sana melewati panggilan); di Fase C, `&nuprog` disimpan compiler di
**r6**, dan setelah `init` exit, r6 berisi sampah (`0x1f`) sehingga
`ldr r0, [r4]` (`r4 = r6`) fault.

Ini bug laten sejak M4 — trampoline bukan return normal sehingga
tidak boleh hanya me-restore sebagian.

### Fix
`userasm.s`: `push {r4-r12, lr}` / `pop {r4-r12, lr}` — simetris,
semua callee-saved di-restore. **Penting:** 10 register = 40 byte
agar stack tetap 8-byte aligned (ARM EABI). Versi awal pakai 9
register (36 byte) → stack misaligned → panic intermiten
(prefetch abort di pc liar) setelah program ke-5 exit. Terverifikasi:
5/5 program (init→ucat→uls→uecho→umon) jalan berurutan hingga halt
bersih.

## Fase D: driver + network di atas Mach 3 (2026-09-28)

Dua commit: `281b5ea` (GPIO/SD/FAT32) + `fc2bd70` (network/HTTP).

### Part 1/2: GPIO + SD + FAT32
- `gpio.c` baru: dual backend (RV1103 asli `#ifdef BOARD_RV1103`,
  mock RAM di QEMU). Syscall 40/41.
- `blk.c` refactor multi-device: `struct blkdev[2]`, identifikasi SD
  via signature boot sector FAT32 (bukan urutan slot).
- `fat32.c` port mekanis; mount `/sd` di `machine_init()`.
  Syscall 52-56. Program user: `ugpio`, `usd`, `ufs`.
- Bug: BSS program user tak ter-map (objcopy tak sertakan NOBITS) →
  `user/pad-bss.py`; ramfs penuh (16→32 file).

### Part 2/2: network + HTTP
- `net.c`: driver virtio-net legacy, polling murni (IRQ dibuang),
  D-cache maintenance eksplisit (D-cache ON di port ini).
- `netstack.c`: ARP/IPv4/ICMP. `tcp.c`: TCP satu-koneksi port 80.
  `http.c`: HTTP/1.0 (GET / dashboard, GET /metrics).
- `netmain.c`: server loop setelah 8 program user, tak kembali.
- Ping ke 10.0.2.2 (QEMU user-net host) BERHASIL.
- **Keterbatasan jujur**: TCP SYN dari host via `hostfwd=tcp::18080-:80`
  belum diterima guest (RX path OK untuk ICMP, tapi SYN tak sampai).
  Investigasi lanjut diperlukan (kemungkinan NAT QEMU atau filter MAC).

### Pelajaran
- QEMU user-net: host = 10.0.2.2 (bukan 10.0.2.1 yang untuk tap).
- D-cache ON → semua DMA (virtio queue + buffer) butuh clean/invalidate
  eksplisit; `used->idx` volatile + invalidate per baca.
- `dcache_*_range` di blk.c dijadikan non-static untuk dipakai net.c.

### Investigasi instabilitas Fase D (2026-09-28, belum selesai)

**Gejala**: kernel panic non-deterministik (prefetch abort ke alamat
sampah 0x0/0x8/heap, kadang undefined instruction, kadang spsr korup).
Fase C (commit caf52ad) stabil 10/10; Fase D (343d131) crash ~20-100%
tergantung konfigurasi.

**Hasil bisect** (5 run per konfigurasi):
- net dimatikan (halt): 4/5 OK, 1/5 panic → bug BUKAN spesifik net.
- net+fat32+gpio dimatikan: 3/5 OK, 2/5 panic → bug di core.
- 5 program (tanpa ugpio/usd/ufs): 4/5 OK, 1/5 panic → bukan jml program.
- Crash terjadi di: user_selftest (fault test), launch_uprog (kmem_alloc/
  pmap_enter), net_init (vq_setup), server loop (net_poll).

**Hipotesis yang sudah disingkirkan**:
- `dcache_inval_range` pakai DCIMVAC (buang dirty tanpa write-back) →
  diubah ke DCCIMVAC (clean+invalidate), TETAP crash 3/3. Bukan ini.
- `e->id` descriptor vs buffer index di net_poll → kode sudah pakai
  `d=bi` dan guard `bi < RX_NBUF`. Bukan ini.
- `need_ast`/preemption → tak pernah di-set. Bukan ini.
- QEMU/TCG/host → bare-metal minimal stabil 543 baris. Bukan host.

**Dugaan tersisa**: korupsi memori non-deterministik (heap/stack),
atau bug emulasi QEMU 8.2.2 (TCG) pada fitur spesifik (MMU/TLB,
cache ops, atau virtio). Perlu investigasi dengan GDB stub QEMU.

**Catatan**: ping ke 10.0.2.2 BERHASIL (ICMP RX path OK). Bug TCP asli
(SYN via hostfwd tak sampai) belum bisa diuji karena blocker ini.

---

### Investigasi GDB stub (2026-09-28, subagent qaonic-mach3-tcpfix)

**Crash signatures** (12+ run, semua non-deterministik):
- `undefined instruction at pc=0x40296eb8` (= `_user_test_ksp`, .bss)
- `undefined instruction at pc=0x40298b92` (.bss, odd address)
- `undefined instruction at pc=0x417e9800` / `0x41816000` (heap)
- `prefetch abort: pc=0x0` / `0x4` / `0x8` / `0x70`
- `prefetch abort: pc=0x102960` / `0x120000` (user VA!)
- `data abort: pc=0x4000987c dfar=0x400537ea dfsr=0x1` (alignment fault,
  r6 korup 0x40298a04 → 0x400537ea = nilai lama r5)

**Temuan GDB** (break di `user_exit_trampoline`, `panic`, trap handler):
- `_user_test_ksp` VALID (0x40078f78 / 0x417c0f78, alamat stack sah),
  hanya ditulis 2x oleh `str sp,[r4]` di `user_enter_test`.
- Save area 40-byte VALID saat pre-pop (lr = 0x4000bd58, alamat kode sah).
- Crash terjadi SETELAH trampoline kembali (pesan PASS terpotong
  mid-printf), atau di lokasi lain (sched_selftest, net_main).
- PABT trap frame: spsr=0x411c0fd3 (SVC), sp 4 byte short dari ekspektasi
  post-pop → pop {r4-r12,lr} seolah tak komplit, atau sp korup.
- r6 (callee-saved, live across call) korup oleh thread_create/
  thread_start/thread_doswapin — nilai baru = nilai lama r5.

**Hipotesis yang disingkirkan via test build** (6 run per konfigurasi):
- Timer IRQ dimatikan: 6/6 TETAP crash → bukan IRQ.
- blk+gpio+fat32 dimatikan: 6/6 TETAP crash → bukan driver Fase D.
- QEMU cmdline lama (1 blk, tanpa net): 6/6 TETAP crash → bukan device.
- D-cache dimatikan: 6/6 TETAP crash → bukan koherensi D-cache.
- I-cache + D-cache dimatikan: 3/3 TETAP crash → bukan I-cache.
- `set_ttbr0` sudah TLBIALL → bukan TLB stale.

**ROOT CAUSE KETEMU (2026-09-28)**: `_irq_handler` (dan 5 handler
lain) di `locore.s` tidak menyimpan `lr_svc` sebelum `bl
arm_trap_handler`.  `bl` selalu menimpa `lr`, sehingga kalau IRQ
menyela kode kernel yang sedang di dalam leaf function (mis.
`uart_putc`, return via `bx lr`), `lr` yang kembali sudah korup
(menunjuk ke tengah `_irq_handler`).  `bx lr` lalu mengeksekusi
`pop {r0-r12}` + `rfefd sp!` dengan stack yang salah -> `pc` =
sampah (kebetulan user VA 0x102960 sekali waktu) -> prefetch abort
non-deterministik.  Cocok dengan semua signature: crash di tengah
`printf`, `r6` <- nilai lama `r5` (off-by-one sejenis), dan fakta
bahwa menonaktifkan SATU sumber IRQ tak cukup (timer DAN virtio-blk
sama-sama bisa memicu).

**Bisect yang menentukan**: caf52ad 10/10 stabil; 281b5ea 9/10
(1 crash: `launch_uprog: creating taspanic: prefetch abort:
pc=0x102960`); setelah fix 10/10 bersih.

**Fix**: semua 6 exception handler kini `push {r0-r12, lr}` /
`pop {r0-r12, lr}`; `struct arm_trap_frame` tambah field `svc_lr`
(di antara `r[13]` dan `lr`).  Akses `frame->lr`/`frame->spsr`
yang ada tetap benar.
