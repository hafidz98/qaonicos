# Prompt: Implementasi Device UI App ke QaonicOS

(Copy-paste ke channel WA untuk mulai implementasi.)

---

Lanjutkan implementasi aplikasi UI device (Qabot + uiapp) ke dalam repo QaonicOS.

**Lokasi kerja**: `~/workspace/qaonic_os/` (repo git, HEAD `6c540bc`, Fase D selesai penuh — TCP/HTTP terverifikasi fungsi).

**Dokumen acuan** (baca dulu sebelum mulai):
1. `~/workspace/your_files/rencana-app-qaonicos/RENCANA.md` — rencana app: arsitektur multi-process, protokol display token, protokol UART v1 ke ESP32-C3, fase A0–A4, Qabot & power management (§11), API companion (§12)
2. `~/workspace/your_files/rencana-face-qaonicos/RENCANA.md` — rencana face (model parameter, struktur file)
3. `~/workspace/qaonic_os/docs/SYSCALL-ABI.md` — syscall ABI (WRITE=20, YIELD=21, EXIT=22, SBRK=24, file 30–34, GPIO 40–41, dst.)
4. `~/workspace/qaonic_os/docs/MIGRASI-MACH3.md` — status migrasi kernel Mach 3
5. Mockup visual (referensi tampilan): https://muse.ai/s/mockup-ui-qaonicos-cd6waoggxhx0g

**Yang sudah ada**: `user/face/` (untracked, belum di-commit) berisi renderer C portable — `face.h`, `face.c`, `face_draw.h`, `face_draw.c`, `font5x7.h` — plus `host_test/` yang sudah me-render 8 ekspresi (idle, listening, speaking, thinking, happy, surprised, angry, tired) ke PNG. Jadi A0 praktis selesai.

**Keputusan arsitektur (final, jangan diubah tanpa konfirmasi)**:
- Multi-process ala Mach: `user/face` = Qabot, IPC server + pemilik display/framebuffer/input/power-state-machine; `user/uiapp` = menu/settings/monitor/power, client display via protokol token (`DISPLAY.GRANT`/`ACQUIRE`/`RELEASE`/`FLUSH`), framebuffer shared via Mach VM.
- Qabot = default/home screen. Idle di menu 30 dtk → auto-return ke Qabot. Idle di Qabot 120 dtk → display off. Display off 300 dtk → sleep. Input apa pun = wake.
- Non-touch: navigasi tombol fisik (UP/DOWN/LEFT/RIGHT/OK/BACK via GPIO, syscall 40–41); di QEMU pakai mapping keyboard. Model navigasi: select-dulu-baru-masuk.
- Status bar permanen: jam kiri, baterai kanan.

**Task — mulai A1**: jadikan `user/face` program user QaonicOS yang jalan di QEMU:
1. Build sebagai user program (ikuti pola `user/hello.c` + file `.ld` + `embed.py`; daftarkan di init).
2. Backend flush untuk QEMU — pakai virtio-gpu 2D, target 240×240 RGB565, framebuffer parsial 1/10 (heap per task 64KB, frame penuh 115KB tidak muat).
3. Render loop Qabot (ekspresi idle) dengan pacing via `SYS_YIELD` (target ~30fps).
4. Verifikasi beberapa run QEMU hijau (boot → face tampil, tidak hang/crash) sebelum lanjut.

**Aturan kerja**:
- Repo ini juga dikerjakan background agent — selalu `git status` dan `git log --oneline -5` dulu sebelum mulai; kerja di branch baru `app/a1-face-qemu`; jangan push/commit ke main tanpa verifikasi hijau.
- `user/face/` masih untracked — commit-kan sebagai bagian dari kerja A1 ini.
- Patuhi pelajaran di `~/AGENTS.md` (framebuffer parsial, heap 64KB/task — kalau butuh heap lebih besar, catat eksplisit sebagai kebutuhan, jangan diam-diam).
- Jangan expose VM ke publik untuk alasan apa pun.

Laporkan progres per milestone, dan kabari kalau ada blocker yang butuh keputusan saya.
