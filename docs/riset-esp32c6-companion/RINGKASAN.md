# Riset: ESP32-C6 sebagai Companion Link QaonicOS

**Tanggal:** 2026-10-18 · **Track:** PLAN-FONDASI.md F5 · **Status:** riset + plan, belum implementasi
**Keputusan:** co-MCU = ESP32-C6 (Hafidz, 2026-10-18), menggantikan ESP32-C3 pada
desain awal (`docs/rencana-app-qaonicos/RENCANA.md` §4, `docs/riset-fido2-esp32c3/`).
Protokol UART v1 (text line-based) tetap sebagai control plane.

## 1. Fakta ESP32-C6 (ringkas)

| Aspek | Nilai | Sumber |
|---|---|---|
| CPU | single-core RISC-V 32-bit @160 MHz + LP core @20 MHz | [datasheet](https://robu-prod-media.s3.ap-south-1.amazonaws.com/uploads/2024/10/esp32-c6_datasheet_en.pdf) |
| Memori | 512 KB HP SRAM + 16 KB LP SRAM; flash eksternal (modul umum 8 MB); **tanpa PSRAM** | datasheet |
| Radio | Wi-Fi 6 (802.11ax, 2,4 GHz) + **BLE 5** + 802.15.4 (Zigbee/Thread/Matter), satu antena shared | datasheet |
| Peripheral relevan | 3× UART, SPI, I2C, **I2S**, USB Serial/JTAG, ADC | [Adafruit](https://www.adafruit.com/product/5671?gad_source=1) |
| Daya | deep sleep ~7 µA; Wi-Fi TX peak ~354 mA | [datasheet](https://wiki.lilygo.cc/datasheet/esp32-c6_datasheet_en.pdf), [qoitech](https://www.qoitech.com/blog/esp32-s3-c3-c6-sleep-power-consumption/) |
| Harga modul | ESP32-C6-WROOM-1-N8 ≈ **US$2,8–4,2**; devkit ≈ US$6–7 | [accio](https://www.accio.com/plp/esp32-store) |
| ESP-IDF | support sejak v5.1 (Des 2023); stabil terkini **v6.1** (Okt 2026) — mature | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32c6/api-guides/usb-serial-jtag-console.html) |
| Arduino | support sejak arduino-esp32 core v3.0.0, status Stable | [docs.espressif.com](https://docs.espressif.com/projects/arduino-esp32/en/latest/getting_started.html) |

**Catatan vs C3:** radio stack lebih kompleks (3 radio koeksis); errata chip-rev
awal — pakai modul rev baru + IDF ≥ v5.3.

## 2. Muse Gadget SDK di C6 — satu chip, dua peran

SDK (`github.com/facebookincubator/muse-gadget-sdk`) punya overlay
`esp32/devices/sdkconfig.c6-nopsram` (`CONFIG_IDF_TARGET="esp32c6"`) +
referensi board `sdkconfig.muse-waveshare-c6-18` — **C6 didukung resmi**.
([sdkconfig.c6-nopsram](https://raw.githubusercontent.com/facebookincubator/muse-gadget-sdk/main/esp32/devices/sdkconfig.c6-nopsram))

- **Batasan:** overlay C6 menonaktifkan `CONFIG_HOMEHUB_TUNNEL` — tanpa PSRAM,
  SRAM internal habis saat BLE + Tunnel + TLS jalan bareng. Default target SDK
  kini ESP32-C5 ("Muse Home Link").
- **Untuk Qabot:** pairing BLE + session control tetap jalan di C6; tunnel penuh
  butuh C5 (ada PSRAM) atau trim fitur. BLE pakai **NimBLE** (hemat memori).
- **Implikasi arsitektur:** C6 yang sama bisa pegang **dua peran** — backhaul
  WiFi (firmware custom kita) + BLE gadget (SDK Muse). Perlu diputuskan:
  satu firmware gabungan vs dua firmware/gantian peran. Riset lanjutan.

## 3. Pola companion network MCU

| Pola | Cocok? | Catatan |
|---|---|---|
| **ESP-AT** (firmware jadi Espressif, `AT+CWJAP`, `AT+HTTPCLIENT`…) | Prototipe cepat saja | C6 didukung (v4.2.0.0). Command serial, parsing rapuh untuk biner; `AT+CIPMODE=1` + `+++` escape terkenal rapuh untuk stream kontinu |
| **ESP-Hosted** (SPI/SDIO/UART, protokol paket) | Tidak | Butuh driver host; untuk QaonicOS harus tulis sendiri |
| **Hibrida: control plane teks + data plane biner** | **Direkomendasikan** | Pola umum produk voice-assistant |

**Keputusan desain:** pertahankan protokol UART v1 text line-based sebagai
**control plane** (kompatibel `user/uartproto/` + mock yang sudah ada).
Tambahkan **data plane biner** terpisah untuk audio up-streaming (dan file
down bila perlu): framing `magic(2B) | type(1B) | len(2B) | payload | CRC-16`,
full-duplex, tanpa escape-hell.

## 4. Temuan penting: baud rate v1 tidak cukup untuk voice

- Audio 16 kHz × 16-bit mono = **32 KB/s** (≈256 kbps + overhead framing).
- UART 115200 (baud v1) ≈ **11,5 KB/s** — TIDAK CUKUP.
- **Rekomendasi:** naikkan ke **921600 baud** (≈92 KB/s) untuk link data.
  ESP32-C6 UART sampai 5 Mbps; **baud maksimum UART RV1103 harus diverifikasi**
  (lihat §6, open question #1).

## 5. Ekstensi protokol v2 (draft)

Control plane (teks, kompatibel v1 — semua perintah v1 tetap):

```
VOICE.START <rate> <bits>      → OK            (C6 siap terima stream audio)
VOICE.STOP                     → OK <frames>   (tutup stream, lapor jumlah frame)
TOOL.DISPATCH <len>            → OK <rlen>     (JSON tool-call dari cloud → RV1103;
                                                argumen inline setelah newline, respons dikembalikan)
NET.WS <url> <len>             → OK            (WebSocket/SSE untuk Opsi B; frame via data plane)
```

Data plane (biner, hanya aktif setelah VOICE.START / NET.WS):

```
[AUD  len  payload(PCM16 LE)  CRC]   RV1103 → C6   (audio uplink)
[BIN  len  payload            CRC]   dua arah      (file/blob generik)
[EVT  len  payload(JSON)       CRC]   C6 → RV1103   (event async: WIFI.DROPPED, dsb)
```

Prinsip yang dipertahankan dari v1: secret (API key, WiFi password) boleh
disimpan di NVS C6 (`KV.SET`); **private key FIDO tidak pernah lewat UART**.

## 6. Rencana firmware ESP32-C6 (ESP-IDF)

Komponen: WiFi station (+ provisioning), mbedTLS/HTTP client, WebSocket client,
NimBLE (GATT server untuk peran gadget / coexistence), UART driver (921600,
DMA), NVS terenkripsi, parser control plane + framer data plane, watchdog.

Tahapan (detail implementasi = plan terpisah setelah v2 freeze):
1. **FW-1:** control plane v1+v2 teks di atas UART + WiFi + `NET.HTTP` via
   mbedTLS (pengganti peran mock co-MCU di `user/uartproto/`).
2. **FW-2:** data plane biner + `VOICE.START/STOP` + uplink audio ke endpoint
   STT (pola: chunked POST atau WebSocket).
3. **FW-3:** evaluasi gabung Muse Gadget SDK (BLE pairing) satu firmware.

## 7. Open questions — TERJAWAB 2026-10-18

1. **Baud maksimum UART RV1103? → TERJAWAB: ya, 921600 didukung.**
   Datasheet §1.x: 4 UART (UART2–UART5), *"up to 4Mbps baud rate"*,
   *"auto flow control mode for all UART"*. 921600 aman; **pakai RTS/CTS**
   untuk streaming audio (tersedia di UART4_M1 / UART5_M0_M1).
2. **Satu firmware vs dua peran? → TERJAWAB: satu firmware, tunnel mati.**
   Bukti: overlay C6 SDK Muse sendiri menonaktifkan `HOMEHUB_TUNNEL`
   (BLE+Tunnel+TLS > SRAM). Kebutuhan kita (WiFi + TLS/HTTP client + UART +
   NimBLE gadget, tanpa tunnel) muat di 512 KB — tapi **ukur footprint saat
   build FW-3**, itu gate-nya.
3. **Pin UART di Pico Mini? → TERJAWAB: tersedia di header.**
   **KOREKSI 2026-10-19** (wiki resmi Luckfox): Pico Mini A/B hanya
   expose **3 UART: UART2, UART3, UART4** (UART2 = debug console).
   UART5 hanya ada di **Pico Plus**, tidak di-routing di Mini.
   Klaim "4× UART" sebelumnya = capability chip RV1103, bukan board.
   Rekomendasi revisi: **UART2 = debug console** (tetap),
   **UART4 = link ke ESP32-C6** (dengan RTS/CTS; UART3 cadangan).
   Bonus temuan: ada **5 castellated holes
   untuk Ethernet** (pin FEPHY) — opsi kabel tetap ada secara fisik bila
   suatu hari dibutuhkan, tapi keputusan C6 tidak berubah.
4. **Mic di RV1103 atau C6? → TERJAWAB: tetap RV1103.**
   Codec built-in + pin MICBIAS sudah di skematik; 16 kHz native; pindah ke
   I2S C6 butuh chip mic/codec eksternal (tambah BOM) tanpa keuntungan
   arsitektur. F3 tetap.

## 8. Langkah berikutnya (checklist F5)

- [ ] Verifikasi baud 921600 di UART RV1103 (Q1) + pin di skematik (Q3)
- [ ] Freeze protokol v2 (dokumen ini → `RENCANA.md` §4 sebagai v2)
- [ ] Extend mock co-MCU di `user/uartproto/` untuk perintah v2 (VOICE.*, TOOL.DISPATCH)
- [ ] Skeleton firmware FW-1 (ESP-IDF v6.x, control plane + NET.HTTP)
- [ ] Evaluasi footprint: WiFi + NimBLE + TLS dalam 512 KB SRAM (Q2)
