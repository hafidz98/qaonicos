# Rencana Implementasi Face UI di QaonicOS

Dokumen perencanaan: bagaimana wajah agent (dari prototipe web "Wajah Agent")
diimplementasikan sebagai bagian dari QaonicOS di Luckfox Pico Mini (RV1103).
Status repo saat dokumen ditulis: kernel = Mach 3.0 asli (CMU) port ARMv7 rilis
v1.0 (Fase A selesai); Fase B sedang berjalan (syscall dasar 20/21/22/24,
satu task user, scheduler kooperatif). Kernel from-scratch Fase 1–17 diarsipkan
di `archive/kernel-scratch/` sebagai referensi driver.

## 1. Keputusan desain (final)

- **Renderer prosedural dalam code C**, bukan PNG/SVG asset. Alasan: RAM
  (satu frame 240×240 RGB565 = 115KB + buffer decode vs kode hitungan KB),
  animasi = interpolasi parameter (bukan sprite), dan preseden RoboEyes
  (gambar primitif, nol bitmap).
- **Prototipe web = living spec.** Model parameternya di-port 1:1 ke C.
- **Face = program userspace** (`user/face`), bukan di kernel. Filosofi Mach:
  kernel kecil, servis di userspace. Face jalan sebagai **IPC server**;
  harness agent (nanti) jadi client.
- Lisensi: konsep RoboEyes direimplementasi, **jangan copy code** (GPL-3.0).

## 2. Arsitektur

```
┌──────────────┐   Mach IPC (port/message)   ┌──────────────┐
│ agent harness│  face_set_state/mood/...    │  user/face   │
│ (userspace)  │ ──────────────────────────▶ │ (IPC server) │
└──────────────┘                             └──────┬───────┘
                                                    │ gambar primitif
                                                    ▼
                                             ┌──────────────┐
                                             │ framebuffer  │  RGB565,
                                             │ (parsial)    │  1/10 layar
                                             └──────┬───────┘
                                                    │ flush(area)
                              ┌─────────────────────┼─────────────────────┐
                              ▼                     ▼                     ▼
                        SYS_SPI_XFER           virtio-gpu flush        (nanti)
                        → driver SPI0          → jendela QEMU
                        → ST7789 (HW)          (lihat §3)
```

- **Framebuffer abstraction**: face selalu gambar ke framebuffer RAM;
  backend flush diganti per target (SPI di hardware, virtio-gpu di QEMU).
  Persis pola `flush_cb` LVGL.
- **Partial framebuffer**: heap per task Fase B = 64KB, tidak muat frame
  penuh 115KB. Pakai buffer 1/10 layar (±11.5KB) + dirty-region flush.
- **Harness → face**: Mach IPC syscall 10–12 (Fase D). Untuk v1 boleh pakai
  pola sentinel file (seperti koordinasi Fase 10) supaya tidak blokir IPC.
- **Boot**: `init` menjalankan face saat boot → boot screen → idle face.
- **2026-09-28**: multi-process diputuskan — face tetap IPC server
  sekaligus **pemilik display**; `user/uiapp` (menu/settings/monitor)
  menjadi client display via protokol token. Detail: rencana-app §2.1.
  Harness tetap client `face_set_state` seperti diagram di atas.

## 3. Display di QEMU (koreksi)

