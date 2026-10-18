# Riset: Audio Input RV1103 (F3)

**Tanggal:** 2026-10-18 · **Track:** PLAN-FONDASI.md F3 · **Status:** riset selesai,
implementasi = fase terpisah (butuh acc)
**Tujuan:** capture mic 16 kHz mono 16-bit untuk voice uplink Qabot (Opsi B).

## 1. Temuan kunci

Dokumen yang dibutuhkan **lengkap tanpa TRM RV1103 penuh**:

| Sumber | Isi | URL |
|---|---|---|
| TRM RV1106 V0.3 Part1, Ch. 21 "Audio Codec" (hlm 659–681) | Semantik bit-field ACODEC_REG0–REG73 | https://github.com/DeciHD/rockchip_docs/tree/main/rv1103_rv1106 |
| `sound/soc/codecs/rv1106_codec.{c,h}` (2741 baris, kernel 5.10) | **Urutan init yang terbukti jalan di silikon**; support `rockchip,rv1103-codec` via `of_device_id` | https://raw.githubusercontent.com/rockchip-linux/kernel/develop-5.10/sound/soc/codecs/rv1106_codec.c |
| `rv1103.dtsi` / `rv1106.dtsi` | Clock, reset, DMA channel | https://raw.githubusercontent.com/rockchip-linux/kernel/develop-5.10/arch/arm/boot/dts/rv1103.dtsi |

**Peringatan:** offset register analog di TRM V0.3 **tidak cocok** dengan
driver shipping (cth. TRM: REG34→0x58, driver pakai 0x88). Aturan:
**offset ← driver, semantik bit ← TRM.** Offset di bawah diverifikasi
langsung dari `rv1106_codec.h`.

## 2. Peta hardware

- **Base address ACODEC:** `0xFF480000` (64 KB, `reg = <0xff480000 0x1000>` di DTS).
- **I2S controller:** `0xFFAE0000` (`rockchip,rv1106-i2s-tdm`); data ADC
  mengalir **codec → I2S → DMA**, bukan langsung dari codec.
- **DMA RX:** `dmac` channel **21** (`dma-names = "rx"`); TX = 22.
- **Clock (DTS):** `pclk_acodec`, `mclk_acodec` (= `MCLK_ACODEC_TX`),
  `mclk_cpu` (= `MCLK_I2S0_8CH_TX`); reset `SRST_P_ACODEC`;
  GRF bit `acodec_ena=1` (pakai ACODEC internal, bukan via PAD).
- **Mic:** `acodec,micbias` + `init-mic-gain = <0x22>` = 20 dB L + 20 dB R
  (range 0–48 dB; `ACODEC_ADC_L_MIC_GAIN_20DB = (0x2<<6)`).
- **Sample rate:** driver lapor `SNDRV_PCM_RATE_8000_192000`; 16 kHz
  didukung (rate diset via DAI I2S + clock tree).

## 3. Urutan enable ADC capture (dari `rv1106_codec_adc_enable()`)

| # | Aksi | Register (offset dari 0xFF480000) |
|---|---|---|
| 1 | GRF: `acodec_ena` = 1 (pilih ACODEC internal) | GRF |
| 2 | Enable clock `pclk_acodec`, `mclk_acodec`; reset `SRST_P_ACODEC` | CRU |
| 3 | ADC ibias enable | `ACODEC_ADC_ANA_CTL0` = 0x88 |
| 4 | Mic L/R work | `ACODEC_ADC_ANA_CTL1` = 0x8c |
| 5 | Enable mic + ADC (`ACODEC_MIC_L/R_EN`, `ACODEC_ADC_L/R_EN`) | `ACODEC_ADC_ANA_CTL3` = 0x94 |
| 6 | ADC clock + ADC work (`esckl/esckr`) | `ACODEC_ADC_ANA_CTL6` = 0xa0 |
| 7 | ADC digital volume L/R | `ACODEC_REG8/9` = 0x20/0x24 |
| 8 | Konfigurasi I2S RX (16 kHz, 16-bit) + DMA ch 21 start | I2S 0xFFAE0000 |

## 4. Desain syscall: `SYS_AUDIO_READ`

Pola mengikuti TCP RX ring buffer (Q2a):

- `SYS_AUDIO_START(rate, bits, ch)` → 0/-1. Param valid: rate=16000,
  bits=16, ch=1 (mono). Menjalankan urutan §3 sekali.
- `SYS_AUDIO_READ(buf, len)` → byte terbaca (PCM16 LE, blocking dengan
  timeout ala `tls_bio.c`, yield saat tunggu).
- `SYS_AUDIO_STOP()` → 0. Matikan dalam urutan terbalik §3.
- Ring buffer kernel 8 KB (250 ms @16 kHz) antara ISR DMA dan syscall;
  overrun → drop tertua + counter (bisa dibaca untuk diagnostik).

Format: **16-bit LE, 16 kHz, mono** — cukup untuk STT, 32 KB/s.

## 5. Keputusan: streaming mentah vs on-device STT

**Keputusan: streaming mentah ke cloud (via ESP32-C6).**
Alasan: (a) 64 MB RAM dan CPU single-core tidak realistis untuk
whisper.cpp/Moonshine + harness + UI bersamaan; (b) F1 sudah
dideprioritaskan karena streaming lewat C6, bukan stack TCP QaonicOS;
(c) data plane biner UART sudah dirancang untuk ini (F5 §5:
frame `AUD`). On-device STT (wake-word ringan) = optimasi masa depan,
bukan fondasi.

## 6. Caveat hardware (penting)

Pin mic codec (`CODEC_MIC0P`/`MICBIAS`/`CODEC_MIC1P`) **tidak terlihat
tersambung ke konektor/pad** di skematik Pico Mini — kemungkinan
unconnected di board. Untuk uji real (H4): mic harus di-solder manual
ke pin QFN (sulit) atau cari test pad. **Verifikasi fisik oleh
PC-agent sebelum implementasi driver selesai** — driver tanpa mic
yang ter-wiring tidak bisa diuji.

## 7. Rencana driver per tahap (fase implementasi terpisah)

| Tahap | Isi | Verifikasi |
|---|---|---|
| D1 | ACODEC init: GRF + clock + reset + ANA_CTL0/1/3/6 (§3 langkah 1–7); baca-balik register | Register readback sesuai |
| D2 | I2S RX + DMA ch 21, ring buffer 8 KB, `SYS_AUDIO_READ` polling | Capture 10 dtk → 320000 byte, checksum stabil |
| D3 | `SYS_AUDIO_START/STOP`, gain control, overrun counter | H4.1 |
| D4 | Integrasi `VOICE.START` (protokol v2) → uplink ke C6 | H4.2 |

Estimasi: D1–D2 butuh baca `rockchip_i2s_tdm.c` + eksperimen register
di hardware (tidak bisa full di QEMU — tidak ada model ACODEC).

## 8. Checklist selesai F3

- [x] Dokumen riset: register map, clock, jalur DMA, desain syscall
- [x] Rencana driver per tahap (D1–D4, fase terpisah)
- [x] Keputusan STT vs streaming: streaming mentah via C6
- [ ] **Butuh acc Hafidz** untuk lanjut ke fase implementasi driver
