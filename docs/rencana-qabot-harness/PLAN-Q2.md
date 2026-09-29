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

## 4. Detail desain Q2c (ditulis 2026-09-29, sebelum implementasi)

Kontrak yang dipakai ulang: `qb_provider.chat()` mengembalikan satu baris
`TOOL:nama k=v ...` atau `FINAL:...` (lihat `user/qabot/loop.c`). Provider
real hanya mengganti *isi* chat(): JSON→HTTPS→JSON, lalu format ulang ke
kontrak baris itu. `qb_run_task`, policy gate, dan tool registry Q1 tidak
berubah.

### 4.1 `user/qabot/json.c` (tanpa lib eksternal, buffer statis)
- `qb_json_escape(dst, src)`: escape `"` `\` dan kontrol → `\n` `\r` `\t`.
- `qb_build_request(h, out, outlen)`: serialize history (role user/
  assistant/tool) + skema tool dari `qb_tool_at(i)` (name+desc, type
  object, properties kosong — mock mengabaikan). Contoh:
  `{"model":"qabot-mock","messages":[{"role":"user","content":"..."}],
  "tools":[{"type":"function","function":{"name":"gpio_read",
  "description":"...","parameters":{"type":"object"}}}]}`
- `qb_parse_response(body, tc_out, final_out)`: cari `"tool_calls"` di
  body; bila ada ambil `"name":"..."` pertama lalu `"arguments":"..."`
  (unescape `\"` minimal), parse pasangan `"k":"v"` flat menjadi
  `qb_toolcall`; bila tidak ada, ambil `"content":"..."` pertama sebagai
  FINAL. Return 1 = toolcall, 0 = final, -1 = gagal parse.
- Semua buffer di .bss pemanggil (request 4KB, respons 4KB).

### 4.2 `user/qabot/provider_real.c`
- `qb_real_chat(ctx, h)`: (1) baca API key via `cfg_load()` + cari KV
  `llm.key`; bila NVS kosong/rusak pakai kunci uji default (SD QEMU
  bermasalah — `blk_selftest: FAIL (no device)` — jadi NVS tidak bisa
  diandalkan di sandbox; di hardware real pakai NVS). (2) `tls_init()` +
  `tls_connect(10.0.2.2:18444, ca_pem, "qabot-test.local")`. (3) Bangun
  HTTP POST `/v1/chat/completions` (Host, Authorization: Bearer,
  Content-Type, Content-Length) via `qb_snprintf`. (4) `tls_write` lalu
  `tls_read` sampai header lengkap + `Content-Length` body terpenuhi.
  (5) `qb_parse_response` → format `TOOL:...`/`FINAL:...` ke buffer
  statis, return pointer-nya. Timeout/abort: `tls_read` mengembalikan
  `TLS_ERR_NET_*`/timeout → return `"FINAL:(provider error)"` agar
  guardrail loop Q1 yang menangani.
- `struct qb_real_ctx { char key[72]; }` + `qb_real_init()`.

### 4.3 `user/uqabotr.c` (daemon uji, one-shot seperti utlscli)
- Stack 8 page (TLS + JSON). Alur: `sys_time_set(FAKE_NOW)` (waktu cert),
  `qb_real_init`, `qb_run_task(&r, "baca pin 40", &qb_real_provider)`,
  cetak `QABOT: PROVIDER_REAL PASS` bila `r.status == QB_DONE` dan event
  log mengandung eksekusi tool + FINAL dari server.
- Skenario mock: prompt → server balas tool_calls `gpio_read pin=40` →
  qabot eksekusi tool real → kirim hasil → server balas content final.

### 4.4 `tools/mock_openai_https.py` (host)
- HTTPS server di 127.0.0.1:18444, cert `user/tls/testcerts/server.crt`
  (SAN qabot-test.local, dari CA test yang sama). Scripted 2 langkah:
  POST #1 → `{"choices":[{"message":{"tool_calls":[...]}}]}`;
  POST #2 (body mengandung "role":"tool") → `{"choices":[{"message":
  {"content":"Pin 40 terbaca."}}]}`. Validasi `Authorization: Bearer`
  longgar (terima kunci uji default maupun NVS).

### 4.5 Build & uji
- `kernel/build-md.sh`: section baru `uqabotr` — compile `qabot/*.c`
  (history tools policy provider provider_real loop eventlog json) +
  `cfg/cfg.c` + `utlscli`-style link (`$TLSOBJS`, `ca_pem.o`, ulib);
  embed limit 1MB; entry 0x100000; pad-bss.
- `daemon_images`: `uqabotr` 8 page. Catatan tcc 1-koneksi: selama uji
  Q2c, `utlscli` dinonaktifkan sementara (keduanya one-shot TLS;
  bergantian, bukan bareng).
- Kriteria: `QABOT: PROVIDER_REAL PASS` di log QEMU.
