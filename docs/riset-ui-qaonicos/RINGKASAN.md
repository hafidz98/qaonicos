# Riset UI untuk QaonicOS

**Tanggal:** 2026-09-28
**Target:** Luckfox Pico Mini (Rockchip RV1103 — Cortex-A7 1.2 GHz, 64 MB DDR2, 128 MB NAND, tanpa display output native; MIPI hanya CSI input kamera; pin SPI0 tersedia di header board)
**Status QaonicOS saat riset:** syscall GPIO (40/41), SD (50/51), FAT32 (52–56), STAT/TLIST/console (57–59); dashboard web HTTP (Fase 12) jalan; TUI `umon` (Fase 17) jalan; belum ada driver SPI.

---

## 1. Hardware display yang realistis

RV1103 **tidak punya display output native** (tidak ada DSI/HDMI/LCDC; MIPI yang ada hanya CSI input kamera — terkonfirmasi dari datasheet §1.2). Jalur display yang realistis: **SPI TFT** atau **I2C OLED** via pin header.

### 1.1 Kandidat modul

| Modul | Resolusi | Interface | Harga umum | Daya |
|---|---|---|---|---|
| **ST7789** 1.3" (kandidat utama) | 240×240 | SPI | ~US$3,5–5,5 (AliExpress); **Rp44rb** (Tokopedia, RYTECH Bandung) | 3,3V logika; total ~40–50 mA (backlight 30–40 mA) |
| ILI9341 2.4"/2.8" | 320×240 | SPI | ~US$3,9–6,3; 2.8" Rp221–254rb (Tokopedia) | 3,3V/5V (ber-regulator); ~100–165 mA |
| SSD1306 0.96" | 128×64 | I2C | ~US$1–2,2 (AliExpress) | 3,3–5V; ~20 mA (emissive, tanpa backlight) |

Pin yang dibutuhkan: ST7789 7-pin (GND, VCC, SCL/SCK, SDA/MOSI, RES/RST, DC, BL/BLK — varian umum **tanpa CS**, CS di-tie ke GND); ILI9341 tambah CS/RESET/SDO (varian touch tambah XPT2046); SSD1306 4-pin (GND, VCC, SCL, SDA, alamat I2C umum 0x3C).

