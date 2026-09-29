# Rencana Implementasi Aplikasi QaonicOS (Device UI App)

Perencanaan implementasi "app" = aplikasi UI yang jalan di atas QaonicOS
(userspace, Luckfox Pico Mini), dari mockup menjadi kode C. Dokumen ini
pasangan dari `rencana-face-qaonicos/RENCANA.md` (spesifik face) —
di sini fokus ke **app shell, navigasi, settings, dan integrasi co-MCU**.

Status saat ditulis (2026-09-28): kernel Mach 3 v1.0 (Fase A selesai),
Fase B selesai (syscall 20/21/22/24), **Fase C sedang berjalan** (agen).

## 1. Ruang lingkup

**Termasuk**: UI app userspace (`user/uiapp`): boot screen, face, main menu
icon grid, system monitor, settings (WiFi/BLE/LLM/Passkey), status bar,
model navigasi, protokol UART ke co-MCU (sisi QaonicOS).

**Tidak termasuk** (track terpisah): kernel/driver (Fase B/C/D), firmware
ESP32-C3 (network + FIDO2, lihat riset `riset-fido2-esp32c3/`), companion
app HP (ditunda — provisioning via BLE standar dulu).

## 2. Keputusan arsitektur

### 2.1 Multi-process ala Mach (keputusan 2026-09-28, user)

**Diputuskan: multi-process.** Setiap peran = program userspace sendiri,
berkomunikasi via Mach IPC (10–12, Fase D; v1 sentinel file).

```
┌──────────────┐  face_set_state/mood   ┌──────────────────┐
│ harness      │ ────────────────────▶ │ user/face        │
│ (nanti)      │                        │ = Qabot server   │
└──────────────┘                        │ - pemilik display│
                                        │   + framebuffer  │
┌──────────────┐  DISPLAY.GRANT/        │ - input routing  │
│ user/uiapp   │  ACQUIRE/RELEASE/FLUSH │ - power state    │
│ menu/settings│ ◀────────────────────▶ │   machine (§11)  │
│ /monitor     │  EV_* (tombol)         └────────┬─────────┘
└──────────────┘         (diforward face)        │ flush
                                         ┌──────┴───────┐
                                         ▼              ▼
                                    flush_spi.c    flush_qemu.c
```

- **user/face** (Qabot server): pemilik display + framebuffer + backend
  flush; routing input tombol; power state machine (§11). IPC server.
- **user/uiapp**: menu grid, settings, monitor, power menu. Tidak
  menyentuh hardware display langsung.
- **Framebuffer shared**: region memori di-map ke face & uiapp
  (via Mach VM share; detail implementasi Fase D).
- **Protokol display (token)**:
  - `DISPLAY.GRANT` — face → uiapp: tombol MENU ditekan di Qabot,
    uiapp boleh tampil.
  - `DISPLAY.ACQUIRE` / `DISPLAY.RELEASE` — uiapp ambil/kembalikan
    token; saat uiapp pegang token, face hentikan render loop.
  - `DISPLAY.FLUSH(x,y,w,h)` — pemegang token minta face flush area
    (hanya face yang bicara ke hardware).
  - Input: face teruskan `EV_*` ke pemegang token via IPC.
- **Boot**: init jalankan face (default) + uiapp (idle menunggu).
- **Auto-return**: uiapp idle 30 dtk → `DISPLAY.RELEASE` → face
  kembali tampilkan Qabot.
- **Crash isolation (jujur)**: logic crash di satu proses tidak membunuh
  yang lain; init restart yang crash, yang selamat pertahankan state.
  Catatan: face pemilik display — face crash = layar mati sampai face
  restart. Isolasi tidak mutlak, tapi jauh lebih baik dari single-process.

### 2.2 Interface modul layar (internal `user/uiapp`)

Semua layar uiapp (menu, settings, monitor, power) implement interface
ini. Face (`user/face`) punya render loop sendiri.

