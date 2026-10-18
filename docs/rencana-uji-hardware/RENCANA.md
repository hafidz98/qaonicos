# Rencana Uji Hardware (via PC-agent)

**Tanggal:** 2026-10-18 · **Status:** plan, menunggu hardware
**Tujuan:** daftar LENGKAP uji yang TIDAK BISA jalan di sandbox/QEMU dan
harus dieksekusi di hardware real. Dieksekusi oleh **PC-agent** (coding
agent di PC Hafidz) begitu hardware tersedia; hasil dilaporkan balik
dengan format §5.

**Prinsip:** tiap uji = ID, tujuan, prasyarat hardware, langkah,
kriteria PASS, format laporan. Uji yang gagal → catat di
`docs/UTANG-TEKNIS.md`, bukan di-fix diam-diam.

## H0. Prasyarat umum (semua fase)

- Toolchain build QaonicOS di PC (lihat `~/workspace/toolchain/README.md`
  atau build native): `clang`, `lld`, `llvm-objcopy`, QEMU (opsional).
- Repo `qaonicos` branch `qabot/q2-tls-provider`, HEAD ter-push.
- Kabel USB-C (power + flashing), USB-to-UART (debug console UART2).
- Cara flash + baca console: dokumentasikan perintah persis yang dipakai.

## H1. Luckfox Pico Mini saja (tanpa co-MCU)

| ID | Uji | Prasyarat | Langkah | PASS bila |
|---|---|---|---|---|
| H1.1 | NTP end-to-end | Jaringan real (UDP tidak diblokir) | Enable daemon `ntp` di `daemon_images[]`; boot; tunggu ≤5 menit; `time` di shell | `time` ≈ jam real (±60 dtk); log `ntp: unix=... (diset)` |
| H1.2 | GPIO real | Kabel jumper + LED/multimeter | `gpio write <pin> 1/0`, `gpio get <pin>` di shell | LED nyala/mati sesuai; baca kembali konsisten |
| H1.3 | Display SPI | LCD ST7789 240×240 ter-wiring | Boot; face tampil | Face Qabot tampil benar (tidak corrupt); idle → display off |
| H1.4 | Power FSM | Catu daya terukur (opsional) | Menu Power → Sleep Now; bangunkan via tombol | Sleep/wake jalan; tidak hang |
| H1.5 | Multi-boot stability | — | Boot 10× berurutan, catat tiap hasil self-test | 10/10 boot tanpa FAULT; marker PASS konsisten |
| H1.6 | Jam maju (tick) | — | `time` → catat; tunggu 60 dtk; `time` lagi | Selisih 55–65 dtk (tick 100 Hz jalan) |

## H2. Pico Mini + ESP32-C6 (companion link)

Prasyarat tambahan: modul ESP32-C6-WROOM-1 ter-wiring ke **UART4/UART5 +
RTS/CTS** (bukan UART2 = debug console); firmware FW-1 ter-flash;
dokumen `docs/riset-esp32c6-companion/RINGKASAN.md` §5 (protokol v2).

| ID | Uji | Langkah | PASS bila |
|---|---|---|---|
| H2.1 | Baud 921600 | Set kedua sisi 921600; kirim pola acak 1 MB via UART bolak-balik | 0 byte corrupt; bandingkan checksum |
| H2.2 | WIFI.SCAN/CONNECT | `WIFI.SCAN` → pilih SSID → `WIFI.CONNECT` + `WIFI.PASS` | `OK CONNECTED`; `WIFI.LIST` tersimpan |
| H2.3 | NET.HTTP real | `NET.HTTP GET https://example.com` (atau API test) | `OK 200` + body lengkap, tidak terpotong |
| H2.4 | TIME.GET via C6 | Bandingkan `TIME.GET` dengan jam HP | Selisih < 5 dtk |
| H2.5 | TLS ke server real | qabotd `provider_real` + API key (Q3) → 1 prompt | Respons model kembali; tidak ada error cert |
| H2.6 | VOICE uplink | `VOICE.START 16000 16`; bicara 10 dtk; `VOICE.STOP` | Frame count ≈ 160000 sampel; tidak ada CRC error; audio bisa di-playback di PC |
| H2.7 | TOOL.DISPATCH | Kirim JSON `gpio_read` dari sisi C6 (simulasi cloud) | Tool dieksekusi di RV1103; hasil kembali utuh |

## H3. Peran ganda C6: Muse Gadget (BLE)

Prasyarat: HP dengan aplikasi Muse (pairing sesuai SDK); firmware FW-3.

| ID | Uji | Langkah | PASS bila |
|---|---|---|---|
| H3.1 | BLE pairing | Scan dari aplikasi Muse → pair `MuseGadget-*` | Pairing sukses; bond tersimpan |
| H3.2 | Koeksistensi | WiFi connect (H2.2) + BLE connect bersamaan 10 menit | Tidak ada drop; SRAM tidak overflow (cek log) |
| H3.3 | Footprint SRAM | Build FW-3; catat `idf.py size` | Total < 512 KB HP SRAM dengan margin ≥ 32 KB |

## H4. Audio input RV1103 (driver, pasca-F3)

Prasyarat: mic ter-wiring ke pin `CODEC_MIC0P/MICBIAS/CODEC_MIC1P`
(sesuai skematik); driver audio selesai (fase implementasi F3).

| ID | Uji | Langkah | PASS bila |
|---|---|---|---|
| H4.1 | ADC capture | Rekam 10 dtk @16 kHz mono via syscall audio | File 320000 byte; tidak ada overrun; noise floor wajar |
| H4.2 | Uplink penuh | H4.1 → kirim via VOICE (H2.6) → STT | Transkrip dikenali |

## Format laporan PC-agent (§5)

Setiap uji dilaporkan sebagai blok teks:

```
UJI <ID>: PASS/FAIL
  HW: <board + wiring singkat>
  CMD: <perintah persis yang dijalankan>
  OUT: <output relevan, maks 15 baris>
  CATATAN: <anomali / timing / foto bila perlu>
```

Kirim laporan per fase (H1, H2, …), bukan per uji — biar tidak
berisik. FAIL wajib sertakan OUT lengkap + langkah reproduksi.

## Traceability

| Uji | Asal |
|---|---|
| H1.1 | F2 sisa (UTANG-TEKNIS.md §2) — sandbox blokir UDP |
| H1.5 | Pending 2026-09-29 (laporan PC-agent) |
| H2.1 | F5 Q1 (baud 921600) |
| H2.2–H2.4 | RENCANA.md §4 (protokol UART v1) |
| H2.5 | Q3 deferred (API key real milik Hafidz) |
| H2.6–H2.7 | RINGKASAN.md §5 (protokol v2) |
| H3.1–H3.3 | F5 Q2 (satu firmware, ukur footprint) |
| H4.1–H4.2 | F3 (driver audio, implementasi terpisah) |
