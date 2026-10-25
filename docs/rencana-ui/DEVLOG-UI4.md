# DEVLOG-UI4 — LVGL Monitor + Power screens (UI-4)

Branch `dev/ui4`. UI-4 replaces the shell's Monitor
placeholder with a real LVGL Monitor screen and the
Power entry's wiring to the old hand-rolled
scr_power screen (a UI-2 coordinator follow-up)
with a real LVGL Power screen. It also carries the
small follow-up from UI-3's gap list: device clocks
honor cfg-tz. Monitor stays honest-state; Power
keeps scr_power's semantics; no strip is bundled
(coordinator decision, as before).

## 1. What was built

### Monitor screen (`user/uiapp/lv_monitor.c/h`)

Driven by the shell loop like the Settings module
(`lvmon_enter` / `lvmon_tick` / `lvmon_on_back` /
`lvmon_active` / `lvmon_exit`). One title, nine
label rows, one BACK button; rows refresh on a
500 ms LVGL timer (the plan sketch's chart/arc
gauges are NOT used: UI-1 vendored only button +
label widgets, so gauges would need new vendored
subsystems for no information gain — labels carry
the same values).

Every row is real data (honest-state rule):

| Row | Source |
|---|---|
| Device | cfg-name, same file the web settings page and shell `setup` use ("-" when unreadable) |
| Uptime | SYS_STAT uptime_ms, seconds |
| Memory | SYS_STAT mem used/total KB |
| Threads | SYS_STAT nthreads |
| Net | SYS_STAT rx/tx KB |
| Disk | SYS_STAT blk used/total sectors as MB (sec/2048) |
| CPU | "-" always: the kernel reports QAON_UNKNOWN (no idle accounting yet) |
| Boot | bootstate 98 sub 0: boot #, last reason (0 unknown 1 power 2 crash 3 watchdog 4 software), fail_count |
| Time | wall clock HH:MM in the configured timezone + NTP sync state (1 synced, else unsynced); uptime clock when no wall time is set |

A failed SYS_STAT shows "STAT FAIL" instead of
rows; a failed bootstate shows "-" for Boot.
Row texts are printed once on entry
(`UILV4: mon ...`) so verify scripts can assert
exact values without OCR. The refresh timer is
deleted on every monitor exit path (BACK button,
BACK key, teardown): a review catch during
development — without it the timer would keep
writing to labels `build_menu` just deleted.

`sys_bootinfo` is promoted to ulib
(`user/ulib/ulib.c/h`, syscall 98 sub 0, same
r0/r1 convention as the shell's `sh_bootstate`
wrapper) instead of duplicating raw asm in the
screen. It is named `sys_bootinfo`, not
`sys_bootstate`, because `ufirst.c` already owns
a static `sys_bootstate(sub)` with a different
signature — the first UI-4 build failed on that
collision, and the rename fixed it.

### Power screen (`user/uiapp/lv_power.c/h`)

