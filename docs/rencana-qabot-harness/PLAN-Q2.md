# PLAN — Qabot Q2: TCP client + TLS (mbedTLS) + provider real

**Track:** Qabot harness · **Tanggal:** 2026-09-29 · **Status:** riset selesai, implementasi Q2a berjalan
**Dasar:** `docs/riset-agent-runtime-harness/RINGKASAN.md` §7 (prasyarat: TCP client + TLS)

## 1. Tujuan Q2

Qabot bisa memanggil **LLM provider real via HTTPS** (API OpenAI-compatible).
Karena sandbox tak punya internet, verifikasi end-to-end dilakukan vs
**server mock di host** (HTTP → TLS → OpenAI-compatible), yang di-sandbox
terjangkau via slirp (10.0.2.2). Yang ke internet real tinggal ganti
hostname + CA bundle.

## 2. Sub-fase

### Q2a — TCP client (kernel) + uji vs HTTP server host
- `kernel/kernel/arm/tcp.c`: tambah koneksi client `tcc` (duplikat pola
  server, tanpa refactor server yang sudah terbukti):
  state CLOSED → SYN_SENT → ESTABLISHED → FIN; retransmit SYN via
  `tcp_tick`; demux di `tcp_on_ip` berdasar dport (80 = server,
  sport ephemeral = client); ring buffer RX 4KB; `netstack_arp_learn`
  dari SYN-ACK.
- Syscall **73–77**: `SYS_TCP_CONNECT` (r0=dst_ip, r1=dst_port),
  `SYS_TCP_STATUS`, `SYS_TCP_SEND`, `SYS_TCP_RECV`, `SYS_TCP_CLOSE`
  + wrapper ulib + `docs/SYSCALL-ABI.md`.
- `user/utcpcli.c` (one-shot): connect 10.0.2.2:18080 → `GET /` →
  baca respons → `UTCPCLI: PASS`. Verifikasi: TCP guest→host via slirp
  jalan di sandbox.

### Q2b — port mbedTLS (TLS client di userspace)
- Download mbedTLS 3.6 LTS; subset ±25 file
  (ssl_tls, ssl_client, x509_crt, asn1, oid, pk, rsa, ecdsa, ecdh, aes,
  gcm, sha256/512, md, cipher, ctr_drbg, entropy, pem, bignum, ecp,
  ecp_curves, dhm, error, platform).
- `user/tls/`: `mbedtls_config.h` custom (tanpa FS/threading/NET_C;
  `MBEDTLS_PLATFORM_MEMORY` → bump allocator di BSS qabot ±128KB;
  time → `sys_time_get`; entropy → `mbedtls_hardware_poll` dari timer
  — **LEMAH, hanya untuk uji**; catat butuh TRNG di HW nyata).
- BIO = callback `mbedtls_ssl_set_bio` → syscall TCP 73–77
  (non-blocking + `sys_yield` saat `MBEDTLS_ERR_SSL_WANT_READ/WRITE`).
- `user/utlscli.c` (one-shot): handshake TLS 1.2 vs `openssl s_server`
  di host (CA test di-embed, validasi chain penuh) → kirim/ping →
  `UTLSCLI: PASS`.
- Target ukuran: < 512KB per bin (embed limit dinaikkan khusus program TLS).

### Q2c — JSON + provider real di qabot
- `user/qabot/json.c`: builder request chat/completions (history + skema
  tool) + parser minimal respons (choices[0].message.content/tool_calls
  → `qb_toolcall`). Tanpa lib eksternal.
- `provider_real` di `provider.c`: HTTPS POST via mbedTLS; API key dari
  NVS (`cfg` KV `llm.key`, sudah ada sejak A3); retry/timeout/abort.
- Uji end-to-end: mock server OpenAI-compatible di host (HTTPS, cert
  dari CA test) → skenario: qabot kirim prompt → server balas tool_calls
  → qabot eksekusi tool real → kirim hasil → server balas final.
  `QABOT: PROVIDER_REAL PASS`.

## 3. Kriteria selesai Q2

- [ ] Q2a: `UTCPCLI: PASS` (guest→host TCP via slirp)
- [ ] Q2b: `UTLSCLI: PASS` (handshake TLS 1.2 + validasi cert vs s_server)
- [ ] Q2c: `QABOT: PROVIDER_REAL PASS` (loop penuh via HTTPS mock)
- [ ] HTTP dashboard lama tetap jalan (regresi server TCP)
- [ ] Commit branch `qabot/q2-tls-provider`
- [ ] DEVLOG.md terisi per langkah

## 4. Risiko & catatan jujur

- TCP stack = satu koneksi server + satu client; tanpa window scaling.
  Cukup untuk pola request/response API.
- Entropi dari timer = lemah (hanya uji). Di Luckfox real butuh sumber
  TRNG — riset terpisah saat bawa ke HW.
- mbedTLS di QEMU: belum ada, ini port pertama — config diset minimal,
  yang gagal dikompilasi di-disable eksplisit dan dicatat.
- Verifikasi internet real (api.openrouter.ai) tak bisa dari sandbox;
  tinggal ganti hostname + CA bundle, kode sama.