Sumber harga: [alitools ST7789](https://alitools.io/en/showcase/1-3-inch-ips-hd-tft-st7789-drive-ic-240-240-spi-communication-3-3v-voltage-4-wire-spi-interface-full-color-lcd-oled-display-diy-32880846744), [Tokopedia ST7789](https://www.tokopedia.com/tokorytech/lcd-oled-1-3-inch-st7789-tft-ips-240x240-display-module-full-color-solder-4d0e3), [accio ILI9341](https://www.accio.com/plp/2-4-tft-spi-240x320-arduino), [Tokopedia ILI9341](https://www.tokopedia.com/donbrivgoods/tzt-240x320-2-8-spi-tft-lcd-touch-panel-serial-port-module-with-pbc-ili9341-st7789v-2-8-inch-spi-serial-display-with-touch), [imall SSD1306](https://imall.com/product/0.96-inch-oled-IIC-Serial-White-Display-Module-128X64-I2C-SSD1306-12864-LCD-Screen-Board/Electronic-Components-Supplies-Optoelectronic-Displays/aliexpress.com/1005004864280191/144-220215886/en).

### 1.2 Pin & power di Pico Mini

Header **H2, 2×11 pin** (dari skematik board). Yang penting: **3,3V tersedia (pin 3, VCC_3V3)** dan **GND (pin 2, 21)**. Peta pin relevan:

| Pin | Sinyal | Fungsi display |
|---|---|---|
| 3 | VCC_3V3 | Power modul |
| 2, 21 | GND | Ground |
| 6 | GPIO1_C0 → **SPI0_CS0_M0** | CS display |
| 7 | GPIO1_C1 → **SPI0_CLK_M0** | SCK |
| 8 | GPIO1_C2 → **SPI0_MOSI_M0** | MOSI/SDA |
| 9 | GPIO1_C3 → **SPI0_MISO_M0** | MISO (opsional) |
| 12 | GPIO1_D0 (gpio56) | RST/RES display |
| 13 | GPIO1_D1 (gpio57) | DC display |
| 14 | GPIO1_D2 → I2C3_SDA_M1 / **SPI0_CS1_M0** | SDA (OLED) / CS kedua |
| 15 | GPIO1_D3 → I2C3_SCL_M1 | SCL (OLED) |

### 1.3 SPI0 RV1103 (dari datasheet + panduan Rockchip)

- 1 kontroler SPI0, **2 chip-select**, mode master/slave (datasheet §1.2)
- Clock maksimum master: **50 MHz** ([Rockchip SPI Developer Guide](https://lo01.g77k.com/aeb/docs/en/Common/SPI/Rockchip_Developer_Guide_Linux_SPI_EN.pdf)); **40 MHz sudah terbukti jalan** (repo resmi Luckfox `spi/fb7789.py` menjalankan SPI0 @ 40 MHz via spidev untuk ST7789 240×240, dengan DC=gpio57, RESET=gpio56 — persis pin 13/12 di atas)
- DMA tidak disebut di datasheet, tapi didukung di level driver kernel (jalur spidev memanfaatkannya)
- FSPI (SPI flash internal) adalah kontroler terpisah → **SPI0 bebas dipakai display**
- Full refresh 240×240 RGB565 (115.200 byte) @ 40 MHz ≈ **23 ms** (~43 fps teoretis); animasi wajah tidak butuh full refresh tiap frame

### 1.4 Catatan wiring (ST7789, sudah terbukti di board ini)

CS→pin 6, CLK→pin 7, SDA(MOSI)→pin 8, RST→pin 12, DC→pin 13, BL→3,3V (atau GPIO untuk dimming), VCC→pin 3, GND→pin 2. Referensi: `fb7789.py` di repo Luckfox dan [catatan bring-up independen](https://github.com/bubbletoptag/neodct/blob/HEAD/docs/HARDWARE_NOTES.md) (pola userspace via spidev — 1:1 dengan rencana driver C di QaonicOS).

**Peringatan:** logika RV1103 = **3,3V** (VCCIO 3,3V) — jangan hubungkan 5V ke pin GPIO.

### 1.5 Rekomendasi modul: ST7789 1.3" 240×240

1. Cukup untuk face agent (full-color, resolusi sedang untuk mata squircle + waveform + teks) dengan framebuffer hanya 115 KB; SSD1306 monokrom 128×64 tidak cukup untuk desain wajah yang disetujui — cocoknya hanya untuk teks status/debug sebagai display kedua (cukup 2 kabel I2C ke pin 14/15).
2. Wiring paling mudah dan **sudah terbukti jalan di board ini** (dua referensi di §1.4).
3. Daya aman (~40–50 mA dari rail 3V3_O board); harga wajar (~Rp44rb / ~$4).
4. ILI9341 hanya bila butuh area UI besar (dashboard) — lebih mahal, lebih boros (~150 mA), fisik besar untuk konsep "wajah" kecil.

---

## 2. LVGL

**Versi & lisensi:** stabil terbaru **v9.6.0** (rilis 16 September 2026 — disebut rilis terakhir seri v9, rilis stabilisasi). Lisensi **MIT** — bebas dipakai produk komersial tanpa royalti/copyleft.

**Kebutuhan RAM/Flash riil (dokumentasi resmi):**
- Flash: minimal >64 kB (rekomendasi >180 kB)
- RAM statis: minimal >16 kB (rekomendasi >48 kB); heap dinamis diatur via `LV_MEM_SIZE` di `lv_conf.h`
- Draw buffer: minimal > 1 baris resolusi horizontal (rekomendasi > 1/10 layar)
- Ringkasnya (README): **32 kB RAM + 128 kB Flash + buffer 1/10 layar** sudah cukup untuk jalan
- Untuk QaonicOS (64 MB RAM): angka ini trivial; bahkan full-frame 320×240 RGB565 (~150 kB) bukan masalah.

**Strategi partial framebuffer untuk RAM kecil:**
- Pola kanonik: buffer 1/10 layar, `LV_DISPLAY_RENDER_MODE_PARTIAL`
- LVGL hanya me-render area yang berubah ("dirty area") ke buffer kecil, lalu memanggil `flush_cb(area, px_map)` untuk mengirim area itu ke display; setelah transfer selesai panggil `lv_display_flush_ready()` agar buffer bisa dipakai lagi
- 1 buffer = render dan transfer serial; 2 buffer = paralel (satu di-render sementara satunya dikirim via DMA) — pola **DMA double-buffering**

**Minimum untuk porting display driver (bare-metal/OS custom):**
1. **Tick 1–10 ms** — panggil `lv_tick_inc(x)` dari timer/IRQ, atau daftarkan `lv_tick_set_cb()`. Dibutuhkan untuk animasi & timing internal.
2. **`flush_cb`** — satu fungsi `(area, px_map)`: set address window di kontroler (ST7789 dsb), kirim `px_map` via SPI, akhiri dengan `lv_display_flush_ready()`. Untuk DMA non-blocking: sinyal selesai via IRQ.
3. **`lv_timer_handler()`** dipanggil periodik (~5 ms) dari superloop/thread UI.
4. **Input device (`lv_indev`) opsional** — tanpa input pun LVGL jalan untuk output-only.

Urutan porting resmi: init driver HW → `lv_init()` → tick source → `lv_display_create()` + buffer + `flush_cb` → loop `lv_timer_handler()`.

Sumber: [LVGL CHANGELOG](https://github.com/lvgl/lvgl/blob/HEAD/docs/src/changelog/CHANGELOG.mdx), [LVGL README](https://github.com/lvgl/lvgl/blob/HEAD/README.md), [porting display docs](https://github.com/open-vela/apps_graphics_lvgl/blob/HEAD/docs/porting/display.rst), [porting guide](https://github.com/lvgl/lvgl/blob/HEAD/docs/src/getting_started/porting.mdx).

---

## 3. Alternatif toolkit

| Toolkit | Lisensi | Karakteristik | Cocok? |
|---|---|---|---|
| **Adafruit GFX** (+ Adafruit_ST7789) | BSD | Primitif 2D saja (titik/garis/lingkaran/teks bitmap) — **bukan** widget library. Sangat Arduino-centric (dependensi `Adafruit_BusIO`, `Wire`/`SPI` Arduino) → porting butuh melepas dependensi Arduino | Kurang — bukan framework UI |
| **TFT_eSPI** (Bodmer) | **GPL-3.0** | Cepat untuk SPI (DMA optimized), dukung ST7789. Arduino-centric, bukan widget framework | Tidak — copyleft + bukan widget |
| **µGUI** | Permissive (penulis) | Sangat minimal: ~3 file, ~7,7k baris C, hanya butuh 1 fungsi put-pixel, tanpa alokasi dinamis, widget dasar (button, textbox). Tapi proyek stagnan, tampilan jadul | Darurat saja |
| **µGFX** | Proprietary (gratis non-komersial) | Ringan untuk touchscreen; status tooling tidak jelas | Tidak — lisensi |
| **emWin** (SEGGER) | Proprietary closed-source | Mature (WM, anti-aliased text, touch). Gratis **hanya** untuk keluarga MCU tertentu (NXP/Renesas/GigaDevice) — **RV1103 tidak termasuk** → butuh lisensi komersial | Tidak — lisensi |
| **Embedded Wizard** | Proprietary berbayar | IDE WYSIWYG generate C untuk bare-metal; workflow editor GUI di PC | Tidak — harga & workflow tidak cocok |
| **Renderer custom minimal** | Bebas | Primitif sendiri ke framebuffer (put-pixel/blit/fill-rect) + gambar prosedural (interpolasi parameter). RAM = framebuffer saja (~115–150 kB), flash hitungan kB | **Ya — untuk face agent sekarang** |

**Kesimpulan perbandingan:** untuk kebutuhan widget masa depan, **LVGL paling masuk akal** (MIT, footprint kecil, porting bare-metal terdokumentasi resmi, ekosistem aktif). Untuk face agent **sekarang**, renderer custom prosedural lebih tepat — kebutuhan driver SPI-nya sama, tanpa kompleksitas widget. Jalur praktis: bangun driver SPI + primitif gfx minimal dulu (dipakai face agent); LVGL bisa di-port belakangan di atas jalur flush yang sama, dan `lv_canvas` LVGL memungkinkan face renderer prosedural dipakai ulang di dalam layar LVGL tanpa rewrite.

Sumber: [Adafruit GFX](https://registry.platformio.org/libraries/adafruit/Adafruit%20GFX%20Library), [TFT_eSPI](https://www.libhunt.com/compare-Adafruit-ST7735-Library-vs-TFT_eSPI?ref=compare), [µGUI](https://github.com/rsta2/circle/blob/HEAD/addon/ugui/README.md), [emWin lisensi](https://www.biospace.com/nxp-semiconductors-to-offer-emwin-graphic-library-free-with-arm-microcontrollers), [Embedded Wizard](https://en.wikipedia.org/wiki/Embedded_Wizard).

---

## 4. Pemetaan arsitektur ke QaonicOS

Prinsip yang sudah terbukti di fase-fase sebelumnya: **kernel hanya menyediakan mekanisme generik, kebijakan dan pengetahuan device-spesifik tinggal di userspace**. Contoh: driver SD generik (sektor read/write) di kernel, logika FAT32 di atasnya. Pola yang sama berlaku untuk display.

### Opsi (a): driver SPI di kernel + syscall framebuffer

- Kernel mengimplementasikan driver SPI0 (register-level, seperti driver GPIO/virtio-blk yang sudah ada).
- Syscall baru yang generik, misalnya `SYS_SPI_XFER` (transfer full-duplex raw: tx buf, rx buf, len, chip-select). **Jangan** menaruh init-sequence ST7789 di kernel — itu pengetahuan device-spesifik.
- Program userspace (`uface`/`udisp`) melakukan: init sequence display (via `SYS_SPI_XFER` + `SYS_GPIO_SET` untuk pin DC/RST/BL) → memegang framebuffer di memorinya sendiri → me-render → flush region kotor via `SYS_SPI_XFER`.
- Kelebihan: satu driver dipakai bersama secara aman; DMA/interrupt bisa dikelola kernel nantinya; konsisten dengan arsitektur syscall yang ada.
- Kekurangan: kode kernel bertambah; perlu desain syscall yang hati-hati (bounce buffer seperti pola `SYS_RPC_USER` Fase 8).

### Opsi (b): SPI bit-bang dari userspace via syscall GPIO yang ada

- Memakai `SYS_GPIO_SET`/`SYS_GPIO_GET` (40/41) untuk membangkitkan protokol SPI sepenuhnya dari userspace. Nol perubahan kernel — bisa jalan hari ini.
- Kekurangan fatal untuk animasi: setiap bit butuh 2–3 toggle GPIO, masing-masing = satu syscall (trap + handler). Satu frame penuh ST7789 240×240×16 bit = 115.200 byte = 921.600 bit → ±2,7 juta syscall per frame. Dengan estimasi optimis ~1 µs per syscall, satu frame butuh **orde detik**. Tidak usable untuk face animasi; hanya cukup untuk update teks statis kecil (mis. SSD1306).
- Verdict: **hanya untuk bring-up** (validasi wiring + init sequence), bukan untuk runtime.

### Opsi (c): hybrid (REKOMENDASI)

1. **Tahap bring-up:** bit-bang via GPIO untuk memastikan wiring benar dan init-sequence ST7789 valid — tanpa menyentuh kernel.
2. **Tahap produksi:** driver SPI0 di kernel (polling dulu, DMA belakangan) + `SYS_SPI_XFER`; init display, framebuffer, dan rendering tetap di userspace.

Hitungan kasar performa: satu frame penuh 240×240 RGB565 = 115.200 byte. Pada SPI 40 MHz, transfer teoritis ≈ 23 ms/frame (≈ 40 fps); dengan overhead realistis 15–25 fps untuk full-screen — lebih dari cukup untuk face agent. Update parsial (dirty region, mis. hanya area mata) jauh lebih cepat. Framebuffer 115 KB muat nyaman di memori userspace (VM_NPAGES=96 → 384 KB sejak Fase 16).

---

## 5. Desain modul face renderer C

Keputusan desain yang sudah disetujui: **renderer = procedural C** (gambar primitif langsung ke framebuffer), bukan PNG/SVG. Alasan: hemat RAM (frame PNG 240×240 RGB565 ≈ 115 KB + buffer decode vs kode hitungan KB), animasi = interpolasi parameter (bukan sprite), dan preseden RoboEyes (gambar via primitif, nol bitmap). Lisensi RoboEyes GPL-3.0 → **reimplementasi konsep, jangan copy code**.

### 5.1 Model parameter (dari prototipe web "Wajah Agent" — living spec)

| Parameter | Deskripsi |
|---|---|
| `eye.w`, `eye.h`, `eye.r` | Lebar, tinggi, border-radius (squircle) tiap mata; kiri/kanan boleh beda |
| `eye.x`, `eye.y`, `eye.spacing` | Posisi dan jarak antar mata |
| `mood` | `DEFAULT`, `HAPPY`, `TIRED`, `ANGRY` (+ `SAD`, `CONFUSED` bila perlu) — memengaruhi geometri mata (mis. HAPPY = mata menyipit jadi busur, TIRED = setengah tertutup, ANGRY = miring) |
| `mode` | `EYES` (normal), `WAVEFORM` (listening/speaking), `SPHERE` (thinking) |
| `waveform` | Amplitudo/fase untuk mode waveform (mendengar vs berbicara bisa beda pola) |
| `sphere` | Blob shapeshifting: radius(θ) = R·(1 + a₁·sin(3θ+t) + a₂·sin(5θ−t)) + pulse (modulasi brightness/radius mengikuti detak) |
| `status_text` | Teks indikator pulsating: `THINKING…` / `PROCESSING…` / `COOKING…`, `LISTENING…`, `SPEAKING…` — nanti di-feed dari state LLM/harness asli |
| `blink`, `gaze` | Autoblinker (interval random) + idle mode (lirikan random) |

### 5.2 Konsep RoboEyes yang direimplementasi

- **Parameterized eyes** — semua bentuk mata = fungsi dari (w, h, r, spacing); tidak ada gambar statis.
- **Mood sebagai transformasi geometri** — `setMood()` hanya mengubah parameter target.
- **Smooth transition via interpolasi** — setiap frame, parameter aktual bergerak menuju target dengan easing (lerp + easing curve); inilah yang membuat animasi terlihat "mahal".
- **Autoblinker** — timer dengan interval + variasi random → picu animasi kedip (tutup-buka).
- **Idle mode** — timer random → geser posisi gaze (N/NE/E/…).
- **One-shot animations** — `blink()`, `confused` (goyang kiri-kanan), `laugh` (goyang atas-bawah) sebagai fungsi pemicu.

### 5.3 Struktur file/fungsi yang disarankan

Program userspace `user/face/` (atau `uface`):

```
user/face/
  face.h        — API publik: face_state_t, face_mood_t, face_mode_t;
                  face_init(fb, w, h), face_set_mood(), face_set_mode(),
                  face_set_thinking_status(const char *),
                  face_update(dt_ms), face_draw()
  face.c        — state machine + interpolasi (param aktual → target, easing)
  face_draw.c   — primitif: draw_squircle (rounded-rect fill),
                  draw_waveform, draw_sphere (blob parametrik),
                  draw_text (font bitmap minimal)
  font5x7.h     — font 5×7 untuk teks indikator (public domain)
  main.c        — loop: tick (timer/delay) → face_update() →
                  face_draw() ke framebuffer → flush dirty region
                  via SYS_SPI_XFER (+ GPIO untuk DC)
```

Framebuffer: RGB565 (format native ST7789), 240×240×2 = 115.200 byte.
Teks pulsating: modulasi brightness teks dengan `sin(t)`.
Catatan SFX: Web Audio hanya ada di prototipe; di hardware, RV1103 punya audio codec built-in — beep sederhana via codec/PWM bisa jadi fase lanjutan (prioritas rendah).

Alur data saat jadi: `harness (state) → face_set_*() → interpolasi → framebuffer → SYS_SPI_XFER → ST7789`.

---

## 6. Status jalur UI lain

| Jalur | Status | Catatan |
|---|---|---|
| Web dashboard HTTP | **Sudah ada** (Fase 12/12d) | Live-generated per request + `/metrics`; di QEMU via virtio-net |
| Akses dashboard via USB | **Rencana** | Pola standar board Luckfox: **RNDIS/ECM gadget** — board tampil sebagai network adapter USB di PC/HP, dashboard dibuka dari browser. Butuh USB device stack di QaonicOS (kerja besar, lihat §7). Di hardware nyata ini satu-satunya jalur network (tidak ada Ethernet fisik). |
| TUI `umon` | **Sudah ada** (Fase 17) | Via UART hari ini; nol hardware tambahan; tetap berguna sebagai fallback/debug |

---

## 7. Rekomendasi + rencana fase

### Rekomendasi

1. **Display: ST7789 240×240 SPI** (kandidat utama; ILI9341 bila butuh area lebih besar; SSD1306 hanya untuk status minimal). Alasan: square cocok untuk face, SPI sederhana, modul murah dan umum.
2. **Software: renderer custom minimal dulu, LVGL belakangan.** Face agent tidak butuh widget — renderer ~500–800 baris C cukup. LVGL dipertimbangkan ulang hanya bila muncul kebutuhan UI widget penuh (menu, settings).
3. **Arsitektur: hybrid (§4 opsi c)** — bit-bang untuk bring-up, lalu driver SPI0 kernel + `SYS_SPI_XFER`.

### Rencana fase (estimasi effort kasar)

| Fase | Isi | Effort estimasi |
|---|---|---|
| U1 | Bring-up SPI bit-bang via GPIO: validasi wiring + init-sequence ST7789, tampilkan pola tes statis | 1–2 hari (userspace saja) |
| U2 | Driver SPI0 di kernel (polling) + syscall `SYS_SPI_XFER` | 3–5 hari |
| U3 | Framebuffer userspace + flush dirty-region; init display pindah ke userspace via `SYS_SPI_XFER` | 2–3 hari |
| U4 | Port face renderer C dari prototipe web (mata/mood/waveform/sphere/teks) | ±1 minggu |
| U5 | Polish: autoblink/idle (sudah di spec), DMA SPI (opsional), SFX via audio codec (opsional) | menyusul |
| U6 (future) | Port LVGL bila butuh widget UI | 2–3 minggu |
| U7 (future) | USB RNDIS/ECM gadget agar web dashboard bisa diakses di hardware nyata | 3–4 minggu (USB stack) |

Estimasi di atas untuk satu engineer yang sudah familiar dengan codebase QaonicOS, dengan verifikasi per fase di QEMU (mock SPI) + hardware.

---

## Sumber

**Hardware:**
- Datasheet RV1103 v1.4 + skematik board: `~/workspace/luckfox-pico-mini-b/documents/` (repo lokal, diekstrak via pdftotext)
- Rockchip SPI Developer Guide (clock maks RV1103 50 MHz): https://lo01.g77k.com/aeb/docs/en/Common/SPI/Rockchip_Developer_Guide_Linux_SPI_EN.pdf
- Repo resmi Luckfox `spi/fb7789.py` (SPI0 @ 40 MHz untuk ST7789): `~/workspace/luckfox-pico-mini-b/spi/`
- Catatan bring-up ST7789 di Pico independen: https://github.com/bubbletoptag/neodct/blob/HEAD/docs/HARDWARE_NOTES.md
- Spec modul ST7789 (daya): https://www.scribd.com/document/512133947/ST013-01
- Konsumsi daya ILI9341: https://forum.pjrc.com/index.php?threads/display_ili9341_touch-power-consumption.66354/

**Software:**
- LVGL CHANGELOG (v9.6.0): https://github.com/lvgl/lvgl/blob/HEAD/docs/src/changelog/CHANGELOG.mdx
- LVGL README (lisensi MIT, kebutuhan RAM/Flash): https://github.com/lvgl/lvgl/blob/HEAD/README.md
- LVGL porting display: https://github.com/open-vela/apps_graphics_lvgl/blob/HEAD/docs/porting/display.rst
- LVGL porting guide (tick, flush_cb): https://github.com/lvgl/lvgl/blob/HEAD/docs/src/getting_started/porting.mdx
- Adafruit GFX: https://registry.platformio.org/libraries/adafruit/Adafruit%20GFX%20Library
- TFT_eSPI (lisensi GPL-3.0): https://www.libhunt.com/compare-Adafruit-ST7735-Library-vs-TFT_eSPI?ref=compare
- µGUI: https://github.com/rsta2/circle/blob/HEAD/addon/ugui/README.md
- emWin (lisensi per keluarga MCU): https://www.biospace.com/nxp-semiconductors-to-offer-emwin-graphic-library-free-with-arm-microcontrollers
- Embedded Wizard: https://en.wikipedia.org/wiki/Embedded_Wizard
- RoboEyes (referensi wajah, GPL-3.0): https://github.com/FluxGarage/RoboEyes

**Konteks internal:** prototipe web "Wajah Agent" (living spec), dokumen riset agent runtime harness (`~/workspace/your_files/riset-agent-runtime-harness/RINGKASAN.md`).