```c
/* user/uiapp/screen.h */
typedef enum { EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_OK, EV_BACK, EV_TICK } event_t;

typedef struct screen {
    void (*enter)(void);                 /* dipanggil saat push */
    void (*exit)(void);                  /* dipanggil saat pop */
    void (*on_event)(event_t ev);        /* input + tick */
    void (*render)(void);                /* gambar ke framebuffer */
} screen_t;

void ui_push(const screen_t *s);         /* masuk layar */
void ui_pop(void);                       /* kembali */
```

Semua layar (termasuk face dan tiap settings) implement interface ini.
Screen manager = stack sederhana.

### 2.3 Model navigasi (keputusan 2026-09-28, dari mockup)

- **Select dulu, baru masuk**: input pertama highlight item (state selected
  jelas), input kedua / OK baru push layar. Berlaku untuk icon grid maupun
  list settings.
- Transisi **tenang**: slide pendek / fade cepat, tanpa efek berlebihan.
- Tombol fisik (target HW): UP/DOWN/LEFT/RIGHT/OK/BACK via GPIO (Fase D).
  Di QEMU: mapping keyboard (mis. WASD + Enter + Esc).
- Keputusan 2026-09-28: **non-touch**, tombol fisik saja.

### 2.4 Status bar permanen

- Jam kiri, baterai/charge kanan, digambar shell di setiap frame.
- **Sumber jam**: `TIME.GET` ke ESP32-C3 via UART (di-cache, refresh 60 dtk).
  RTC driver RV1103 = Fase D (opsional, nanti).
- **Sumber baterai**: driver ADC / fuel gauge I2C = Fase D. v1: ikon
  statis / status charging dari co-MCU bila tersedia — jujur di UI
  (jangan tampilkan angka palsu).

## 3. Layar-layar (dari mockup)

| Layar | Modul | Data / aksi |
|---|---|---|
| Boot splash | `scr_boot.c` | Logo + progress; jalan saat init |
| Qabot (face) | `scr_face.c` | Port dari RENCANA-face; **default/home screen**, nama "Qabot" (§11) |
| Main menu | `scr_menu.c` | Icon grid 2 kolom: Qabot, Monitor, Settings |
| System monitor | `scr_mon.c` | CPU/mem/storage via `SYS_STAT` (57, tersedia sejak Fase C) — data real |
| Power | `scr_power.c` | Settings → Power: Shutdown / Restart / Sleep Now (§11) |
| Settings root | `scr_settings.c` | List: WiFi, Bluetooth, LLM, Passkey |
| WiFi | `scr_wifi.c` | `WIFI.SCAN` → list SSID; connect → input password → `WIFI.CONNECT`; `WIFI.LIST` saved |
| Bluetooth | `scr_ble.c` | `BLE.ON/OFF/NAME/STATUS` |
| LLM | `scr_llm.c` | Pilih provider, input API key (masked), pilih model → `KV.SET` |
| Passkey | `scr_passkey.c` | `FIDO.LIST` kredensial; tambah/hapus; terima `FIDO.EVENT` → layar konfirmasi approve/deny |

Input teks (password, API key): keyboard layar 240×240 tidak ergonomis —
**alur utama = provisioning dari HP via BLE**, layar device hanya konfirmasi
(keputusan 2026-09-28). Keyboard on-screen tetap disediakan sebagai fallback.

## 4. Protokol UART QaonicOS ↔ ESP32-C3 (v1)

Text line-based, `CMD arg…\n` → `OK data…` / `ERR kode pesan`. Baud 115200.

```
WIFI.SCAN                 → OK 3\nSSID1,-67,WPA2\n...
WIFI.CONNECT <ssid>       → OK NEEDPASS | OK CONNECTED | ERR ...
WIFI.PASS <password>      → OK CONNECTED | ERR AUTH
WIFI.LIST                 → OK <saved…>
WIFI.FORGET <ssid>        → OK
BLE.ON / BLE.OFF          → OK
BLE.NAME <nama>           → OK
BLE.STATUS                → OK ON|OFF <paired_addr|NONE>
NET.HTTP <m> <url> <len>  → OK <status> <rlen>  (body via chunked binary)
TIME.GET                  → OK 2026-09-28T20:00:00+07:00
KV.SET <k> <len>          → OK   (secrets → NVS terenkripsi ESP32)
KV.GET <k>                → OK <len>
KV.DEL <k>                → OK
FIDO.LIST                 → OK <rpId,user…>
FIDO.DEL <id>             → OK
FIDO.EVENT                → (async ESP32→Pico: REGISTER|AUTH <rpId> <user>)
FIDO.APPROVE / FIDO.DENY  → OK
```

