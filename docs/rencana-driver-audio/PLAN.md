# Plan: Implementasi Driver Audio RV1103 (D1–D4)

**Tanggal:** 2026-10-19 · **Riset:** `docs/riset-audio-rv1103/RINGKASAN.md`
(F3) · **Acc implementasi:** Hafidz 2026-10-19 ("Ok lanjut next")

## Konteks

Capture mic 16 kHz mono PCM16 LE untuk voice uplink Qabot (Opsi B).
Jalur data: codec ACODEC → I2S RX → DMA → ring buffer kernel →
syscall → uplink ESP32-C6 (frame `AUD`, protokol v2 F5).

Keputusan F3: streaming mentah ke cloud (bukan on-device STT).
Caveat: pin mic kemungkinan unconnected di PCB Pico Mini → verifikasi
fisik masuk H4 (PC-agent).

## Pola implementasi

Mengikuti `gpio.c`: `#if defined(BOARD_RV1103)` → MMIO asli;
selain itu (QEMU) → virtual register file di RAM, offset identik.
QEMU tidak punya model ACODEC → D1/D2 terverifikasi sebagai *code
path* (write → readback cocok), bukan silikon. Verifikasi silikon = H4.

## Tahap

### D1 — ACODEC init + register readback ✅ SELESAI 2026-10-19
- File: `kernel/kernel/arm/audio.c` (+ deklarasi di header yg sesuai).
- Register (offset dari `0xFF480000`, dari `rv1106_codec.h`):
  `ANA_CTL0=0x88`, `ANA_CTL1=0x8c`, `ANA_CTL3=0x94`, `ANA_CTL6=0xa0`,
  `REG8=0x20`, `REG9=0x24`.
- GRF route (RV1103B, exact dari driver):
  SYS_GRF base `0xFF000000`; `AUDIO_CON0=0x50020`
  (mask `(0x3<<19)|(0x3<<21)|(0x3<<23)|(0x3<<25)`,
  sel `(0x3<<3)|(0x3<<5)|(0x3<<7)|(0x3<<9)`),
  `AUDIO_CON1=0x50024` (mask `(0x7<<16)|(0x3<<21)`, IO_SEL_DIS).
  Write pakai skema HIWORD (mask<<16 | val).
- Clock/reset: PCLK_ACODEC gate `PERICLKGATE_CON(6)` bit 3;
  MCLK_ACODEC_TX gate `PERICLKGATE_CON(6)` bit 4, div
  `PERICLKSEL_CON(8)` bits 0–7; reset `SRST_P_ACODEC`.
  **TODO:** alamat absolut CRU (base `0xFF3A0000` + offset
  PERICLKGATE_CON) butuh TRM CRU chapter — tandai jelas di kode,
  verifikasi di H4. Asumsi sementara: bootloader sudah enable clock.
- Test: `user/uaudiotest.c` one-shot saat boot (pola `utimeset.c`):
  `acodec_init()` → readback semua register → `UAUDIO: PASS/FAIL`.
  Di QEMU PASS = write/readback konsisten via virtual reg file.
- **Hasil:** `UAUDIO: PASS` di QEMU (ANA_CTL0=0x10, CTL1=0xee,
  CTL2=0xa0, CTL3=0x33, CTL6=0xff — semua sesuai bit-def driver).
  Syscall baru: `SYS_AUDIO_INIT` (82), `SYS_AUDIO_REG` (83).

### D2 — I2S RX + DMA ch 21 + ring buffer
- I2S base `0xFFAE0000`; DMA RX channel 21.
- Ring buffer kernel 8 KB (pola TCP RX Q2a); `SYS_AUDIO_READ`
  blocking + timeout ala `tls_bio.c`.
- Verifikasi QEMU: terbatas (tanpa model ACODEC); target hardware:
  capture 10 dtk = 320000 byte.

### D3 — START/STOP + gain + overrun counter
- `SYS_AUDIO_START(16000,16,1)`, `SYS_AUDIO_STOP()`, gain
  (`init-mic-gain` 0x22 = 20+20 dB, range 0–48 dB), overrun counter.
- Verifikasi: H4.1.

### D4 — VOICE.START → uplink C6
- Integrasi protokol v2 F5: `VOICE.START` → streaming frame `AUD`
  biner via UART 921600 ke ESP32-C6.
- Verifikasi: H4.2.

## Kriteria selesai keseluruhan
- D1–D3: kode + test boot di QEMU (PASS via virtual reg file).
- D4: kode + protokol v2 freeze; uji real menunggu hardware (H4).
- Commit per tahap di branch `qabot/q2-tls-provider`.
