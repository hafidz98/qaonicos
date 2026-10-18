# Utang Teknis Qabot — 2026-09-29

## 1. TCP keep-alive (flaky)
**Status:** DOKUMENTASI LIMITASI — workaround aktif.

Respons HTTP tanpa FIN menetes lambat/parsial di guest. Root cause di
`tcp.c:tcc_on_data` / interaksi slirp belum diinvestigasi tuntas.

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
