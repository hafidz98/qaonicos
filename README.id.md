# QaonicOS

Sistem operasi untuk **Luckfox Pico Mini** (Rockchip RV1103, Cortex-A7, 64MB RAM),
dikembangkan dan diuji di QEMU (`-M virt -cpu cortex-a7`) sebelum dibawa ke hardware asli.

Kernel = **mikrokernel Mach 3.0 asli (CMU)**, di-port ke ARMv7. Di atasnya berjalan
userspace lengkap: shell interaktif, network stack, HTTP server, dan **Qabot** —
agent runtime harness on-device.

> 🇬🇧 English version: [README.md](README.md)

## Struktur repo

```
qaonic_os/
├── mach3-port/      # Port Mach 3.0 (CMU) ARMv7 — workspace build (peta layout:
│                    #   mach3-port/README.md): upstream/ = Mach 3.0 murni,
│                    #   overrides/ = file MI yang di-patch, src/ = source port kami
│                    #   (md/arm = glue MD Mach, ext/ = extension layer),
│                    #   build-md.sh/build-mi.sh = build, history/ = laporan M1–M6
├── user/            # Program userspace
│   ├── ulib/        #   ulib: libc mini + wrapper syscall (usys.h)
│   ├── qabot/       #   Qabot harness: loop ReAct, policy gate, tool registry,
│   │                #   provider LLM (mock + real via HTTPS)
│   ├── tls/         #   mbedTLS: TLS 1.2 client (UTLSCLI)
│   ├── face/        #   Qabot face (home screen) + display server
│   ├── uiapp/       #   UI app: menu, power FSM
│   ├── uartproto/   #   Protokol UART v1 (komunikasi co-MCU)
│   ├── cfg/         #   Konfigurasi persisten (NVS)
│   ├── sh.c         #   Shell interaktif `qaon>` (daemon console)
│   ├── qabotd.c     #   Qabot daemon persisten
│   └── ...          #   init, ucat, uls, umon, tui, ntp, ugpio, utcpcli, ...
├── tools/           # mkfat32.py (generator image FAT32), pem2c.py, mock_openai_https.py, ...
├── scripts/         # run-qemu.sh — boot di QEMU
├── hw/dts/          # Device tree RV1103 (referensi alamat hardware)
├── docs/            # Dokumentasi: SYSCALL-ABI.md, MIGRASI-MACH3.md, roadmap,
│                    #   rencana-qabot-harness/ (PLAN-Q1..Q9), rencana-face-qaonicos/,
│                    #   rencana-app-qaonicos/, devlog/, UTANG-TEKNIS.md
├── disk-images/     # Image QEMU (dibuat otomatis, tidak di-commit)
├── LICENSE          # Lisensi Apache 2.0
└── archive/         # kernel-scratch/ = kernel from-scratch Fase 1–17 (ARSIP,
                     #   referensi driver net/FAT32/GPIO + net stack pra-Mach)
```

## Cara build & run (QEMU)

Butuh: `clang` + `lld` + `qemu-system-arm` (atau toolchain portable sejenis).

```sh
cd mach3-port && ./build-md.sh   # build -> mach3-port/build/mach3.elf
./scripts/run-qemu.sh        # boot (Ctrl-A X untuk keluar)
```

Saat boot, self-test otomatis berjalan: pmap, IPC, blk, task, user, sched,
lalu `spawn embed:uls` — semua PASS.

## Status

- **Kernel**: Mach 3.0 (CMU) port ARMv7, rilis `v1.0` — pmap 4KB, IPC,
  virtio-blk, timer 100Hz, user mode + syscall, scheduler kooperatif.
- **Fase A–D** (selesai): migrasi ke kernel Mach 3 → syscall inti + userland →
  file syscall + ramfs → driver GPIO/SD/FAT32 + net stack (ARP/IPv4/ICMP/TCP/UDP)
  + HTTP server (dashboard system monitor live) + NTP + persistensi NVS.