QEMU **bisa** menampilkan display OS — klaim sebelumnya ("cuma bisa dump
framebuffer ke file") terlalu pesimis. Opsi untuk `-M virt`:

| Opsi | Cara | Effort | Catatan |
|---|---|---|---|
| **virtio-gpu** (`-device virtio-gpu-pci`) | Driver guest 2D: create resource, attach backing, flush rect | Sedang | Tampil di jendela SDL/GTK QEMU. Pola driver virtio (blk, net) sudah ada di `archive/` — bisa dicontoh |
| ramfb | Framebuffer via fw_cfg | Kecil–sedang | Butuh driver fw_cfg; lebih simpel protokolnya |
| Dump framebuffer ke file | Tanpa driver baru | Nol | Fallback paling murah |

**Rekomendasi: virtio-gpu 2D.** Mode 2D-nya pada dasarnya "kasih framebuffer,
flush rectangle" — jauh lebih sederhana dari GPU 3D, dan beberapa hobby OS
sudah melakukannya. Backend flush face tinggal ganti: QEMU → virtio-gpu,
hardware → SPI/ST7789, tanpa ubah kode gambar.

## 4. Yang dibutuhkan dari OS (mapping fase)

| Kebutuhan | Fase | Status |
|---|---|---|
| Heap 64KB/task (`SYS_SBRK`) | B | ✅ ada |
| `SYS_YIELD` untuk pacing frame (~30fps) | B | ✅ ada |
| Multi program user + `ulib` | C | berjalan |
| Syscall file/ramfs (sentinel, opsi v1) | C | 30–34 dicadangkan |
| Mach IPC `SYS_SEND/RECV/RPC` (10–12, 23) | D | dicadangkan |
| Driver GPIO (40–41) — bit-bang bring-up | D | dicadangkan |
| **Driver SPI0 + `SYS_SPI_XFER`** | D | baru — kerja utama |
| Driver virtio-gpu 2D (QEMU) | D | baru — kerja utama |
| `SYS_STAT` (data system monitor) | D | 57 dicadangkan |

Urutan: selesaikan B → C → D (SPI + virtio-gpu) → face. Face tidak bisa
mulai sebelum driver display ada, kecuali logika gambar diuji di host
terlebih dahulu (lihat §7).

## 5. Model parameter (dari prototipe web)

```c
/* user/face/face.h */
typedef enum { MOOD_DEFAULT, MOOD_TIRED, MOOD_ANGRY, MOOD_HAPPY } face_mood_t;
typedef enum { FACE_EYES, FACE_WAVEFORM, FACE_SPHERE } face_mode_t;

typedef struct {
    int16_t x, y;      /* posisi tengah */
    int16_t w, h;      /* lebar/tinggi */
    int16_t r;         /* corner radius (squircle) */
} eye_t;

typedef struct {
    face_mode_t mode; face_mood_t mood;
    eye_t eye_l, eye_r; int16_t eye_space;
    /* waveform */ uint8_t wave_amp;
    /* sphere */   uint8_t morph_phase, pulse_phase;
    /* teks */     char status_text[24];  /* "LISTENING...", "THINKING", ... */
    /* hidup */    uint32_t blink_timer, idle_gaze_timer;
    int16_t gaze_x, gaze_y;
} face_state_t;

/* API untuk harness (via IPC atau langsung di v1) */
void face_set_mode(face_mode_t m);
void face_set_mood(face_mood_t m);
void face_set_status(const char *text);   /* teks indikator pulsating */
void face_update(uint32_t dt_ms);        /* interpolasi + timer, per frame */
void face_render(void);                  /* gambar ke framebuffer */
```

- **Transisi smooth** = interpolasi semua parameter tiap frame dengan easing
  (tidak ada ganti state mendadak).
- **Autoblinker/idle gaze** = timer acak (pola RoboEyes).
- **Teks indikator**: font bitmap 5×7 monospace (`font5x7.h`), digambar
  langsung ke framebuffer, efek pulsating via alpha/brightness.
- **SFX**: di hardware butuh buzzer/piezo via PWM/GPIO — ditunda (U5).
  Web Audio di prototipe tidak dibawa ke C.

## 6. Struktur file yang disarankan

```
user/face/
├── face.h        /* model parameter + API (lihat §5) */
├── face.c        /* state machine, interpolasi, timer blink/idle */
├── face_draw.c   /* primitif: fill_round_rect, circle, arc, waveform,
                     sphere (blob polar), teks 5x7 → framebuffer RGB565 */
├── font5x7.h     /* font bitmap */
├── flush_qemu.c  /* backend flush via virtio-gpu */
├── flush_spi.c   /* backend flush via SYS_SPI_XFER → ST7789 */
└── main.c        /* IPC server loop + frame pacing via SYS_YIELD */
```

## 7. Strategi testing

1. **Host**: kompilasi `face.c`/`face_draw.c` untuk x86 + SDL — verifikasi
   visual & animasi tanpa QEMU/HW. (Primitif gambar murni C, portable.)
2. **QEMU**: `user/face` jalan di QaonicOS, flush via virtio-gpu →
   tampil di jendela QEMU. Verifikasi IPC harness→face.
3. **Hardware**: ganti backend ke `flush_spi.c`, ST7789 1.3" 240×240
   (wiring terbukti di repo resmi Luckfox). Bawa juga SSD1306 sebagai
   display debug kedua bila perlu.

## 8. Risiko & catatan jujur

- DMA SPI tidak disebut di datasheet RV1103 — perlu verifikasi saat nulis
  driver; polling 40MHz cukup untuk 30fps (full refresh ≈ 23ms).
- Scheduler masih kooperatif (Fase B) — pacing frame via `SYS_YIELD`
  per tick 100Hz; cukup untuk 30fps.
- Heap 64KB/task: buffer parsial 11.5KB + kode + stack muat, tapi jangan
  alokasi frame penuh.
- Init-sequence ST7789 (command list) ditulis di userspace (`flush_spi.c`),
  bukan di kernel — kernel hanya mekanisme transfer.
- USB RNDIS gadget tidak lagi dibutuhkan untuk network — digantikan co-MCU
  ESP32-C3 (lihat §10). Stack TCP Fase 11/12 tetap berguna untuk LAN/debug.

## 10. Co-MCU ESP32-C3 sebagai network companion (keputusan 2026-09-28)

Pico Mini **tidak punya WiFi/BLE onboard** (terverifikasi skematik + docs).
Solusi: ESP32-C3 (±Rp30–50rb, WiFi 4 + BLE 5) sebagai coprocessor network,
terhubung ke Pico Mini via UART 3.3V.

```
QaonicOS (Pico Mini)            ESP32-C3 (co-MCU)               Internet
 face UI + harness (userspace)  WiFi + BLE + TLS + HTTP(S)
        │  UART, protokol teks          │
        │  "POST https://api..." ──────▶ │ ──── HTTPS ────▶ LLM provider
        │   ◀────── respons ──────────── │
```

- Firmware v1: **Espressif AT command** — tanpa perlu nulis firmware sendiri.
- QaonicOS hanya butuh driver UART (console UART sudah ada) + framing
  protokol teks sederhana.
- **Implikasi arsitektur**: TLS (mbedTLS) + HTTP client **pindah ke ESP32**.
  QaonicOS tidak perlu USB RNDIS gadget; TCP stack tetap ada untuk LAN/debug.
- **BLE**: provisioning WiFi via HP + kontrol lokal. Settings sensitif
  (password WiFi, API key) idealnya di-provision dari HP via BLE, layar
  device hanya untuk konfirmasi — input keyboard di 240×240 tidak ergonomis.
- UART bukan bottleneck: API call LLM berupa teks kecil.

## 11. Companion passkey / FIDO2 authenticator (keputusan 2026-09-28)

Device juga berfungsi sebagai **roaming authenticator** (seperti security key
/ "use phone as security key").

- Transport kandidat: **CTAP2 over BLE** (di ESP32-C3 — pas: BLE + akselerasi
  hardware ECC/SHA/AES) atau **CTAP2 over USB HID** (butuh USB device stack
  di QaonicOS — kerja besar, ditunda).
- Nilai tambah vs security key biasa: **layar + face UI untuk konfirmasi**
  registrasi/autentikasi, ala hardware wallet.
- Scope jujur: CTAP2 adalah protokol real (CBOR, ECDSA P-256, attestation
  certs) — proyek medium-besar tersendiri. **Butuh riset dedicated**
  (evaluasi stack FIDO2 open-source untuk di-port ke ESP32-C3) sebelum jadi
  fase implementasi.
- Settings UI: layar Passkey (status authenticator, daftar kredensial,
  tambah/hapus) — sudah masuk mockup.

## 12. Spesifikasi UI dari mockup (keputusan 2026-09-28)

- Main menu = **icon grid** (bukan list).
- **Status bar permanen** di semua layar: jam di kiri, indikator
  baterai/charge di kanan.
- **Model navigasi: select dulu, baru masuk** — tap pertama hanya
  highlight item (state selected jelas), tap kedua / tombol OK baru
  pindah layar. Berlaku untuk grid menu maupun list settings.
- Transisi antar layar **tenang dan simpel** (slide pendek / fade cepat),
  tanpa animasi berlebihan.

## 9. Estimasi kasar (satu engineer familiar codebase)

| Tahap | Kerja | Estimasi |
|---|---|---|
| U0 | face_draw + face.c di host/SDL, visual check | 3–5 hari |
| U1 | Bring-up bit-bang SPI via GPIO (validasi wiring, HW) | 1–2 hari |
| U2 | Driver SPI0 kernel + `SYS_SPI_XFER` | 3–5 hari |
| U3 | Driver virtio-gpu 2D (QEMU) | 1–2 minggu |
| U4 | `user/face` + IPC server + integrasi init/boot | ±1 minggu |
| U5 | Polish: DMA, dirty-region optimal, SFX piezo | menyusul |
