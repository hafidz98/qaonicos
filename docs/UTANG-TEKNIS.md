# Utang Teknis Qabot — 2026-09-29

## 1. TCP keep-alive (flaky)
**Status:** INVESTIGASI SELESAI 2026-10-19 (F1) — limitasi lingkungan slirp,
workaround dipertahankan.

**Hasil investigasi F1:**
- Logika RX guest (`tcc_on_data`: buffer, seq check, ACK) **benar** —
  diverifikasi via code review + instrumentasi kernel (counter
  accept/drop). Tidak ada bug deterministik di `tcp.c`.
- Plain TCP keep-alive 1 roundtrip: **5/5 PASS** (`utcpkeep`).
- TLS keep-alive 2 roundtrip: **~75% PASS** (`utlskeep`). Gagal berupa
  stall satu arah (mock→guest) 20 dtk: data menetes parsial
  (176B, 313B) lalu berhenti; `drops=0` di guest (bukan out-of-order).
- Bug mock yang diperbaiki selama investigasi: single-threaded
  `TCPServer` nyangkut di koneksi mati (→ handshake timeout palsu),
  `Content-Length` salah di test.
- Karakterisasi: stall satu arah + `drops=0` + intermiten menunjuk ke
  interaksi slirp/host (ACK/segmen tertunda di NAT user-mode QEMU),
  bukan logika TCP guest. Tanpa `tcpdump` di VM, penyebab paket-level
  tak bisa dipastikan.

**Keputusan:** `Connection: close` tetap sebagai pola resmi untuk
jalur dev/test QEMU (deterministik). Di hardware produksi, TCP/TLS
diterminasi di ESP32-C6 — stack TCP guest hanya untuk dev/test.

**Workaround (Q4):** `proxy_horde.py` tutup koneksi tiap respons
(`Connection: close`); `provider_real.c` pakai mode `qr_plain`
(tanpa keep-alive). Andal untuk polling Horde.

**Target:** investigasi RX path + pacing ACK, atau tetapkan
"tanpa keep-alive" sebagai limitasi resmi.

## 2. sys_time_set (TIME_GET < TIME_SET)
**Status:** RESOLVED — BUG TIDAK TERBUKTI ADA (2026-10-18, F2).

Investigasi F2: inspeksi kernel (`user.c`: `time_unix_set = a0`,
`TIME_GET = set + dt/100`), wrapper ulib, daemon NTP, dan
`arm_timer_ticks()` (100 Hz) — semua benar. Regression test baru
`user/utimeset.c` (one-shot, jalan tiap boot): `TIME_SET(X)` →
`TIME_GET == X` bit-exact untuk 3 nilai known-good
(`0x6abf5a00`, `1790683200`, `0x12345678`) — **UTIMESET: PASS**.

Root cause laporan: kemungkinan salah catat heksadesimal.
`1790683200` (FAKE_NOW yang dipakai uji Q2b) = **`0x6abba840` persis** —
digit yang sama dengan "nilai salah" di laporan. Tidak ada bug di
syscall; tidak ada perbaikan kode yang dibutuhkan selain pencabutan
bypass.

**Perubahan F2:**
- `user/tls/tls_port.c`: bypass hardcode `1790683200L` di
  `tls_time_get()` DICABUT — kini pakai `sys_time_get()` beneran.
- Verifikasi: `UTLSCLI: PASS` (handshake TLS 1.2 vs `openssl s_server`,
  validasi cert penuh via jalur jam real). Upaya pertama FAIL timeout
  (flaky TCP, lihat §1), retry bersih PASS.
- `user/utimeset.c`: regression test permanen (one-shot tiap boot).

**Sisa:** uji end-to-end daemon NTP tak bisa di sandbox (UDP diblokir) —
perlu verifikasi di lingkungan dengan jaringan real.

## 3. mbedtls_strerror hang
**Status:** DIPERBAIKI (2026-09-29).

`tls_err()` di `user/tls/tls_api.c` kini pakai formatter hex minimal
tanpa libc (`-0xXXXXXXXX`), menggantikan `mbedtls_strerror` yang
dugaan hang di `snprintf` bare-metal.

## 4. Entropy / TRNG
**Status:** DOKUMENTASI — BUTUH HARDWARE.

`mbedtls_hardware_poll` pakai timer + xorshift (LEM AH). Di RV1103
fisik, WAJIB pakai TRNG hardware untuk TLS yang aman. Tanpa TRNG,
kunci sesi TLS dapat diprediksi.

**Target:** driver TRNG RV1103 saat porting hardware; sampai saat itu,
TLS hanya untuk uji/QEMU, bukan produksi.

## 5. ufs mkdir FAIL (filesystem)
**Status:** FIXED 2026-10-19 (commit dc6eb3a).

**Root cause:** bukan bug filesystem — `/sd/T` sudah ada dari boot
sebelumnya (SD image persisten antar test manual). `fat32_mkdir`
benar menolak bikin direktori yang sudah ada.

**Perbaikan:** `fat32_mkdir` kini idempoten — return 0 bila entry sudah
ada dan berupa direktori, -1 bila berupa file. Verifikasi: `[ufs] ok`.

**Catatan:** shell `qaon>` perintah `ls`/`cat` masih gagal baca filesystem
— investigasi terpisah (kemungkinan path handling di shell, bukan driver).

## 6. QABOT S2 FAIL (test expectation usang)
**Status:** FIXED 2026-10-19 (commit 29b595c).

Skenario S2 (`user/qabot/main.c`): ekspektasi `confirm: auto-yes`, tapi
kode (`policy.c`) mencetak `confirm: ya (mode uji)`. Eksekusi
`gpio_write` sendiri BERHASIL (`exec: gpio_write -> pin=5 val=1 ok`).

**Perbaikan:** update ekspektasi test ke string baru. Verifikasi:
`QABOT: 3/3 PASS`.

## 7. Panic boot dengan virtio-gpu
**Status:** BUG AKTIF — investigasi parsial 2026-10-19.

Boot dengan `-device virtio-gpu-device`: face jalan (`display 240x240 ok`,
screenshot berhasil), tapi kemudian `panic: data abort: pc=0x4000cbec
dfar=0x4c6c530c dfsr=0x805` di shell `qaon>`, lalu `halting cpu`.
Boot tanpa GPU stabil.

**Investigasi 2026-10-19:**
- Fault di kernel mode (bukan user) — `dfar` konsisten `0x4c6c530c`
  antar-boot, `dfsr` bit 10 set = write fault.
- Alamat `0x4c6c530c` di luar RAM QEMU (0x40000000-0x44000000) —
  wild pointer atau buffer overflow deterministik.
- Bukan di path UART (`UART0_BASE=0x09000000`) atau framebuffer
  (`gpu_backing` static, sudah terbukti jalan via screenshot).
- **Konteks:** bug spesifik QEMU virtio-gpu. Hardware Pico Mini pakai
  display SPI (ST7789), bukan virtio-gpu — kemungkinan tidak terdampak
  di hardware.

**Target:** debug dengan gdb stub QEMU (`-s -S`) untuk stack trace;
atau tunda sampai hardware (verifikasi GUI di hardware pakai SPI).