- Body HTTP besar di-chunk (UART bukan bottleneck untuk teks, tapi jangan
  kirim sekaligus 100KB).
- Event async (FIDO.EVENT, WIFI.DROPPED): ESP32 kirim kapan saja; uiapp
  baca via UART IRQ → ring buffer → event queue → layar yang relevan.
- **Private key FIDO tidak pernah lewat UART** — hanya metadata +
  approve/deny (dari riset FIDO2).

## 5. Persistensi

| Data | Simpan di | Alasan |
|---|---|---|
| Password WiFi, API key, kredensial FIDO | NVS terenkripsi ESP32-C3 (`KV.*`) | Secrets tidak keluar dari secure element-ish storage |
| Preferensi UI (mood default, dsb.) | ramfs (v1, volatile) → SD FAT32 (Fase D) | Non-sensitif |

## 6. Kebutuhan dari OS (mapping fase)

| Kebutuhan | Fase | Status |
|---|---|---|
| `SYS_YIELD` pacing frame | B | ✅ |
| Heap ≥64KB (`SYS_SBRK`) | B | ✅ (app akan butuh lebih — lihat §8) |
| Multi program user, `ulib` | C | berjalan |
| Mach IPC 10–12 (harness→uiapp) | D | dicadangkan |
| Driver UART data (bukan cuma console) | D | console ada; mode data perlu |
| GPIO tombol fisik (40–41) | D | dicadangkan |
| SPI0 + `SYS_SPI_XFER` | D | baru |
| virtio-gpu 2D (QEMU) | D | baru |
| `SYS_STAT` (57) untuk monitor | D | dicadangkan |
| ADC / fuel gauge (baterai) | D | baru |
| RTC (jam alternatif) | D | opsional |

## 7. Strategi testing

1. **Host SDL**: `uiapp` + modul layar dikompilasi untuk x86_64 + SDL —
   verifikasi navigasi, grid, settings flow tanpa OS. (Semua logika UI
   murni C portable; hanya `flush_*` dan `uart_*` yang di-abstract.)
2. **QEMU**: uiapp sebagai user program QaonicOS; display via virtio-gpu
   (jendela QEMU); input via keyboard mapping; co-MCU di-simulasikan
   (skrip host jawab protokol UART via socket/pipe).
3. **Hardware**: ST7789 via SPI; tombol via GPIO; ESP32-C3 fisik via UART.

## 8. Risiko & catatan jujur

- **Arbitrasi display**: protokol token harus deadlock-free — face harus
  selalu bisa merebut kembali display saat kritis (mis. layar konfirmasi
  FIDO), uiapp tidak boleh hold token selamanya.
- **Shared framebuffer**: butuh Mach VM share antar task (detail Fase D);
  fallback v1: kedua proses map region fisik yang sama (carve-out).
- **Dua program = dua bring-up**: event loop non-blocking wajib di
  keduanya; debug IPC antar proses lebih ribet dari single-process —
  ini harga yang disetujui untuk isolasi.
- **Heap 64KB/task (Fase B)**: uiapp (semua layar + face buffer 11.5KB +
  stack) kemungkinan butuh lebih — siapkan kenaikan heap di Fase C/D
  atau alokasi framebuffer statis di luar heap. Jangan asumsi muat.
- **Scheduler kooperatif**: pacing via `SYS_YIELD`/tick 100Hz cukup untuk
  30fps, tapi UART async + UI dalam satu thread = desain event loop
  non-blocking wajib dari awal.
