# Boot & Resilience

QaonicOS's resilience story has three independent layers, each
assuming the one below it can fail: daemons die alone and are
reclaimed; the boot layer counts failures across power cycles and
falls back to a recovery shell; the update layer reverts any
unconfirmed trial by itself. This page summarizes the mechanisms;
the acceptance bar is at the end.

> **Key facts**
>
> - **Boot chain (hardware):** MaskROM (unbrickable) → idbloader at sector 64 → U-Boot → kernel. Flashing: hold BOOT + USB-C → MaskROM mode → `upgrade_tool`.
> - **Boot state** lives in a raw sector at the device tail (second-to-last block sector; the last is selftest scratch) — outside any filesystem.
> - **fail_count ≥ 3 → safe mode** (shell only); one healthy boot clears it.
> - **Healthy mark is earned**: the boot one-shot battery must pass before `fail_count` resets and a trial update confirms.
> - **Soak-verified (2026-10-20):** 100/100 cold boots, 30/30 SIGKILL power cuts.
> - **FND-3 supervisor is in progress** — the runtime half of this page is being built now.

## Bootstate and the bootlog

`bootstate.c` owns two persistent structures at the tail of the
block device:

| Structure | Content |
|---|---|
| `struct bs_state` (magic `0x424f4f54` "BOOT") | `boot_count`, `fail_count`, `last_reason`, flags (bit0 provisioned, bit1 sabotage — suppresses exactly one healthy mark, bit2 GUI/user mode), update fields (`cur_version`, `active_slot`, `upd_state`, `pending_*`) |
| Bootlog ring | 16 records × 512 B (`struct bs_rec`, magic `0x424c4f47`, FNV-1a checksum), events 1–12: boot, trial, rollback, safe mode, … event 12 = software reset (FND-1) |

Boot reasons: 0 unknown · 1 power · 2 crash · 3 watchdog ·
4 software. The reserved `BSR_WATCHDOG=3` is what the hardware
watchdog's two-stage timeout will stamp on hardware.

Access is syscall 98 (`SYS_BOOTSTATE`, sub-operations including
stage/confirm/GUI-mode) and 99 (`SYS_BOOTLOG`); the shell exposes
`bootctl` and `bootlog`.

## Safe mode

A boot that never earns its healthy mark leaves `fail_count`
standing. At three consecutive failures the next boot enters
**safe mode**: only `sh` runs, with a banner and a bootlog
SAFEMODE entry. Safe mode is a *recovery surface*, not a dead end
— the shell can inspect state, and a single healthy boot clears
the counter. The dev-mode disk is shared state in QEMU testing:
killed boots increment `fail_count`, so verify batteries can
occasionally trip safe mode and recover with clean boots — a
documented test-environment behaviour, not a product bug.

## FND-1: real reset and poweroff

Before FND-1, the Power screen's "Shutdown" was theatre (black
screen + an infinite yield loop). Now:

- Syscalls **106 `SYS_RESET`** / **107 `SYS_POWEROFF`**; the kernel
  records reason 4 + bootlog event 12 **before** the platform acts.
- QEMU conduit is **PSCI over HVC** (proven from the DTB `method`
  property; `psci.c` carries a TODO(HW) seam for the RV1103
  PMU/WDT register path).
- Userspace: `sys_reset()`/`sys_poweroff()` in ulib, `reboot` /
  `poweroff` shell builtins, and the uiapp Power screen performs
  real poweroff/reset (yield-forever remains only as the
  platform-refusal fallback).
- Verified: FND1-VERIFY **PASS 16/16** (genuine second boot,
  `boot_count` increments, reason stamped with the requesting boot).

## FND-2: resource reclamation on daemon death (built)

Fault isolation already worked — a faulting daemon dies alone.
What was missing: everything it *held* stayed held forever, so a
dead uiapp kept the display token and the device needed a power
cycle. FND-2 adds one reclaim path, `daemon_reclaim(idx)` in
`user.c`, called from the single funnel both death routes share
(clean `SYS_EXIT` via `sched_exit_switch`, and `user_fault` which
forwards into it). No new syscall.

