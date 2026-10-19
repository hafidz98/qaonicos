# Verifikasi GUI QaonicOS via QEMU Capture

Selain test code (PASS/FAIL di serial log), GUI diverifikasi secara visual
melalui capture QEMU. Ini bukti tampilan beneran ke-render, bukan cuma
klaim dari kode.

## 1. Verifikasi Shell/CLI (`scripts/verify-shell.sh`)

Boot QEMU dengan serial interaktif (unix socket), kirim perintah ke prompt
`qaon>`, verifikasi respons.

```sh
./scripts/verify-shell.sh
```

Yang diverifikasi:
- Prompt `qaon>` muncul (boot selesai)
- `help` → daftar perintah
- `uptime` → waktu jalan
- `time` → jam dinding

Output: transkrip interaksi di `/tmp/shell-verify.log` + `SHELL-VERIFY: PASS/FAIL`.

**Known issues (bukan kegagalan verifikasi):**
- `ls` / `cat` gagal baca filesystem via shell (`sys_readdir`/`read` return <0).
  Bug terpisah di layer filesystem, shell-nya sendiri responsif.

## 2. Verifikasi Display/Face (`scripts/capture-gui.sh`)

Boot QEMU dengan `virtio-gpu-device`, ambil screenshot via QEMU monitor
`screendump` setelah face jalan.

```sh
./scripts/capture-gui.sh [output.png]
```

Yang diverifikasi:
- Display 240x240 aktif (`face: display 240x240 ok` di serial log)
- Screenshot menunjukkan face ter-render (bukan layar hitam)

**Known issues:**
- Boot dengan GPU kadang panic (data abort) setelah face jalan — investigasi
  terpisah. Capture diambil sebelum panic.

## 3. Kapan dijalankan

- Setiap perubahan pada `user/face/`, `user/uiapp/`, `user/sh.c`, atau
  driver display.
- Sebelum merge ke `core`: lampirkan screenshot + transkrip shell.

## Riwayat

- 2026-10-19: metodologi dibuat atas permintaan Hafidz ("bukan cuma test
  code doang, minimal shell/cli").
