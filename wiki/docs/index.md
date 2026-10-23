# QaonicOS — Wiki Home

QaonicOS is a single-purpose appliance OS for the Luckfox Pico Mini B
(Rockchip RV1103), built on the real **Mach 3** kernel. It boots to a
living agent face (Qabot), is configured entirely from the owner's
phone, updates itself with signed packages that can never brick it,
and — on hardware — pairs with an ESP32-C6 companion MCU that owns
Wi-Fi/BLE and private keys.

> **Key facts**
>
> | | |
> |---|---|
> | Hardware target | Luckfox Pico Mini B — RV1103, single-core Cortex-A7 @ **1.104 GHz**, 64 MB DDR2, 128 MB SPI NAND, 240×240 SPI display |
> | Companion | ESP32-C6 (Wi-Fi 6 / BLE 5, key residency) over UART4 @ 921600 baud — hardware phase |
> | Kernel | Genuine Mach 3 (CMU), frozen core + a hand-written extension layer |
> | Userspace | Four cooperative daemons (face, uiapp, qabotd, sh) + boot one-shots, embedded in the kernel image |
> | Development state | Runs and is verified in QEMU today; the physical board is the next hardware phase |
> | Product bar | Plug-and-play: boots, recovers and updates unattended — see [Boot & Resilience](boot-resilience.md) |

## What kind of OS is it, in plain words?

Textbook labels only half-fit. The research verdict
([`docs/riset-arsitektur-os/RINGKASAN.md`](../docs/riset-arsitektur-os/RINGKASAN.md)):

- **Hybrid in structure** — like XNU, there is a Mach substrate with a
  monolithic-style "personality" inside the kernel (the extension
  layer: block layer, network stack, HTTP/web UI, filesystems,
  bootstate, credential store). Mach's own threads and IPC are *not*
  the service mechanism; syscalls are the only kernel↔userspace
  crossing.
- **RTOS-shaped in composition** — the program set is fixed at build
  time and embedded in the kernel image. There is no general
  fork/exec Unix world, and none is planned without a product need.
- **Appliance-shaped in its rules** — the architecture is judged by
  out-of-the-box promises: 100/100 unattended cold boots, a
  power-cut-safe filesystem, a failed update falls back, three
  crashes land in recovery — never a brick.

## Where to start

=== "Humans"

    1. [Architecture](architecture.md) — the parts and the flows.
    2. [Hardware & SoC](hardware-soc.md) — what the silicon actually offers (several older assumptions were corrected by research).
    3. [Tracks & Roadmap](tracks-roadmap.md) — what is being built, in what order.
    4. [Development](development.md) — how to build and verify.

=== "AI agents"

    Start at the [AI Agent Index](ai-index.md): one line per page,
    the canonical source document for each topic, and repo layout
    pointers. Rule of precedence everywhere in this wiki:
    **code beats docs**; where docs conflict, the newest research wins.

## Page map

| Page | Covers |
|---|---|
| [Architecture](architecture.md) | Layers, component map, boot flow, the eight runtime flows |
| [Kernel / Userspace Boundary](kernel-userspace-boundary.md) | Placement rules R1–R14, verdicts on today's kernel residents, decisions D-ARCH-1..4 |
| [Hardware & SoC](hardware-soc.md) | RV1103 + Pico Mini B: corrected specs, capability matrix, unused-block leverage |
| [Boot & Resilience](boot-resilience.md) | Boot chain, bootstate/bootlog, safe mode, daemon-death reclaim, supervisor |
| [Storage](storage.md) | littlefs on `/data`, FAT32 on `/sd`, NAND layout, SDMMC future |
| [Networking & the C6 Link](networking-c6.md) | The in-kernel guest stack, the UART wire protocol, the C6L track |
| [Display, UI & Face](display-ui-face.md) | Procedural face renderer, uiapp, display arbitration, the LVGL plan |
| [Qabot Agent Runtime](qabot-agent.md) | ReAct harness, policy gate, connectivity modes A/B/C, voice |
| [Onboarding & Security](onboarding-security.md) | Phone-first setup, PIN, QR, web shell, credential store, signed updates |
| [Power & Battery](power-battery.md) | Power states, SARADC battery sensing, the PWR track |
| [Updates](updates.md) | A/B slots, QUP1 signed packages, trial/rollback, the FND-5 delivery gap |
| [Syscall ABI](syscall-abi.md) | Calling convention and the documented syscall surface (with an honest staleness warning) |
| [Development](development.md) | Build, QEMU verification methodology, dev-log and commit discipline |
| [Tracks & Roadmap](tracks-roadmap.md) | Locked track order, proposed tracks, hardware-gated work, parked items |
| [Glossary](glossary.md) | Project terms, defined once |
| [AI Agent Index](ai-index.md) | Machine-friendly coverage map and canonical sources |

## Status snapshot (2026-10-23)

- **Built and QEMU-verified:** Mach 3 port, extension layer, four
  daemons, display arbitration, kernel web UI (PIN-gated), credential
  store, signed A/B updates, boot resilience (soak: 100/100 boots,
  30/30 power cuts), audio path, onboarding ONB-1/ONB-2, FND-1
  (reset/poweroff), FND-2 (daemon-death reclaim).
- **In progress:** FND-3 (watchdog / health supervisor).
- **Queued:** FND-4, FND-5 → C6L-1..5 → UI-1..6 (locked order).
- **Researched, unscheduled:** SOC-1..6, PWR-1..4, THR-1..5.
- **Waiting on hardware:** board flash test, NAND/SPI drivers,
  C6 firmware, real sleep states.

Sources for this page:
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md),
[`docs/riset-arsitektur-os/RINGKASAN.md`](../docs/riset-arsitektur-os/RINGKASAN.md),
[`docs/riset-out-of-box/RINGKASAN.md`](../docs/riset-out-of-box/RINGKASAN.md).
