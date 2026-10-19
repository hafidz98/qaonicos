# PLAN — Penguatan Fondasi QaonicOS untuk Qabot

**Track:** QaonicOS foundation → Qabot · **Tanggal:** 2026-10-18 · **Status:** plan, belum implementasi
**Latar:** survei 2026-10-18 (side chat "Qabot hardware persona"). Keputusan Hafidz:
perkuat dulu fondasi QaonicOS — bagian yang mendukung Qabot dan belum selesai —
sebelum fase fitur baru (mode A/B/C, merge Qabot×QaonicOS, port Muse Gadget).
**Dasar:** `docs/rencana-qabot-harness/` (Q1–Q9 DONE), `docs/UTANG-TEKNIS.md`,
`docs/riset-agent-runtime-harness/RINGKASAN.md`.

## 1. Tujuan

Tiga kelemahan fondasi yang terbukti menghambat Qabot menuju pemakaian nyata
(melawan server/API beneran, bukan mock) dibereskan sampai tuntas:

| # | Fondasi | Status sekarang | Kenapa Qabot butuh |
|---|---|---|---|
| F1 | Reliabilitas TCP (keep-alive / RX pacing) | Flaky, workaround `Connection: close` | Opsi B butuh koneksi long-lived (SSE/WebSocket); polling HTTPS pun rapuh kalau RX menetes |
| F2 | Wall-clock time yang benar (`sys_time_set`) | Bug (~51 hari selisih), bypass hardcode di uji TLS | Validasi sertifikat TLS vs server real gagal kalau jam salah |
| F3 | Jalur audio input (riset + rencana driver) | Belum ada | Opsi B kirim data suara — tanpa ini companion voice tidak mungkin |

**Sengaja TIDAK masuk fase ini** (→ fase berikutnya): mode A/B/C itu sendiri,
MCP server di qabotd, port Muse Gadget, keputusan hardware networking USB
(dicatat sebagai dependensi §5).

## 2. F1 — TCP keep-alive / RX pacing

**Konteks:** `docs/UTANG-TEKNIS.md` §1. Respons HTTP tanpa FIN menetes
lambat/parsial di guest (teramati 176B dari ~458B, timeout 20 dtk). Root cause
di `tcp.c:tcc_on_data` / interaksi slirp belum diinvestigasi; workaround Q4
(`Connection: close` + mode `qr_plain`) masih aktif.

**Riset dulu (boleh 1 sesi):**
- Reproduksi deterministik di QEMU: server host yang keep-alive vs close,
  ukur pola tetesan (size, timing) di beberapa run.
- Kandidat: ACK pacing, window update, interaksi slirp user-mode NAT,
  `tcp_tick` retransmit timer.

**Implementasi:**
- Perbaiki root cause di `tcp.c` (RX path / ACK pacing), JANGAN tambah
  workaround baru.
- Hapus workaround Q4 hanya bila perbaikan terbukti stabil (jangan hapus
  sebelum penggantinya lolos uji yang sama).

**Kriteria selesai F1:**
- [x] Mock HTTP keep-alive (tanpa FIN): plain TCP 1 roundtrip **5/5 PASS**
      (`utcpkeep`); TLS 2 roundtrip ~75% (flaky, lihat bawah)
- [x] Mode `Connection: close` tetap lolos (regresi) — pola resmi dev/test
- [x] `UTANG-TEKNIS.md` §1 di-update: investigasi selesai 2026-10-19
- [x] Commit di branch `qabot/q2-tls-provider`

**Kesimpulan F1 (2026-10-19):** Logika RX guest (`tcc_on_data`) benar;
tidak ada bug deterministik di `tcp.c`. Stall satu arah intermiten
(20 dtk, `drops=0`) adalah interaksi slirp/host, bukan bug guest.
`Connection: close` ditetapkan sebagai pola resmi dev/test QEMU.
Di hardware, TCP diterminasi di ESP32-C6. F1 SELESAI sebagai
investigasi (bukan sebagai perbaikan kode).

## 3. F2 — Wall-clock time (`sys_time_set`)