- **Dua firmware** (QaonicOS + ESP32-C3) = dua toolchain, dua debug path.
  Protokol UART v1 harus di-freeze sebelum kedua sisi jalan paralel.
- **Interim co-MCU**: firmware custom ESP-IDF (network+FIDO2) butuh waktu
  (P0–P3 riset FIDO2). Untuk A2 bisa interim pakai **AT firmware** hanya
  untuk WiFi/HTTP sambil firmware custom dibangun — lalu migrasi.
- Display contention: hanya uiapp yang pegang framebuffer. Harness tidak
  gambar langsung — lewat IPC.

## 9. Fase implementasi app

| Fase | Kerja | Estimasi* | Ketergantungan |
|---|---|---|---|
| A0 | Host SDL: shell + screen manager + grid + status bar + navigasi; port modul face | 1–2 minggu | — |
| A1 | `user/face` (Qabot server) + `user/uiapp` sebagai dua user program; backend virtio-gpu; input keyboard; protokol display token | 2–3 minggu | Fase C, virtio-gpu, IPC |
| A2 | Protokol UART v1 + layar WiFi/BLE/LLM fungsional vs co-MCU (interim AT firmware) | 2–3 minggu | Driver UART data, ESP32-C3 hw |
| A3 | Persistensi pref, jam via `TIME.GET`, baterai (ADC bila ada) | 1 minggu | Fase D parsial |
| A4 | Hardware: SPI ST7789, tombol GPIO, uji vs ESP32-C3 fisik | 2–3 minggu | Fase D |
| — | **Track paralel: firmware ESP32-C3** custom (network + FIDO2 P0–P3) | 3–6 bulan | lihat riset FIDO2 |

\* Satu engineer yang familiar dengan codebase.

## 10. Keputusan

1. ✅ 2026-09-28: **multi-process diputuskan** (user). Face = IPC server
   + pemilik display; uiapp = client display via protokol token (§2.1).
2. ✅ 2026-09-28: **non-touch**, tombol fisik saja.
3. ✅ 2026-09-28: web + Android apps **nanti**; desain API + keamanan
   disiapkan sekarang (§12).
4. ✅ Selesai dengan sendirinya: `SYS_STAT` sudah tersedia sejak Fase C
   dan Fase D selesai — system monitor langsung pakai **data real**,
   tanpa placeholder.

## 11. Qabot & power management (keputusan 2026-09-28)

- Layar face bernama **Qabot**; Qabot = default/home screen device.
- **Auto-return**: idle di menu/settings selama `UI_IDLE_RETURN_S`
  (default 30 dtk) → otomatis kembali ke Qabot.
- **Display off**: idle di Qabot selama `UI_IDLE_DISPLAY_OFF_S`
  (default 120 dtk) → backlight mati, rendering berhenti (framebuffer
  dipertahankan).
- **Sleep**: display off selama `UI_SLEEP_AFTER_S` (default 300 dtk) →
  mode sleep hemat daya. v1 boleh = display off + CPU idle (wfi);
  sleep beneran (clock gating RV1103) diverifikasi saat hardware.
- **Wake**: input tombol apa pun → bangun ke Qabot, display on.
  Event penting (FIDO.EVENT, dsb.) juga membangunkan.
- Timeout dihitung via tick `SYS_YIELD`; semua konstanta configurable,
  bukan magic number.
- **Power menu**: Settings → Power → **Shutdown** / **Restart** /
  **Sleep Now**. (Sudah masuk mockup.)
- **Mockup = patokan** (keputusan 2026-09-29): mockup interaktif
  https://muse.ai/s/mockup-ui-qaonicos-cd6waoggxhx0g adalah referensi
  desain; implementasi yang mengejar mockup, bukan sebaliknya.
  Gap A2 vs mockup (ditutup bertahap): **Restart** belum ada di uiapp
  (butuh driver reset/PSCI — tambah saat driver tersedia); **jam**
  status bar masih uptime (butuh RTC di HW); **wake** hanya tombol
  terpetakan di QEMU (di HW semua tombol fisik terpetakan).

