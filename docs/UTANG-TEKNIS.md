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
**Status:** BUTUH VERIFIKASI RUNTIME.

Laporan App A4: `TIME_GET` kembali `0x6abba840` setelah
`TIME_SET(0x6abf5a00)` (selisih ~51 hari — bukan error pembulatan).

Inspeksi kode (`kernel/kernel/arm/user.c:780-791`):
- `time_unix_set = a0` (unsigned, langsung)
- `TIME_GET = time_unix_set + (ticks_now - ticks_set)/100`
- `arm_timer_ticks()` = 100 Hz (terdokumentasi di http.c)

Kode terlihat benar untuk aritmetika unsigned. Kandidat:
- `sys_display_simple(69u, ...)` di ulib memotong argumen, atau
- NTP daemon salah panggil, atau
- korupsi memori di sekitar `time_unix_set`.

**Target:** uji runtime dengan nilai known-good, trace a0 di syscall.

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