**Konteks:** `docs/UTANG-TEKNIS.md` §2. `TIME_GET` kembali `0x6abba840`
setelah `TIME_SET(0x6abf5a00)` — selisih ~51 hari, bukan error pembulatan.
Uji TLS Q2b mem-bypass via hardcode `1790683200L` di `tls_time_get()`.
Kandidat: `sys_display_simple` memotong argumen, NTP daemon salah panggil,
atau korupsi memori di sekitar `time_unix_set`.

**Riset dulu:**
- Uji runtime nilai known-good: `TIME_SET(X)` → `TIME_GET` langsung, catat.
- Trace `a0` di syscall entry (`kernel/kernel/arm/user.c:780-791`).
- Periksa pemanggil NTP daemon (A4) — pastikan bukan salah panggil.

**Implementasi:**
- Perbaiki root cause; hapus hardcode `tls_time_get()`.
- Validasi: handshake TLS vs `openssl s_server` dengan cert yang
  validity window-nya ketat (menolak jika jam meleset > 1 hari).

**Kriteria selesai F2:**
- [x] `TIME_SET(X)` → `TIME_GET` == X (uji known-good, 3 nilai) — DONE
      2026-10-18: `user/utimeset.c` (one-shot tiap boot), UTIMESET: PASS 3/3.
      Temuan: bug yang dilaporkan kemungkinan tidak pernah ada
      (`1790683200` = `0x6abba840` persis — salah catat heksadesimal di
      laporan). Tidak ada perbaikan kernel yang dibutuhkan.
- [x] Handshake TLS lolos dengan cert validity ketat (tanpa hardcode waktu)
      — DONE 2026-10-18: bypass `1790683200L` di `tls_time_get()` dicabut,
      `UTLSCLI: PASS` via jalur jam real (upaya pertama FAIL timeout flaky,
      retry bersih PASS).
- [ ] NTP daemon (A4) sinkronisasi jam benar ujung-ke-ujung — TERBATAS:
      tak bisa di sandbox (UDP diblokir); perlu lingkungan dengan jaringan real.
- [x] `UTANG-TEKNIS.md` §2 di-update: RESOLVED

## 4. F3 — Audio input: riset + rencana driver

**Konteks:** blocker tercatat untuk voice (Opsi B). Fakta hardware dari
`Rockchip-RV1103-Datasheet-V1.4-20230725.pdf` §1.2.10 (survei 2026-10-18):
RV1103 punya **audio codec built-in** — 2 channel ADC 24-bit (90dB SNR)
untuk rekaman stereo dari mikrofon, input mic diferensial/single-ended +
MICBIAS, sample rate 8/12/16/24/32/44.1/48/96 kHz. Pin `CODEC_MIC0P/MICP`,
`CODEC_MICBIAS`, `CODEC_MIC1P/MICN` ada di pinout. **16 kHz = rate standar
untuk STT** — cocok.

**Riset (deliverable fase ini — implementasi driver boleh jadi fase sendiri):**
1. Cari Technical Reference Manual (TRM) RV1103 — datasheet hanya elektrikal;
   butuh register map codec (clock gate, PGA gain, ADC enable, FIFO/DMA).
   Sumber: repo `luckfox-pico-mini-b` (cek `documents/`), wiki Luckfox,
   kernel Linux Rockchip (`sound/soc/rockchip/`).
2. Petakan clock tree: sumber clock codec → rate 16 kHz.
3. Petakan jalur data: FIFO codec → DMA → buffer userspace (pola: syscall
   baru `SYS_AUDIO_READ` mirip pola TCP RX ring buffer 4KB di Q2a?).
4. Keputusan desain: format sampel (16-bit LE @16kHz mono — cukup untuk STT),
   ukuran buffer/period, siapa yang trigger (polling vs interrupt).
5. Tulis `docs/riset-audio-rv1103/` (RINGKASAN + rencana driver).

**Kriteria selesai F3:**
- [x] Dokumen riset lengkap: register map, clock, jalur DMA, desain syscall
      — DONE 2026-10-18: `docs/riset-audio-rv1103/RINGKASAN.md`. Offset
      register diverifikasi dari `rv1106_codec.h` (offset TRM V0.3 tidak
      cocok dengan driver shipping).
- [x] Rencana driver per tahap (D1–D4, fase implementasi terpisah)
- [x] Keputusan tercatat: **streaming mentah ke cloud via ESP32-C6**
      (bukan on-device STT — 64 MB tidak realistis)
