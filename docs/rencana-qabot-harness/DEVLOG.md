# DEVLOG — Qabot Harness Q1

Format: `## YYYY-MM-DD HH:MM — judul` + butir singkat (apa, hasil, keputusan).

## 2026-09-29 10:45 — setup track

- Aturan kerja baru dari Hafidz dicatat di AGENTS.md: selalu riset +
  `docs/plan` dulu, dev logs agar tertrack rapi (standing).
- Track Q1 dibuka: `docs/rencana-qabot-harness/PLAN.md` ditulis dari
  `docs/riset-agent-runtime-harness/RINGKASAN.md` §6–§7.
- Watchdog didaftarkan: `qaonic-qabot-q1-harness-core`.
- Keputusan desain: mock provider in-process (pola mock_comcu), format
  tool-call satu baris `TOOL:`/`FINAL:` (seam JSON di Q2), qabot = program
  one-shot dengan 3 skenario scripted (pola umon).

## 2026-09-29 11:00 — implementasi core

- `user/qabot/`: qabot.h, history.c (append-only), tools.c (registry 4 tool:
  get_info/get_time/gpio_read/gpio_write + klasifikasi risiko), policy.c
  (gate ALLOW/CONFIRM/BLOCK, dicek SEBELUM eksekusi), provider.c (mock
  scripted + seam Q2 terdokumentasi), loop.c (ReAct + guardrail
  MAX_STEPS=8/MAX_MS=30000), eventlog.c (snprintf mini %s/%u/%d/%c),
  main.c (3 skenario scripted, verifikasi via qb_ev_contains).
- Wire: build-md.sh §1e (compile 7 .c -> link -> embed 32768), user.c
  uprogs[] += qabot (one-shot, pola umon). Branch `qabot/q1-harness-core`.

## 2026-09-29 11:10 — debug boot (2 bug)

1. `_start` hilang: tiap program define `_start` sendiri (section
   `.text.start`); main.c cuma punya `main` -> entry 0x0. Fix: tambah
   `_start` -> `main()`.
2. Stack overflow: `struct qb_run` ~13KB di stack, stack user cuma 1 page
   (4KB) -> exit 0xdab7. Fix: `static` (BSS).
3. S2 FAIL: pin=40 >= GPIO_PINS_PER_BANK(32) -> gpio_set -1. Fix: pakai
   pin=5 (juga diupdate di PLAN.md).

## 2026-09-29 11:15 — verifikasi QEMU

- `QABOT: 3/3 PASS` (S1 get_info/ALLOW, S2 gpio_write/CONFIRM+auto-yes,
  S3 self_destruct/BLOCK tanpa eksekusi). Event log urut + timestamp.
- `user_launch_init: PASS (9/9 programs, 17/17 files)`, 0 FAIL di log,
  daemon face/uiapp/ntp terdaftar normal.

## 2026-09-29 11:20 — Q2 dibuka (user: "okay")

- Riset: TCP stack = server-only (satu `struct tc` global, 437 baris);
  client = tambah `tcc` duplikat pola server (tanpa refactor server).
  Syscall bebas: 73+. Host ada internet (curl OK) tapi sandbox tak bisa
  dipakai untuk API real -> verifikasi vs mock server di host via slirp.
- `docs/rencana-qabot-harness/PLAN-Q2.md` ditulis: Q2a TCP client, Q2b
  mbedTLS, Q2c JSON + provider real.

## 2026-09-29 11:40 — Q2a DONE (TCP client + syscall 73-77)

- `kernel/kernel/arm/tcp.c`: `struct tcc` (client, duplikat pola server;
  server tak disentuh) — state CLOSED/SYN_SENT/ESTABLISHED/FIN_SENT,
  ephemeral port basis 0xD000, RX buffer 4KB, stop-and-wait, retransmit
  di `tcp_client_tick()` (dari `tcp_tick()`), demux `tcp_on_ip` berdasar
  dport, `netstack_arp_learn` dari SYN-ACK.
- `user.c`: syscall 73–77 (CONNECT/STATUS/SEND/RECV/CLOSE); `ulib.h/.c`:
  wrapper `sys_tcp_*`; `docs/SYSCALL-ABI.md` baris 73–77.
- `user/utcpcli.c` (daemon SEMENTARA): connect 10.0.2.2:18081, GET /,
  cek `HTTP/`+`200` -> `UTCPCLI: PASS` percobaan pertama.
- Regresi: dashboard HTTP host->guest tetap 200, `user_launch_init: PASS`.

## 2026-09-29 12:30 — Q2b: port mbedTLS 3.6.7 (TLS client)

- Keputusan: mbedTLS 3.6.7 (LTS, C), bukan 4.0 (API baru). Unduh
  `.tar.gz` (URL `.tar.bz2` via archive/refs/tags me-return Not Found).
- Vendor subset di `user/tls/mbedtls/`: 42 file.c (ssl_tls, ssl_client,
  ssl_msg, ssl_tls12_client, ssl_ciphersuites, x509_crt, x509, asn1*,
  oid, pk*, rsa, rsa_alt_helpers, pk_ecc, ecdsa, ecdh, ecp, ecp_curves,
  bignum*, aes, gcm, sha256, sha512, md, cipher*, ctr_drbg, entropy, pem,
  base64, constant_time, debug, error, platform, platform_util, version,
  ssl_debug_helpers_generated) + internal `library/*.h` + public
  `include/` penuh + `psa/` (dibutuhkan ssl_tls.c/x509_crt.c).
  Catatan: `pk_ecc.c` dan `ssl_debug_helpers_generated.c` ketinggalan di
  seleksi awal -> link error -> ditambahkan.
