# PLAN — Qabot Q4 + Q7 + Q8 + utang teknis

**Track:** Qabot harness · **Tanggal:** 2026-09-29 · **Status:** riset selesai,
implementasi belum mulai
**Prasyarat:** Q1 (harness core) + Q2 (TLS + provider real) selesai.
**Catatan proses:** daftar fase ini DITURUNKAN dari riset di bawah, bukan
dari kepala — koreksi Hafidz 2026-09-29 ("riset + plan doc dulu buat bikin
daftar phase").

## 0. Temuan riset (dasar daftar fase)

1. **Face sudah siap dirender** (`user/face/`): `EXPR_LISTENING`,
   `EXPR_SPEAKING`, `EXPR_THINKING`, `EXPR_IDLE` + `status_text[24]`
   ("LISTENING..." dsb., sesuai permintaan Hafidz di prototipe web).
   Yang BELUM ada: jalur IPC harness → face. Face adalah display server
   persisten (App A2); harness adalah program userspace terpisah.
2. **Pola IPC yang konsisten**: display sudah pakai syscall 60–68.
   Jalur termurah & konsisten = syscall baru, bukan shared memory.
3. **Qabot masih one-shot** (`uqabotr.c`): prompt hardcoded, exit setelah
   selesai. Agar face bermakna (THINKING → SPEAKING → IDLE terlihat),
   qabot butuh mode **daemon persisten** yang baca prompt dari console.
4. **Input console ada**: `SYS_READ_CONSOLE` (59) → byte mentah
   non-blocking via `cnmaygetc()`. Cukup untuk line editor shell.
5. **Syscall siap pakai untuk tool baru**: FAT_READ/WRITE/READDIR
   (53/54/56), UPTIME (67), TIME_GET (70), GPIO (40/41, sudah dipakai).
   Tanpa syscall baru untuk Q7.
6. **Shell = program userspace biasa**; peluncuran program lain belum ada
   (koordinasi via init/sentinel). Q8 minimal = built-in saja.

## 1. Q4 — Integrasi harness ↔ face

**Tujuan:** status agent terlihat di wajah Qabot.

- **Syscall 78 `SYS_FACE_EXPR`**: r0=expr (0–7, `face_expr_t`), r1=ptr
  teks status (≤23 char) atau 0. Kernel simpan di global
  `face_req_expr` / `face_req_text` + flag `face_req_pending`.
- **Face** (`user/face/main.c`): tiap iterasi loop, cek flag → panggil
  `face_set_expr()` + salin `status_text`, clear flag.
- **Qabot daemon** (`user/qabotd.c`, baru): loop persisten —
  baca 1 baris prompt dari console → `EXPR_LISTENING` saat mengetik/
  menunggu, `EXPR_THINKING` selama `provider.chat()`, `EXPR_SPEAKING`
  saat emit final, kembali `EXPR_IDLE` saat selesai.
- **Kriteria:** di QEMU, jalankan qabotd + face; ketik prompt →
  ekspresi berubah THINKING → SPEAKING → IDLE; teks status tampil.
  Verifikasi visual via screenshot ("Lihat SSnya").

## 2. Q7 — Tool real tambahan

**Tujuan:** tool registry mencakup I/O file + info sistem (semua syscall
sudah ada; tanpa syscall baru).

| Tool | Syscall | Risiko |
|---|---|---|
| `file_read` (path) | FAT_READ (54) | ALLOW (baca saja) |
| `file_write` (path, data) | FAT_WRITE (53) | CONFIRM (tulis) |
| `file_list` (path) | READDIR (56) | ALLOW |
| `sys_uptime` | UPTIME (67) | ALLOW |
| `net_status` | info TCP/IP dari kernel | ALLOW |

- Klasifikasi risiko mengikuti pola Q1 (tulis = CONFIRM, baca = ALLOW).
- `get_info` diperkaya: sertakan uptime + versi.
- **Kriteria:** skenario uji mock: prompt "tulis catatan" → policy
  CONFIRM tercatat → file tertulis di FAT → `file_list` melihatnya.
  `QABOT: TOOLS7 PASS`.

## 3. Q8 — Shell (terakhir, sesuai urutan)

**Tujuan:** prompt interaktif `qaon>` di console.

- `user/sh.c`: line editor (echo, backspace, Enter) via
  `SYS_READ_CONSOLE`; parser perintah + argumen sederhana.
- Built-in: `help`, `ls`, `cat`, `gpio`, `time`, `uptime`, `face`,
  `qabot` (kirim prompt ke qabotd — via syscall 78? atau flag; detail
  saat implementasi).
- **Tanpa** peluncuran program eksternal di v1 (butuh spawn — di luar
  scope; catat sebagai Q9 kandidat).
- **Kriteria:** di QEMU via console: `qaon> help` → daftar perintah;
  `qaon> gpio get 40` → nilai pin; `qaon> time` → jam. 3/3 run hijau.

## 4. Utang teknis

1. **TCP keep-alive pacing** (dari Q2c): respons HTTP tanpa FIN menetes
   lambat/parsial di guest (flaky). Investigasi: path RX
   `tcp.c:tcc_on_data`, interaksi slirp, pacing ACK. Target: keep-alive
   andal atau dokumentasi limitasi resmi.
2. **`sys_time_set`**: `TIME_GET` kembali lebih kecil dari yang di-set
   (`0x6abf5a00` → `0x6abba840`). Kandidat: `dt/100u` di `user.c`
   vs satuan `arm_timer_ticks()`. Selidiki + perbaiki.
3. **`mbedtls_strerror` hang**: selidiki root cause (dugaan: panggil
   fungsi yang blokir); bila perlu, tulis `tls_strerror` minimal sendiri.
4. **Entropy**: `mbedtls_hardware_poll` (timer+xorshift) LEMAH — catat
   resmi butuh TRNG di RV1103; tanpa TRNG, TLS di HW = kunci lemah.

## 5. Urutan kerja

Q4 → Q7 → Q8 → utang teknis. Tiap fase: implementasi → verifikasi QEMU
→ DEVLOG → commit. Q3/Q5/Q6 (API key real, STT, System-1) tetap deferred
milik Hafidz.