| Released on death | Detail |
|---|---|
| Display token `dpy_holder` | uiapp death → back to face + event queue cleared; face death → sleep request + pending face request cleared |
| Console owner `console_owner` | back to −1 (the boot default) |
| Web-shell bridge | settled by `webui_shell_executor_died()`: a queued command is dropped; a taken-but-unfinished command completes into the result state with a definitive "executor exited" error, so the web side never hangs |
| Face request mailbox | dropped if the *poster* dies (new `face_req_owner`) or if the face dies |
| TCP client connection | closed with a graceful FIN if the dying daemon opened it (new `tcp_client_owner`) |
| Deliberately **not** reclaimed | the sleep-request flag on holder death (the Sleep Now flow sets it just before release — losing it would lose the user's request); daemon Mach tasks/pmaps (slots are fixed; respawn is FND-3's design space); GPIO pin levels (outputs must not glitch because a daemon died); kernel services (not daemon-owned) |

Verified the test-first way: `scripts/verify-fnd2.py` fails on the
pre-fix tree (wedged device) and passes after the fix. Dev log:
[`docs/rencana-fondasi-tanpa-board/DEVLOG-FND2.md`](../docs/rencana-fondasi-tanpa-board/DEVLOG-FND2.md).

## FND-3: watchdog / health supervisor (in progress)

!!! warning "Status: in progress"
    FND-3 is the current implementation phase. This section
    describes the approved design in the FND plan, not shipped
    behaviour.

Design (from
[`docs/rencana-fondasi-tanpa-board/PLAN.md`](../docs/rencana-fondasi-tanpa-board/PLAN.md) §FND-3):

- **Software supervisor in kernel**: per-daemon liveness deadlines
  derived from each daemon's role (GUI daemons poll events at frame
  cadence; `sh` blocks on input, so its deadline is generous /
  input-aware). Liveness is proven by normal syscall/yield activity
  — no new heartbeat API.
- **Escalation ladder:** (1) log a runtime event → (2) kill +
  reclaim via the FND-2 path, respawning where the launcher can →
  (3) if the hang repeats or the daemon is not respawnable, mark
  the crash through bootstate (fail_count, reason 3 watchdog) and
  `SYS_RESET` — the existing 3-strikes safe mode then applies at
  the next boot.
- **Trust requirements:** fixed table, integer math, runs from the
  timer/yield path, no allocation — the supervisor itself must be
  simple enough to trust.
- **On hardware**, the supervisor also feeds the NS hardware
  watchdog (`dw-wdt` @ `0xff5a0000`, two-stage), which covers the
  case software cannot: the kernel itself wedged. Timeout values in
  seconds still need the WDT clock rate from the CRU table.
- **Verification:** a hang-injection one-shot (a daemon that spins
  without yielding) must be detected and recovered, with the
  watchdog event visible in bootlog/bootstate; a healthy system
  must run the full regression battery with zero false positives.

Why this is needed at all: daemon scheduling is cooperative and
`net_pump()` runs only from the two scheduler switch points — one
daemon that stops yielding freezes the GUI *and* the network,
undetected, until FND-3.

## The out-of-box acceptance bar

From [`docs/riset-out-of-box/RINGKASAN.md`](../docs/riset-out-of-box/RINGKASAN.md) —
the bar the owner set (2026-10-20) and the yardstick for all of the above:

- 100/100 cold boots unattended.
- 100 random power cuts → filesystem intact.
- A failed update → the previous version boots.
- 3 consecutive crashes → recovery mode, never a reset-loop brick.
- First boot offline → the device tells the user what to do.

Sources:
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md) (§4, §6.1),
[`docs/rencana-fondasi-tanpa-board/PLAN.md`](../docs/rencana-fondasi-tanpa-board/PLAN.md),
[`docs/rencana-fondasi-tanpa-board/DEVLOG-FND1.md`](../docs/rencana-fondasi-tanpa-board/DEVLOG-FND1.md),
[`docs/rencana-fondasi-tanpa-board/DEVLOG-FND2.md`](../docs/rencana-fondasi-tanpa-board/DEVLOG-FND2.md),
[`docs/rencana-resilience/PLAN-B2.md`](../docs/rencana-resilience/PLAN-B2.md),
[`docs/riset-out-of-box/RINGKASAN.md`](../docs/riset-out-of-box/RINGKASAN.md).