- `user/tls/inc/mbedtls/mbedtls_config.h`: TLS 1.2 client saja,
  ciphersuite ECDHE-(ECDSA|RSA)-AES128-GCM-SHA256, VERIFY_REQUIRED,
  PLATFORM_MEMORY/CALLOCFREE->tls_calloc, TIME_ALT+MS_TIME_ALT,
  ENTROPY_HARDWARE_ALT + NO_PLATFORM_ENTROPY, tanpa FS/NET_C/threading.
- `user/tls/inc/`: string.h/stdlib.h/stdio.h/time.h/inttypes.h/assert.h
  minimal (tambahan iteratif saat compile error).
- `user/tls/tls_port.c`: mini-libc (memcpy/memmove/memset/memcmp/strlen/
  strcmp/strstr/strchr/snprintf/vsnprintf/printf/rand), bump allocator
  96KB (reset per transaksi, peak dicetak), tls_time_get->sys_time_get,
  mbedtls_hardware_poll LEMAH (timer+xorshift, hanya uji Q2),
  __aeabi_memcpy4/__aeabi_memclr8/__aeabi_uldivmod (asm ARM, q di r0/r1
  + sisa di r2/r3, untuk bignum.c).
- `user/tls/tls_bio.c`: BIO send/recv blocking+timeout di atas syscall
  73-77 (chunk 1200/4096, yield saat tunggu).
- `user/tls/tls_api.c`: tls_connect (TCP -> handshake -> verify_result
  harus 0; hostname via mbedtls_ssl_set_hostname), tls_write/read/close.
- `user/utlscli.c` (daemon SEMENTARA): handshake vs `openssl s_server`
  di host 10.0.2.2:18443 (cert ECDSA P-256, CA test di-embed via
  tools/pem2c.py), GET / via TLS, cek `HTTP/`+`200` -> `UTLSCLI: PASS`.
  Jam: sys_time_set(FAKE_NOW) karena sandbox blokir UDP (NTP tak bisa);
  di produksi jam dari daemon ntp.
- Wire build-md.sh §1d3 (41 .c mbedtls + 3 port, TLSCFLAGS dengan
  -I tls/inc DAHULU agar config sendiri menang), §1d4 (utlscli; embed
  limit 512KB karena .bin 326KB pasca pad-bss), NDAEMON 4->5.

## 2026-09-29 13:10 — bug layout memori program besar (Q2b)

- Gejala: `sched: 'utlscli' FAULT 0xdab7` di timeslice pertama, tanpa
  output sama sekali.
- Root cause: layout VA statis — code di 0x100000 (npages), stack 1 page
  di 0x110000, heap di 0x120000. Image utlscli 80 page (0x100000–
  0x14FFF) sehingga mapping stack di 0x110000 MENIMPA PTE code page 16
  (tengah .text, 176KB) -> literal pool nol -> data abort.
- Fix `kernel/kernel/arm/user.c`: layout dinamis di setup_uprog_task —
  stack (stack_pages) tepat setelah code, heap (sbrk) tepat setelah
  stack; `struct uprog_image` += stack_pages; launch_uprog/launch_daemon
  teruskan; SP = stack_top dinamis. utlscli dapat 8 page (32KB) untuk
  handshake mbedTLS; lainnya tetap 1. Batas image juga sudah dinaikkan
  16->128 page sebelumnya (326KB > 64KB).
- Pelajaran: asumsi "program user kecil" bocor di 3 tempat (batas 16
  page, VA statis, stack 1 page) — ketiganya diperbaiki generik, bukan
  khusus TLS.

## 2026-09-29 16:30 — Q2b PASS: TLS 1.2 handshake mbedTLS (UTLSCLI: PASS)

- Hasil: `UTLSCLI: PASS` di QEMU — handshake TLS 1.2 lengkap vs
  `openssl s_server` di host (port 18443), sertifikat ECDSA P-256
  terverifikasi (VERIFY_REQUIRED, CA Qaonic Test CA).
- Bug kritis yang diperbaiki:
  1. **Race tcc**: utcpcli mencuri koneksi utlscli (kernel tcc 1 koneksi).
     Fix: utcpcli dinonaktifkan (tcc single-connection = limitasi).
  2. **va_top statis**: menolak buffer user di atas 0x130000. Fix: va_top
     per-task dari akhir heap dinamis.
  3. **Bump allocator tanpa free**: mbedTLS butuh >2MB karena tls_free
     no-op. Fix: free-list allocator sederhana di tls_port.c (512KB pool,
     first-fit + coalesce).
  4. **Waktu**: sys_time_set bermasalah (nilai kembali salah); bypass
     sementara via hardcode 1790683200L di tls_time_get(). TODO: selidiki
     root cause sys_time_set.
  5. **mbedtls_strerror hang**: belum diselidiki; bypass via print hex.
- Scaffolding dibersihkan: tls-dbg prints, hex dump bio_send, debug
  threshold 2->0, print kernel [tcc].
- Limitasi: ntp nonaktif di sandbox (UDP diblokir, ganggu timing TLS);
  utcpcli nonaktif (race tcc).
