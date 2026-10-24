# Plan: Board-less Foundations Track (FND)

Date: 2026-10-23. Owner decision (2026-10-23): implement BOTH
open tracks, **foundations first, C6 link second**. This plan
covers the foundations track; the C6 link track follows it
under `docs/rencana-komunikasi-c6/PLAN.md` (C6L-1..C6L-5).

Scope = the top-5 of the board-less foundation audit
(`docs/riset-fondasi-tanpa-board/RINGKASAN.md`). Audit items
6–14 are listed as backlog at the end — in scope for later
owner approval, not for this track. Hardware-blocked and
owner-parked items (secure boot, LLM API key) stay out, per
the audit appendix.

## Design principles

- Every phase ships with: implementation, a QEMU-runnable
  verification (one-shot or script in the style of
  `scripts/verify-onb1.py`), the regression battery green
  (SHELL-VERIFY, SAFEMODE, FIRSTBOOT, UPDATE-VERIFY,
  ONB1 19-check, ONB2, plus the ULFS / UVOICE / UCREDSTORE
  one-shots), a dev log in this directory, and a commit.
- Kernel changes stay inside the Mach 3 extension layer
  (`mach3-port/src/ext/`), never in the Mach core.
- New syscalls take the next free numbers and are added to
  `docs/SYSCALL-ABI.md` in the same commit (the doc is
  already 28 syscalls stale — audit #14; keeping it current
  from here is a rule of this track, the backfill itself is
  backlog).

## Phases

### FND-1 — Reset / poweroff syscall (audit #2)

Goal: software can reboot and power off the device.

Scope:
- Kernel: `SYS_RESET` and `SYS_POWEROFF` (next free syscall
  numbers). QEMU implementation via PSCI `SYSTEM_RESET` /
  `SYSTEM_OFF` (SMC/HVC conduit as the virt machine
  provides); the RV1103 mapping (PMU/WDT) is a documented
  TODO(HW) seam in the same function.
- Record the reset reason in bootstate before resetting
  (reuse `last_reason`: 1 power, 2 crash, 3 watchdog; add
  4 = software request) so the next boot's bootlog tells
  the truth.
- Userspace: ulib wrappers; shell builtins `reboot` and
  `poweroff`; uiapp Power screen gains the Restart item
  the D-GUI spec promises, and Shutdown calls the real
  syscall instead of the black-screen yield loop.

Verification: QEMU script — at the shell, `reboot` produces
a full second boot on the same disk with `boot_count`
incremented and reason 4 in the bootlog; `poweroff`
terminates QEMU. Power screen still renders (GUI capture).

Non-goals: sleep/wake (power FSM exists), hardware PMU
mapping.

### FND-2 — Resource reclamation on daemon death (audit #3)

Goal: a daemon that exits or faults hands back everything
it held; no field failure of the form "screen/console dead
until power cycle".

Scope: in `sched_exit_switch` / the `user_fault` path, if
the dying daemon holds any of these, release it:
- display token (`dpy_holder`) — face must be able to
  render again;
- console ownership (`console_owner`) — sh death must not
  wedge the console;
- pending face request / expression state owned by the
  dead daemon;
- event queue entries are already global; note any
  per-daemon state found during implementation in the
  dev log.

Verification: one-shot or shell-driven test that kills the
display holder (fault injection) and proves the face
re-renders and the menu can be granted again; console
equivalent for sh. Regression battery green.

Non-goals: daemon restart policy (that is FND-3's
escalation decision), supervisor-grade isolation.

### FND-3 — Watchdog / health supervisor (audit #1)

Goal: a hung daemon is detected and the device recovers by
itself; the out-of-box promise "never a reset-loop brick"
extends from boot time to runtime.

Scope:
- Kernel supervisor: per-daemon progress heartbeat
  (a daemon proves liveness by its normal syscall/yield
  activity; qabotd/face/uiapp/sh each get a deadline
  derived from their role — GUI daemons are expected to
  poll events at FRAME_MS cadence, sh blocks on input so
  its deadline is generous / input-aware).