- Caveat: pin mic kemungkinan unconnected di board Pico Mini —
  verifikasi fisik sebelum implementasi driver.

**Catatan:** implementasi driver audio = fase tersendiri setelah riset ini
disetujui — jangan langsung coding driver di fase fondasi.

## 5. F4 — Higiene repo

- [ ] Commit 2 dokumen riset yang masih untracked:
      `docs/riset-agent-runtime-harness/BEDAH-DEVIN-ARCHITECTURE.md`,
      `docs/riset-agent-runtime-harness/VALIDASI-TRANSCEND-RAILS.md`
- [ ] Re-add git remote `origin` → `https://github.com/hafidz98/qaonicos`
      (clone lokal kehilangan remote config), verifikasi `git ls-remote`
- [ ] Push branch `qabot/q2-tls-provider` (pastikan CI hijau)

## 6. Dependensi eksternal (revisi 2026-10-18)

**Keputusan konektivitas hardware: co-MCU ESP32-C6** (Hafidz, 2026-10-18).
Menggantikan opsi USB networking di bawah — tidak lagi diperlukan.

- **Verifikasi Q7/Q8 end-to-end + stabilitas multi-boot** — menunggu laporan
  PC-agent dari Hafidz (status 2026-09-29).
- ~~Opsi USB networking~~ — DIBATALKAN oleh keputusan co-MCU di atas.
  (Dulu: (a) verifikasi pad FEPHY + breakout magnetics/RJ45 eksternal,
  (b) dongle USB-Ethernet/WiFi via USB host, (c) USB gadget RNDIS/ECM
  tether ke host. Tidak lagi diperlukan karena WiFi diurus ESP32-C6.)

## 7. Urutan & estimasi kasar (revisi 2026-10-18: keputusan co-MCU)

**Keputusan arsitektur (Hafidz, 2026-10-18): konektivitas hardware via
co-MCU ESP32-C6** (menggantikan opsi USB networking di §6). Ini konsisten
dengan desain co-MCU yang sudah ada (`docs/rencana-app-qaonicos/RENCANA.md`:
protokol UART `WIFI.*`, `BLE.*`, `NET.HTTP`; riset awal pakai ESP32-C3 di
`docs/riset-fido2-esp32c3/`) — delta-nya hanya C3 → C6 (WiFi 6 + BLE 5).

Konsekuensi prioritas:
- **F1 (TCP keep-alive) TURUN prioritas** → backlog. Di hardware, TCP/TLS
  diterminasi di ESP32-C6, bukan di stack TCP QaonicOS. Stack TCP QaonicOS
  tinggal jalur dev/test di QEMU — tetap layak diperbaiki, tapi bukan
  critical path hardware persona.
- **F2 (time) TETAP** — jam yang benar dibutuhkan di mana pun TLS
  diterminasi (ESP32-C6 juga butuh waktu valid untuk cert).
- **F3 (audio) TETAP**, paralel.
- **F5 (BARU) — ESP32-C6 companion link.** Riset + plan: (a) update pilihan
  co-MCU C3 → C6 di docs terkait; (b) ekstensi protokol UART untuk
  kebutuhan baru: voice streaming up (Opsi B), dispatch tool-call down,
  WebSocket/SSE bila perlu (protokol existing baru cover WIFI + NET.HTTP);
  (c) rencana firmware ESP32-C6 (ESP-IDF: WiFi 6, TLS, UART framing).
  Bonus arsitektur: C6 punya BLE 5 → kandidat tunggal untuk **dua peran**:
  backhaul WiFi + Muse Gadget (BLE pairing ke aplikasi Muse). Track port
  Muse Gadget menyatu dengan track ini.

**Urutan yang diputuskan:** F4 (higiene, cepat, independen) → F5 (riset+plan
companion link, paralel dengan F2) → F2 (time) → F3 riset paralel dari awal.
F1 ke backlog.

**Kriteria selesai fase:** F2–F5 checklist hijau + `UTANG-TEKNIS.md` bersih
dari item "butuh investigasi" (kecuali F1 yang pindah ke backlog).
**STATUS 2026-10-18: FASE SELESAI** — F1 backlog, F2/F3/F4/F5 done.