## 12. Desain API companion — web & Android (keputusan 2026-09-28)

Implementasi **nanti**; desain + keamanan **disiapkan sekarang**.
Companion apps bicara ke device lewat **ESP32-C3** (BLE; nanti: HTTP via
LAN). Pico Mini tidak expose API langsung ke radio.

### Transport

- **BLE GATT** (utama): custom service `QAON-COMPANION`:
  - `wifi_prov` (Write): SSID + password
  - `llm_cfg` (Write/Read): provider, model; API key **write-only**
    (read hanya kembalikan `key_present: true/false`)
  - `dev_status` (Read/Notify): state, baterai, versi firmware
- FIDO2 tetap pakai **CTAP2 over BLE standar** (service `0xFFFD`),
  bukan characteristic custom.
- **HTTP LAN API** (nanti): device join WiFi / mode AP → REST
  (`GET /api/status`, `POST /api/notify`, …) untuk web dashboard.

### Keamanan

- **BLE**: LE Secure Connections + **bonding**; characteristic sensitif
  hanya untuk bonded device. Pairing pakai numeric comparison —
  manfaatkan layar device sebagai trust anchor.
- **Konfirmasi fisik**: operasi sensitif (hapus kredensial FIDO, factory
  reset, ganti API key) wajib dikonfirmasi via **tombol OK di device** —
  tidak bisa di-approve dari HP saja.
- **Secrets**: write-only, tidak bisa dibaca balik.
- **HTTP LAN (nanti)**: token sesi hasil BLE pairing (out-of-band),
  expiry pendek; anti-replay via nonce.
- **Threat model v1**: penyadap radio → LESC + bonding; aplikasi HP
  jahat → tanpa konfirmasi fisik di device, operasi sensitif ditolak;
  relay attack FIDO → mengandalkan mekanisme CTAP2 (UP bit + presence
  fisik).
- Eksplisit **tidak** dijanjikan v1: remote attestation device,
  E2E encryption app↔cloud via device.

## Status implementasi (catatan berjalan)

| Fase | Status | Catatan |
|---|---|---|
| A1 (face QEMU) | DONE 2026-09-29 | `user/face` Qabot di QEMU via virtio-gpu (branch `app/a1-face-qemu`) |
| A2 (face server + uiapp) | DONE 2026-09-29 | face = display server persisten, uiapp = menu/monitor/settings/power via protokol token, syscall 62–68 (branch `app/a2-faceserver-uiapp`) |
| A3 (UART v1 + WiFi/BLE/LLM) | DONE 2026-09-29 | `user/uartproto/` (framing + mock co-MCU in-process) + layar WiFi (scan→password→TERHUBUNG), BLE (toggle), LLM (provider/model/API key via KV), Passkey (FIDO.LIST). Terverifikasi end-to-end di QEMU. co-MCU fisik (ESP32-C3) = syarat HW untuk data real (branch `app/a3-uartproto-screens`) |
| A4 (jam NTP + persistensi NVS) | DONE 2026-09-29 | UDP di netstack + syscall 69–72 (TIME_SET/GET, UDP_SEND/RECV); daemon `ntp` (DNS 10.0.2.3:53 → SNTP pool.ntp.org:123, retry 3×3 dtk, resync 3600 dtk, tolak timestamp < 2023); status bar = jam WIB bila sinkron (fallback uptime); mock TIME.GET → ISO8601 jam real. NVS: mkfat32 reserve cluster 3 = BAD → LBA 552 (2 sektor), preserve byte-identik bila magic `QNVS` cocok; `user/cfg` (nvs_state 868B, CRC-16/CCITT); mock_comcu write-through cfg_save. Verifikasi: unit test host DNS/SNTP OK; RX kernel OK (injeksi loopback); TX OK; persistensi OK 2 boot; 8/8 program + 17/17 file PASS; HTTP 200. Batas: sandbox blokir semua UDP sendto → NTP asli tak end-to-end di sini (retry graceful 60 dtk). (branch `app/a4-ntp-persist`, commit `44a2c5f`) |