Same actions, same order, same semantics as
scr_power: SHUTDOWN / RESTART / SLEEP NOW / BACK.
Focus starts on SHUTDOWN, matching the old
screen's sel = 0; focus markers (`UILV4: focus
shutdown/restart/sleep/back`) mirror the shell
menu's so navigation is observable. Shutdown and
Restart act immediately on OK (SYS_POWEROFF /
SYS_RESET, never returning on success) — the old
screen asked no confirmation, so neither does
this one. Sleep Now blanks the display via
SYS_DISPLAY_SLEEP(1) and then releases the token,
exactly like the old screen. One deliberate
deviation: if a power syscall ever returns
(platform refused), the screen shows a "refused"
status and stays usable instead of the old
screen's yield-forever fallback (that path never
triggers on QEMU/PSCI).

### Shell rewiring (`user/uiapp/lv_shell.c/h`, `main.c`)

Monitor and Power become KIND_MONITOR /
KIND_POWER entries driven by the shell loop
exactly like Settings. The SHELL_POWER dispatch
in main.c and its define are removed (nothing
references them anymore). With no placeholder
entries left, the placeholder machinery
(build_placeholder, back_btn_cb, VIEW_PLACEHOLDER,
KIND_PLACEHOLDER, the `coming` field) is removed
rather than left dead.

### Clocks honor cfg-tz (follow-up, own section)

New helper `uilvgl_tz_offset_min()` in
`lv_port.c`: parses "+HHMM"/"-HHMM" from cfg-tz,
WIB (+420) on missing or malformed values
(wrong length, non-digit, HH > 14, MM > 59).
Both fmt_clock copies (LVGL `lv_shell.c`, old
`ui.c`) use it; the old copy gains an
`lv_port.h` include. The read is cached: the
cache is invalidated on any cfg-tz write through
`uilvgl_lfs_write` (the Settings screen's path)
and expires after 5 s otherwise (shell `setup tz`
writes around the cache, from another program),
so a tz change moves the clocks within 5 s.
The face shows no clock (verified by grep: no
time use in `user/face/`), so no face change
was needed — the task's "face" touch point is
vacuous, and the dev log says so rather than
inventing one.

## 2. Size (objcopy-binary method, same as UI-1/UI-2/UI-3)

| | kernel binary | headroom | uiapp.bin |
|---|---|---|---|
| UI-3 baseline (fresh worktree) | 4,005,424 | 188,880 | 435,824 |
| UI-4 | TBD | TBD | TBD |
| UI-4 cost | TBD | TBD | TBD |

Trajectory: TBD (restated after the post-change
measure). Linkd untouched (93.9% cap). No strip
bundled (coordinator decision).

## 3. Verification

### UI4-VERIFY (new, `scripts/verify-ui4.py`)

- Pre-change (UI-3 kernel): **FAIL TBD/TBD**.
  No `UILV4` string exists anywhere in the base
  tree, so every marker wait fails by
  construction; the passing checks are harness
  checks (boot, shell setup, no FAULT, size
  evidence) plus the +0700 old-clock check (the
  UI-3 clock hardcodes WIB, so it matches at
  +0700 by coincidence — the discriminating
  old-path check is the +0800 one).
- Post-change: **PASS TBD/TBD** -- menu, Monitor
  rows (Device TestDev, CPU "-", Boot #, Time),
  Monitor BACK button, Power four blue buttons,
  Power BACK key, tz +0800 LVGL + old-path
  (glyph-exact 5x7 template match), malformed tz
  -> WIB fallback, +0700 restore with setup
  read-back, Shutdown (QEMU exits 0), Restart
  (SYS_RESET + new prompt), Sleep (release +
  changed dump). 0 FAULT, 0 uiapp FAIL,
  0 flush failures, no QEMU left.
  uiapp.bin=TBD, kernel binary=TBD,
  headroom=TBD.

The script never changes the shared PIN (UI-3
lesson): teardown restores tz +0700 and name
qaon and unprovisions; PIN 246810 untouched
throughout. Full output is kept per leg (no tail
truncation, UI-3 lesson).

### FND1 Phase C update (same branch)

FND1's Phase C drove the old scr_power screen
(bands at y=58/88/118/148). UI-4 replaces it,
so the section now asserts the LVGL Power
screen: `UILV4: power` marker + four button-blue
bands (48-72, 74-98, 100-124, 126-150). Same
class of change as the UI-2 coordinator
follow-up and the UI-3 UI2-VERIFY update: the
product changed, so the gate follows it.

## 4. Battery (worktree; full logs per leg)

TBD — table filled after the battery run. Known
flake classes applied as discriminators (not
hidden): FND1 ev=12 battery-order flake (rerun
standalone), C6L5 serial-under-load misses
(rerun standalone), one-shot SD/fat timing
(rerun standalone), ONB2's exit code (read the
verdict line from the full log).

## 5. Commits (branch `dev/ui4`, no merge, no push)

- `5975abc` -- ui4: verify script (FAILs
  pre-change) + FND1 Phase C follows LVGL Power
  screen
- TBD -- ui4: LVGL Monitor + Power screens +
  tz-clock follow-up
- dev log: this file (committed on `dev/ui4`;
  final hash in the coordinator's merge report)

## 6. What UI-4 deliberately does NOT do

- No text editing (UI-5): Monitor values are
  read-only; device name editing still waits.
- No chart/arc gauges (labels carry the values;
  see §1).
- No strip of test programs (coordinator call).
- No linkd, face, daemon-order, or wire change.
- No confirmation dialog on Power (scr_power
  parity, see §1).