- **Qabot** — agent runtime harness on-device (track Q1–Q9, selesai):
  Q1 loop ReAct + policy gate + guardrail → Q2 TLS 1.2 (mbedTLS) + JSON +
  provider LLM real via HTTPS → Q4 daemon persisten → Q7 tool real
  (file/uptime/net) → Q8 shell `qaon>` → Q9 syscall spawn (79/80/81).
- **App A1–A4** (selesai): face Qabot di QEMU (virtio-gpu) → display server +
  uiapp → protokol UART v1 + layar WiFi/BLE/LLM/Passkey → sinkron jam NTP +
  persistensi setting.
- **v0.1** (QEMU): stabilitas multi-boot 3/3 PASS, tanpa panic.
- **Berikutnya**: v0.2 = hardware Luckfox Pico Mini fisik.

Catatan jujur: verifikasi end-to-end tool Qabot via LLM butuh proxy di jaringan
normal (sandbox blokir UDP); detail limitasi ada di `docs/UTANG-TEKNIS.md`.

## Referensi hardware

- Luckfox Pico Mini: single-core Cortex-A7 1.2 GHz, 64MB DDR2, 128MB SPI NAND.
- `hw/dts/` berisi device tree RV1103 sebagai referensi alamat hardware.
  Datasheet + schematic board tidak di-commit (lihat repo referensi Luckfox Pico).

## Sumber

QaonicOS berdiri di atas proyek upstream berikut. Masing-masing
di-vendor apa adanya di bawah lisensinya sendiri (lihat file lisensi
di tiap direktori); kode QaonicOS sendiri berlisensi Apache 2.0
(lihat [LICENSE](LICENSE)).

| Komponen | Versi | Upstream | Lisensi | Di-vendor di |
| --- | --- | --- | --- | --- |
| Mikrokernel Mach 3.0 | 3.0 (MK83) | https://github.com/Prajna/mach | Lisensi CMU (teks di bawah) | `mach3-port/upstream/mach3/` (pristine, tidak pernah diubah) |
| Library grafis LVGL | 9.5.0 | https://github.com/lvgl/lvgl | MIT | `user/uiapp/lvgl/` (subset: core, display, draw sw, Montserrat 14, widget button + label) |
| Filesystem littlefs | 2.9 | https://github.com/littlefs-project/littlefs | BSD-3-Clause | `mach3-port/src/ext/littlefs/` |
| mbedTLS | 3.6.7 | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 (dual Apache-2.0 OR GPL-2.0-or-later; kami memakai Apache-2.0) | `user/tls/mbedtls/` |

Referensi desain saja (tanpa kode): wajah Qabot mengimplementasikan
ulang konsep dari FluxGarage RoboEyes (GPL-3.0) dari nol; tidak ada
kode RoboEyes di dalam repo ini.

Build tools (tidak ikut di image): `clang`/`lld` dan
`qemu-system-arm`, hanya dipakai untuk build dan test.

## Lisensi

Lisensi Apache 2.0 — lihat [LICENSE](LICENSE).

Catatan: `mach3-port/upstream/mach3/` berisi mikrokernel Mach 3.0 © Carnegie Mellon University
yang tetap memakai lisensi asli CMU. Apache 2.0 berlaku untuk kode QaonicOS
sendiri: driver, program userspace, tools, dan dokumentasi.

### Lisensi asli Mach 3.0 (CMU)

Sumber Mach 3.0 membawa notice berikut di header tiap file
(`mach3-port/upstream/mach3/kernel/`), dikutip di sini karena
lisensinya mensyaratkan notice tampil di dokumentasi pendukung
(teks hukum dikutip apa adanya dalam Bahasa Inggris):

> Mach Operating System
> Copyright (c) 1992 Carnegie Mellon University
> All Rights Reserved.
>
> Permission to use, copy, modify and distribute this software and its
> documentation is hereby granted, provided that both the copyright
> notice and this permission notice appear in all copies of the
> software, derivative works or modified versions, and any portions
> thereof, and that both notices appear in supporting documentation.
>
> CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS-IS"
> CONDITION. CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
> ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
