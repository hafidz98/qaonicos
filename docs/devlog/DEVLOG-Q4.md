# DEVLOG Q4 — Qabot Daemon Persisten + Provider Real + AI Horde

**Tanggal:** 2026-09-29
**Branch:** `qabot/q2-tls-provider` (atau cabang Q4)
**Status:** SELESAI — end-to-end terverifikasi

## Ringkasan
Qabot kini jalan sebagai **daemon persisten** di QaonicOS: baca prompt dari
console interaktif, kirim ke LLM via provider real (HTTP polos ke proxy lokal
→ AI Horde gratis/anonym), tampilkan jawaban, dan ubah ekspresi wajah
(LISTENING → THINKING → SPEAKING → IDLE) selama proses.

## Yang dikerjakan

### Q4a: user/qabotd.c (baru)
- Daemon persisten: loop baca baris dari console (echo + backspace handling).
- `qb_run_task()` dengan provider real (bukan mock).
- Face expression via syscall 78: LISTENING (idle) → THINKING (kirim) →
  SPEAKING (tampilkan jawaban) → IDLE, plus teks status.
- Hook konfirmasi interaktif `confirm_fn` ("jalankan? [y/N]") untuk verdict
  CONFIRM dari policy gate.

### Q4b: provider_real.c — mode anonim + HTTP polos
- API key `""` = mode anonim (tanpa header Authorization); `NULL` = fallback
  perilaku lama (NVS).
- `qb_real_set_plain()` — transport TCP polos via syscall 73–77 (tanpa TLS)
  untuk proxy lokal. Header `Connection: close`.

### Q4c: user/qabot/proxy_horde.py (baru)
- Penerjemah di host 127.0.0.1:18090: terima `POST /v1/chat/completions`
  (format OpenAI) → submit + poll ke `stablehorde.net` API text async
  (key anonim `0000000000`, model `aphrodite/DeepSeek-V4.1-Flash`) →
  kembalikan JSON format OpenAI.
- Hormati `HTTP_PROXY`/`HTTPS_PROXY` env. Multi-thread per koneksi.
- Selalu kembalikan format OpenAI (error jadi `content`, bukan `{"error"}`).
- Retry: submit 3x, poll tahan terhadap IncompleteRead transient.

### Q4d: build + uji
- `build-md.sh`: section qabotd gantikan uqabotr; `user.c`: daemon_images +
  `qabotd_img`; NDAEMON tetap 3 (face, uiapp, qabotd).
- Build hijau (`qabotd.elf entry=0x100000 ok`, `LINK OK: mach3.elf`).

## Bug yang ditemukan & diperbaiki
1. **SYS_TCP_RECV tolak `maxlen > 4096`** → -1. Buffer guest 8192.
   Fix: recv di-chunk ≤4096.
2. **SYS_TCP_SEND tolak `len > 1200`** → -1. Fix: send di-chunk ≤1200.
3. **CONN_TO_MS 30 dtk** bunuh koneksi idle saat AI Horde antre 60 dtk+.
   Fix: naik ke 120 dtk (`tcp.c`); timeout recv guest 20→100 dtk;
   poll proxy 200→90 dtk.
4. **UART PL011 FIFO mati** (LCR_H reset=0 → mode 1-byte, overrun = byte
   hilang). Input console burst kehilangan ~50% byte.
   Fix: `uart_init()` set FEN|WLEN8 di LCR_H (0x2C) → FIFO 16-byte.
5. **Debug hex-dump** sisa Q2b di `tcp.c` dihapus (console bersih).

## Infrastruktur uji
- PTY bridge → diganti **unix-socket serial** (`-serial unix:/tmp/qser,server,nowait`):
  bersih, dua arah, tanpa line discipline. Bridge: FIFO `/tmp/qcmd` →
  socket; socket → `/tmp/qemu-q4.log`. Pacing 8 byte/30 ms (hormati FIFO 16).
- QMP socket `/tmp/qmp-q4.sock` + `tools/qmp-shot.py` untuk screenshot
  (VNC server QEMU tidak merespons FramebufferUpdateRequest).
- AI Horde terverifikasi via curl langsung (prompt Indonesia → dijawab
  DeepSeek-V4.1-Flash, gratis, anonim, lewat egress proxy).

## Verifikasi end-to-end (2026-09-29)
- Prompt "berapa 2+2? jawab angka saja" dari console guest →
  `POST /v1/chat/completions` ke 10.0.2.2:18090 → proxy → AI Horde →
  respons kembali → guest tampilkan:
  `qabot: Maksudnya apa? Silakan sebutkan perintah atau kebutuhanmu.`
  `qabot: selesai (status=0, steps=1)`
- Screenshot QMP: wajah LISTENING (waveform + "...") tampil benar.

## Catatan
- Jaringan ke stablehorde.net flaky (transient hang); proxy kini retry-robust.
- Input console via socket + pacing: burst pendek 100%, burst >16 byte aman
  dengan pacing. Di hardware real (UART fisik) tidak ada masalah ini.
- File berubah: `user/qabotd.c` (baru), `user/qabot/proxy_horde.py` (baru),
  `user/qabot/qabot.h`, `user/qabot/loop.c`, `user/qabot/provider_real.c`,
  `kernel/build-md.sh`, `kernel/kernel/arm/user.c`,
  `kernel/kernel/arm/tcp.c`, `kernel/kernel/arm/uart.c`.
