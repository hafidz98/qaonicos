# QaonicOS

Sistem operasi untuk **Luckfox Pico Mini** (Rockchip RV1103, Cortex-A7, 64MB RAM),
dikembangkan dan diuji di QEMU (`-M virt -cpu cortex-a7`) sebelum dibawa ke hardware asli.

Kernel = **Mach 3.0 asli (CMU)**, di-port ke ARMv7. Di atasnya berjalan userspace
lengkap: shell interaktif, network stack, HTTP server, dan **Qabot** — agent runtime
harness on-device.

## Struktur repo

```
qaonic_os/
├── kernel/          # Kernel Mach 3.0 (CMU), port ARMv7 — rilis v1.0
│                    #   kernel/ = source MI+MD, build-md.sh/build-mi.sh = build,
│                    #   docs/ = laporan porting M1–M6 + RELEASE
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
└── archive/         # kernel-scratch/ = kernel from-scratch Fase 1–17 (ARSIP,
                     #   referensi driver net/FAT32/GPIO + net stack pra-Mach)
```

## Cara build & run (QEMU)

Butuh: `clang` + `lld` + `qemu-system-arm` (atau toolchain portable sejenis).

```sh
cd kernel && ./build-md.sh   # build -> kernel/build/mach3.elf
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

## Lisensi

Kode kernel Mach 3.0 © Carnegie Mellon University (lisensi asli Mach tetap berlaku,
lihat `kernel/`). Kode QaonicOS (driver, userspace, tools) ditulis untuk proyek ini.