- Escalation ladder: (1) log a runtime event; (2) kill and
  reclaim the hung daemon (FND-2 path) — for daemons the
  launcher can respawn, respawn via the existing daemon
  launch machinery; (3) if the hang repeats or the dead
  daemon is not respawnable, mark the crash through the
  bootstate path (fail_count, reason 3 watchdog) and
  `SYS_RESET` (FND-1) — the existing 3-strikes safe mode
  then applies at the next boot.
- The supervisor itself must be simple enough to trust:
  fixed table, integer math, runs from the timer/yield
  path, no allocation.

Verification: hang-injection one-shot (a daemon that
spins without yielding) — supervisor detects, recovers,
bootlog/bootstate show the watchdog event; a second test
proves a healthy system runs the full regression battery
with zero false positives; soak test still PASS.

Non-goals: preemptive scheduling changes; per-thread
watchdogs inside a daemon.

### FND-4 — Persistent littlefs backend in QEMU (audit #4)

Goal: `/data` survives reboots in emulation, so the
provisioning / credential / onboarding flows can be
regression-tested across boots without the board.

Scope:
- `lfs_port.c` gains a persistent backend over a reserved
  region of the blk device (region layout mirrors the HW
  draft's littlefs split in `draft/sfc_nand.c`: blocks
  320..999 of the NAND map to a sector range on
  `mach3-blk.img` that does not collide with the bootstate
  state sector / bootlog ring at the device tail or the
  selftest scratch — the layout must be written down in
  the dev log and checked against `bootstate.c`).
- Backend selection: persistent by default for normal
  boots; the RAM backend (with its power-cut simulator)
  stays selectable for the B1/B2 tests that depend on it.
- NAND semantics preserved (erase -> 0xFF, program-only
  clearing bits) so littlefs sees the same rules as on
  hardware.

Verification: write config + a credential, reboot, read
back identical (scripted, two boots, same disk); ULFS
still PASS on both backends; soak test still PASS;
ONB1/ONB2 still PASS — now with persistence assertions
across the reboot added where the scripts already reboot.

Non-goals: the real SFC/NAND driver (draft exists,
hardware phase); wear levelling beyond littlefs itself.

### FND-5 — Update-package delivery path (audit #5)

Goal: a real package file produced by
`scripts/sign-update.py` can enter the B4/B5.3 pipeline
end to end, board-less.

Scope:
- Shell: `update stagepkg <path>` reads a package file
  from `/sd` (FAT) or `/data` (littlefs, once FND-4
  persists it) in chunks into the kernel package buffer
  and calls the existing verify+stage path (syscall 98
  sub 9 semantics: bad signature -> -3, downgrade -> -2).
  The 8 KB QEMU package cap stays.
- The compile-time embedded package in `uupdsig` remains
  as a unit test; `stagepkg` is the product path.

Verification: build a package with `sign-update.py`,
place it on the SD image, stage it from the shell, reboot
into the trial slot, confirm (healthy mark) — the
UPDATE-VERIFY flow extended with the real file; negative
tests: tampered package -> -3 and no stage, older
version -> -2.

Non-goals: network delivery of updates (that rides the
C6 link / webui later), raising the 8 KB cap (a flash
layout question for the hardware phase).

## Backlog (audit #6–#14 — not this track)

6. Factory reset (OS-side wipe; wants FND-4) — first
   candidate after this track.
7. copyin/copyout page-table validation.
8. Process model v2 (multi-spawn, argv, spawn from /data).
9. Persistent runtime logging beyond boot events.
10. SYS_STAT unknowns (cpu_pct, blk_used_sec).
11. CI on the working branch + automated verify battery.
12. Userspace IPC (reserved syscalls 10–12, 23).
13. TLS entropy strengthening.
14. SYSCALL-ABI.md backfill (28 syscalls).

## Phase map

FND-1 -> FND-2 -> FND-3 (one recovery chain, in this
order), then FND-4, then FND-5. After FND-5: the C6L
track (C6L-1..C6L-5) per
`docs/rencana-komunikasi-c6/PLAN.md`. After C6L: the UI
track (UI-1..UI-6, LVGL inside uiapp) per
`docs/riset-ui-framework/RINGKASAN.md` — owner decision
2026-10-23 (LVGL adopted; Qt/QML rejected; UI-1 is a
measure-first go/no-go against the re-measured
boot-partition headroom — 932,724 B at IPC-5, not the
retracted ~287 KB figure, which counted bss).
