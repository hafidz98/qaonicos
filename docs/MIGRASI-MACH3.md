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
