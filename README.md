# QaonicOS

An operating system for the **Luckfox Pico Mini** (Rockchip RV1103, Cortex-A7, 64MB RAM),
developed and tested on QEMU (`-M virt -cpu cortex-a7`) before moving to real hardware.

The kernel is the genuine **Mach 3.0 microkernel (CMU)**, ported to ARMv7. On top of it
runs a complete userspace: an interactive shell, a network stack, an HTTP server,
and **Qabot** — an on-device agent runtime harness.

> 🇮🇩 Versi Bahasa Indonesia: [README.id.md](README.id.md)

## Repository structure

```
qaonic_os/
├── mach3-port/      # Mach 3.0 (CMU) ARMv7 port — build workspace (layout map:
│                    #   mach3-port/README.md): upstream/ = pristine Mach 3.0,
│                    #   overrides/ = patched MI files, src/ = our port sources
│                    #   (md/arm = Mach MD glue, ext/ = extension layer),
│                    #   build-md.sh/build-mi.sh = build, history/ = M1–M6 reports
├── user/            # Userspace programs
│   ├── ulib/        #   ulib: mini libc + syscall wrappers (usys.h)
│   ├── qabot/       #   Qabot harness: ReAct loop, policy gate, tool registry,
│   │                #   LLM provider (mock + real over HTTPS)
│   ├── tls/         #   mbedTLS: TLS 1.2 client (UTLSCLI)
│   ├── face/        #   Qabot face (home screen) + display server
│   ├── uiapp/       #   UI app: menu, power FSM
│   ├── uartproto/   #   UART protocol v1 (co-MCU communication)
│   ├── cfg/         #   Persistent configuration (NVS)
│   ├── sh.c         #   Interactive shell `qaon>` (console daemon)
│   ├── qabotd.c     #   Persistent Qabot daemon
│   └── ...          #   init, ucat, uls, umon, tui, ntp, ugpio, utcpcli, ...
├── tools/           # mkfat32.py (FAT32 image generator), pem2c.py, mock_openai_https.py, ...
├── scripts/         # run-qemu.sh — boot on QEMU
├── hw/dts/          # RV1103 device tree (hardware address reference)
├── docs/            # Documentation: SYSCALL-ABI.md, MIGRASI-MACH3.md, roadmap,
│                    #   rencana-qabot-harness/ (PLAN-Q1..Q9), rencana-face-qaonicos/,
│                    #   rencana-app-qaonicos/, devlog/, UTANG-TEKNIS.md
├── disk-images/     # QEMU images (generated, not committed)
├── LICENSE          # Apache License 2.0
└── archive/         # kernel-scratch/ = from-scratch kernel phases 1–17 (ARCHIVE,
                     #   reference for net/FAT32/GPIO drivers + net stack pre-Mach)
```

## Build & run (QEMU)

Requires: `clang` + `lld` + `qemu-system-arm` (or an equivalent portable toolchain).

```sh
cd mach3-port && ./build-md.sh   # build -> mach3-port/build/mach3.elf
./scripts/run-qemu.sh        # boot (Ctrl-A X to quit)
```

On boot, an automatic self-test runs: pmap, IPC, blk, task, user, sched,
then `spawn embed:uls` — all PASS.

## Status

- **Kernel**: Mach 3.0 (CMU) ARMv7 port, release `v1.0` — 4KB pmap, IPC,
  virtio-blk, 100Hz timer, user mode + syscalls, cooperative scheduler.
- **Phases A–D** (done): migration to the Mach 3 kernel → core syscalls + userland →
  file syscalls + ramfs → GPIO/SD/FAT32 drivers + net stack (ARP/IPv4/ICMP/TCP/UDP)
  + HTTP server (live system-monitor dashboard) + NTP + NVS persistence.
- **Qabot** — on-device agent runtime harness (tracks Q1–Q9, done):
  Q1 ReAct loop + policy gate + guardrails → Q2 TLS 1.2 (mbedTLS) + JSON +
  real LLM provider over HTTPS → Q4 persistent daemon → Q7 real tools
  (file/uptime/net) → Q8 `qaon>` shell → Q9 spawn syscalls (79/80/81).
- **Apps A1–A4** (done): Qabot face on QEMU (virtio-gpu) → display server +
  uiapp → UART protocol v1 + WiFi/BLE/LLM/Passkey screens → NTP time sync +
  settings persistence.
- **v0.1** (QEMU): 3/3 multi-boot stability PASS, no panics.
- **Next**: v0.2 = real Luckfox Pico Mini hardware.

An honest note: end-to-end verification of Qabot tools via LLM needs a proxy on
a normal network (the sandbox blocks UDP); limitations are documented in
`docs/UTANG-TEKNIS.md`.

## Hardware reference

- Luckfox Pico Mini: single-core Cortex-A7 @ 1.2 GHz, 64MB DDR2, 128MB SPI NAND.
- `hw/dts/` holds the RV1103 device tree as a hardware address reference.
  The datasheet + board schematics are not committed (see the Luckfox Pico
  reference repos).

## Sources

QaonicOS stands on the following upstream projects. Each is vendored
verbatim under its own license (see the license file in each directory);
QaonicOS's own code is Apache 2.0 (see [LICENSE](LICENSE)).

| Component | Version | Upstream | License | Vendored at |
| --- | --- | --- | --- | --- |
| Mach 3.0 microkernel | 3.0 (MK83) | https://github.com/Prajna/mach | CMU license (notice below) | `mach3-port/upstream/mach3/` (pristine, never edited) |
| LVGL graphics library | 9.5.0 | https://github.com/lvgl/lvgl | MIT | `user/uiapp/lvgl/` (subset: core, display, draw sw, Montserrat 14, button + label widgets) |
| littlefs filesystem | 2.9 | https://github.com/littlefs-project/littlefs | BSD-3-Clause | `mach3-port/src/ext/littlefs/` |
| mbedTLS | 3.6.7 | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 (dual Apache-2.0 OR GPL-2.0-or-later; we take Apache-2.0) | `user/tls/mbedtls/` |

Design reference only (no code taken): the Qabot face reimplements
concepts from FluxGarage RoboEyes (GPL-3.0) from scratch; no RoboEyes
code is included in this repository.

Build tools (not shipped in the image): `clang`/`lld` and
`qemu-system-arm`, used only to build and test.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Note: `mach3-port/upstream/mach3/` contains the Mach 3.0 microkernel © Carnegie Mellon University,
which retains its original CMU license. Apache 2.0 applies to QaonicOS's own
code: drivers, userspace programs, tools, and documentation.

### Mach 3.0 original license (CMU)

The Mach 3.0 sources carry the following notice in every file header
(`mach3-port/upstream/mach3/kernel/`), reproduced here because the
license requires the notices to appear in supporting documentation:

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
