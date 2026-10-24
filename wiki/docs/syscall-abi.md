# Syscall ABI

The syscall ABI is the only kernel↔userspace contract in
QaonicOS (principle P2): documented, and grown by append. This
page describes the ABI's shape and its documented surface.

!!! danger "Staleness warning — read this first"
    The canonical list, [`docs/SYSCALL-ABI.md`](../docs/SYSCALL-ABI.md),
    is **28 syscalls stale** (foundation audit item #14): it
    documents the Fase B–D surface in detail, but the extension
    syscalls added by later tracks (78–107) were never backfilled
    into it. The backfill is a known, scheduled backlog item —
    **not done yet**. Below, Table 1 reproduces what
    SYSCALL-ABI.md documents; Table 2 lists the in-use numbers
    documented across track plans and dev logs instead. When in
    doubt, the kernel source (`mach3-port/src/ext/user.c`) is the
    truth. **Do not invent rows**: a number not in either table
    is not known to exist.

> **Key facts**
>
> - **Convention (ARM, EABI-like):** number in `r7`, args in `r0`–`r5`, return in `r0`, trap via `svc #0`. Errors are negative: −1 general failure, −2 ENOSYS (unknown number).
> - **User memory layout:** code at `0x100000` (embedded image, own task per program), stack page at `0x110000`, sbrk heap `0x120000`–`0x130000` (64 KB). Buffer validation is VA-range checking, not page-table walking (honest limitation, audit #7).
> - **Shape:** poll-shaped, non-blocking (R9) — consumers poll and yield; there are no blocking syscalls.
> - **Reserved:** 10–12 and 23 are held for Mach-style IPC that has never been implemented (see decision D-ARCH-4 on the [boundary page](kernel-userspace-boundary.md)).

## Table 1 — documented in SYSCALL-ABI.md

| Range | Family | Highlights |
|---|---|---|
| 20–24 | Core | `SYS_WRITE` (20), `SYS_YIELD` (21 — cooperative, waits one 100 Hz tick), `SYS_EXIT` (22), `SYS_SBRK` (24; 64 KB pre-allocated heap per task) |
| 30–34 | Files (ramfs) | `SYS_OPEN` (30), `SYS_READ` (31), `SYS_CLOSE` (32), `SYS_LS` (33), `SYS_DELETE` (34). ramfs: flat, 32 files, per-task fd table |
| 40–41 | GPIO | `SYS_GPIO_SET` / `SYS_GPIO_GET` (bank 0; RAM mock in QEMU, real registers on RV1103) |
| 50–51 | Block | `SYS_SD_READ` / `SYS_SD_WRITE` (sector I/O) |
| 52–56 | FAT32 (`/sd`) | `SYS_MKDIR`, `SYS_FAT_WRITE`, `SYS_FAT_READ`, `SYS_FAT_DELETE`, `SYS_READDIR` |
| 57–59 | Monitor | `SYS_STAT` (57 — fills `struct qaon_stat`), `SYS_TLIST` (58), `SYS_READ_CONSOLE` (59) |
| 60–68 | Display | `SYS_DISPLAY_INFO/FLUSH/GRANT/ACQUIRE/RELEASE/GET_EVENT/STATUS` (60–66), `SYS_UPTIME` (67), `SYS_DISPLAY_SLEEP` (68). 240×240, RGB565; flush is token-gated |
| 69–70 | Wall clock | `SYS_TIME_SET` / `SYS_TIME_GET` (Unix seconds UTC; `0` = never set) |
| 71–72 | UDP | `SYS_UDP_SEND` / `SYS_UDP_RECV` (≤512 B datagrams, single RX slot, non-blocking) |
| 73–77 | TCP client | `SYS_TCP_CONNECT/STATUS/SEND/RECV/CLOSE` — **one** client connection at a time; send is stop-and-wait per segment; recv returns 0 = "not yet" |
| 106–107 | Power (FND-1) | `SYS_RESET` / `SYS_POWEROFF` via PSCI; the kernel stamps reason 4 + bootlog event 12 before acting |

`struct qaon_stat` (via 57): uptime (real), memory used/total
(real, 64 MB), block totals, net counters — with two fields
honestly `QAON_UNKNOWN` (`0xFFFFFFFF`) today: `cpu_pct` (no idle
accounting) and `blk_used_sec`. There is **no battery field**
(see [Power & Battery](power-battery.md)).

Input events (via 65): console keys are translated by the kernel
— W/A/S/D arrows, Enter = OK, Esc = BACK, M = MENU, plus escape
sequences; the 16-entry queue is flushed on every token
GRANT/RELEASE.

## Table 2 — in use, documented in track docs (backfill pending)

These numbers are attested by the architecture/boundary documents
and the track dev logs cited per row — they are **not** in
SYSCALL-ABI.md yet, which is exactly the staleness the warning
above is about.

| No | Name / family | Source |
|---|---|---|
| 78 | `SYS_FACE_POLL` family — face expression/status requests | [`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) §2 row 13 |
| 79–80 | Spawn (run a one-shot, collect exit code) | FND plan / boundary inventory |
| 82–88 | Audio — ACODEC/I2S0 RX: init, capture, gain, overrun | audio track D1–D4 dev logs; ARSITEKTUR-SISTEM §3.1 |
| 89–92 | PL330 DMA (stage 1) | ARSITEKTUR-SISTEM §3.1 |
| 93–97 | littlefs (`/data`) | [`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) §2 row 10 |
| 98 | `SYS_BOOTSTATE` — subs incl. 7 stage, 9 stage-signed-package, 11 set-GUI-mode | B2/B4 plans; [Updates](updates.md) |
| 99 | `SYS_BOOTLOG` | B2 plan |
| 100–101 | `SYS_TIME_QUALITY` / `SYS_TIME_SYNC` — only the NTP path may mark time *synced* (vs merely set) | B3 plan; UTIMEGATE battery |
| 102 | `SYS_GUI_START` — launch the GUI daemon set (dev mode) | boot-UX plan |
| 103 | `SYS_WEBUI` — PIN/session/web-shell bridge control | ONB-1 dev log |
| 104 | `SYS_CRED` — credential store (sub 0 set / 1 get / 2 present / 3 clear / 9 self-test) | B5.5 dev log |
| 105 | `SYS_EVENT_INJECT` — push a button event (a0 ≤ 6); the test path, and the future GPIO path | ONB/boot-UX work |
| 108 | `SYS_POWER` — **proposed**, not allocated in code | [Power & Battery](power-battery.md) (PWR-1 design) |
| — | `SYS_RANDOM` — **proposed** with the TRNG service | SOC-2 ([Hardware & SoC](hardware-soc.md)) |

## Growing the ABI

New numbers are appended, never recycled; a new kernel-resident
surface must pass the admission rule R14 first
([boundary page](kernel-userspace-boundary.md)). When you add a
syscall, the ABI doc update belongs in the same change — the
28-row backlog exists precisely because that rule was not always
followed.

Sources:
[`docs/SYSCALL-ABI.md`](../docs/SYSCALL-ABI.md) (canonical, stale as warned),
[`docs/riset-fondasi-tanpa-board/RINGKASAN.md`](../docs/riset-fondasi-tanpa-board/RINGKASAN.md) (audit #14),
[`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) (§2 inventory).
