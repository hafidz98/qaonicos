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
| UI-4 (final, incl. face fix) | 4,013,364 | 180,940 | 441,708 |
| UI-4 cost | +7,940 | -7,940 | +5,884 |

Trajectory: UI-1 200,116 -> UI-2 ~196,776 ->
UI-3 188,880 -> UI-4 180,940 (objcopy-binary
method throughout). The pre-verify estimate was
180,956; the face race fix (below) added 16 B.
Linkd untouched (93.9% cap). No strip bundled
(coordinator decision).

## 3. Verification

### UI4-VERIFY (new, `scripts/verify-ui4.py`)

- Pre-change (UI-3 kernel): **FAIL 38/74** (36
  FAILs, full log `logs-ui4/ui4-prechange.log`).
  No `UILV4` string exists anywhere in the base
  tree, so every marker wait fails by
  construction; the passing checks are harness
  checks (boot, shell setup, no FAULT, size
  evidence) plus the +0700 old-clock check (the
  UI-3 clock hardcodes WIB, so it matches at
  +0700 by coincidence — the discriminating
  old-path check is the +0800 one).
- Post-change: **PASS 73/73** (run 8, full log
  `logs-ui4/ui4-postchange8.log`, EXIT=0) -- menu,
  Monitor rows (Device TestDev, CPU "-", Boot #,
  Time), Monitor BACK button, Power four blue
  buttons, Power BACK key, tz +0800 LVGL +
  old-path (glyph-exact 5x7 template match:
  75/75 first try at +0800, 61/61 at +0700),
  malformed tz -> WIB fallback, +0700 restore
  with setup read-back, Shutdown (QEMU exits 0),
  Restart (SYS_RESET + kernel banner), Sleep
  (release + black dump). 0 FAULT, 0 uiapp FAIL,
  0 flush failures, no QEMU left.
  uiapp.bin=441,708, kernel binary=4,013,364,
  headroom=180,940.

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

Full logs: `~/workspace/logs-ui4/battery-<leg>.log`
(in-chain) and `~/workspace/logs-ui4/rerun-<leg>.log`
(standalone reruns). UI4 itself (73/73) is not a
battery leg; it is §3. The battery was killed
externally mid-FND2 (VM replacement, ~01:29 UTC)
and resumed from FND2 after a fix-state run (PIN
246810 + unprovisioned); completed legs' results
stand. In-chain failures were discriminated by
standalone rerun, never hidden.

| Leg | In-chain | Standalone | Failing checks / cause |
| --- | --- | --- | --- |
| UI1 | PASS 25/25 | — | — |
| UI2 | PASS 38/38 | — | — |
| UI3 | FAIL 50/76 | FAIL 50/76 (same count) | First grant flakes in both runs (single-attempt `key menu`, no injection verification — the PL011 byte-loss disease UI4's script was hardened against); both re-grants PASS, so the grant product path is healthy. UI3-script harness weakness, NOT a UI-4 regression; flagged for the coordinator's harness cleanup, not fixed on this branch. |
| C6L1 | PASS 29/29 | — | — |
| C6L2 | PASS 36/36 | — | — |
| C6L3 | PASS 72/72 | — | — |
| C6L4 | PASS 60/60 | — | — |
| C6L5 | FAIL 54/55 | PASS 55/55 | In-chain: `ulink5d ok markers continue after client death` — known serial-under-load flake class. |
| IPC2 | FAIL 28/34 | PASS 34/34 | In-chain: web login + web shell + ev=17 checks — PIN cascade (UI3's leg leaves PIN 1234; IPC2 logs in with 246810). UI3's script forces 1234 at start and restores 1234, never 246810; its teardown only does `setup reset`. Fix-state (restore 246810) + rerun = green. |
| IPC4 | FAIL 36/58 | PASS 58/58 | In-chain: all 22 failures cascade from web login (PIN 1234 left by UI3); the tools server runs via web shell, so registration + all client checks fell with it. |
| IPC5 | PASS 20/20 | — | — |
| FND1 | PASS | — | LVGL Power Phase C gate works on the UI-4 tree. |
| FND2 | PASS 21/21 (resume) | — | Resume leg passed after fix-state; confirms the PIN theory. |
| FND3 | FAIL 35/45 (resume) | PASS 46/46 | In-chain root: `boot 3 is NOT safe mode (self-cleared)` — fail_count carried over from the VM-killed boots; all 9 other failures were `[blocked: no finale]` cascade. Clean state + rerun = green. |
| FND4 | PASS 36/36 | — | — |
| FND5 | PASS 33/33 | — | — |
| ONB1 | FAIL (3 checks, resume) | PASS | In-chain: `POST /login wrong PIN rejected`, `correct PIN -> 303`, `throttle: 4 more wrong PINs still just rejected` — order-sensitive state (ran after FND5 without a PIN reset); clean preconditions + rerun = green. |
| ONB2 | FAIL (2 checks, resume) | PASS | In-chain: `finale shows the eyes again` (0 eye pixels), `finale log line` — lag/state; rerun finale shows 4460 eye pixels. Face finale path healthy (UI3's rerun also reaches finale). |
| SHELL | FAIL (resume) | PASS | In-chain: single sub-check `ls` missing `/sd:` — one-shot SD/fat timing flake class. |
| SAFEMODE | FAIL (resume) | PASS | In-chain: boot3 fail_count=1 (safe-mode boot did not self-clear under carried-over state); rerun boot3 fail_count=0. |
| UPDATE | PASS | — | — |

Final shared-disk state after all runs + a closing
fix-state: PIN 246810, unprovisioned, healthy
(fail_count 0), no `qemu-system-arm` left. Battery
artifacts that landed in the repo worktree
(`logs/*.ppm`, `logs/*.png`, `logs/*-serial.log`
from legs that dump into repo `logs/`) were left
untracked, never committed; a tracked
`logs/ipc4-serial.log` overwritten by the IPC4 leg
was restored with `git checkout` before commit.

## 5. Face race fix (product bug, found by UI-4 verify)

Face's loop checks the display holder at the
top of the iteration, grants mid-iteration on
EV_MENU, then renders at the bottom -- and the
render flush fails (no longer holder) and
exits(1). Death is intermittent per grant (a
frame must be due in the grant iteration), so
early grants pass and later grants on the same
boot fail with no face left to grant -- exactly
the run-7 pattern (face FAIL + exit on all four
boots, grants dying partway through). Fix
(`user/face/main.c`, +16 B): re-check
`sys_display_status() == 0` immediately before
the render flush and skip the frame instead;
uiapp paints over it anyway. Run 8: 14 grants,
0 face FAILs. This is a robustness fix, not a
face redesign: face stays hand-rolled, LVGL
stays in uiapp only. No respawn was observed
for face (FND-3 respawn does not cover it) --
a follow-up worth filing, not fixed here.

## 6. Harness lessons (verify script only, no device change)

- Guest wall-time lag: responses arrive 20-30 s
  real-time (menu idle is 30 guest-s ~= 37 s
  real). Needle waits went 20 s -> 32 s (below
  the real-time idle limit, above the lag).
- PL011 RX byte-loss under load (known C6L
  family): `key menu` -> `ey menu` seen twice.
  Every key send now verifies `key: injected`
  and resends once.
- Dev-mode boots are shell-first: daemons
  launch only on `gui`. Action phases must run
  `gui` (wait for `face: Qabot server
  starting`) before any grant -- a grant
  without gui can never arrive.
- A relaunch must close a still-alive QEMU
  first, or the new QEMU dies on hostfwd 18080
  (run-2 crash: `conn(SER)` timeout).
- Old screens idle out on their own timer
  printing `uiapp: token released`, never
  `UILV2: release`: the Wi-Fi BACK-out accepts
  both (release-first priority).
- Restart's new-boot check waits for the kernel
  banner, not `qaon>` (any prompt matched --
  run-7 false positive).
- Reader marker timestamps (`ui4-marks.log`)
  adjudicated wait-expired-vs-device-slow
  disputes during diagnosis.

## 7. Commits (branch `dev/ui4`, no merge, no push)

- `5975abc` -- ui4: verify script (FAILs
  pre-change) + FND1 Phase C follows LVGL Power
  screen
- `310a200` -- ui4: fix verify flow (shell state
  after old_clock_check; skip setup on action
  boots)
- `b4581ab` -- ui4: LVGL Monitor + Power screens
  + tz-clock follow-up
- `350c067` -- ui4: fix verify harness races
  (stolen console bytes theory -- later
  disproven, see §6; stale monitor row; qemu
  reap)
- `e4dbe87` -- ui4: fix verify harness (gui
  before action grants, key resend, probe
  pattern, restart close)
- `d0e873a` -- ui4: verify waits 20s->32s for
  guest lag; fix restart/sleep pos-before-ok
- `abdb47b` -- ui4: verify grant recovery +
  wait_either wifi back-out
- `05294ff` -- ui4: verify back-out accepts
  old-screen idle release
- `1178dc3` -- ui4: face must not die on
  post-grant flush; verify banner check + sleep
  dump wait
- dev log: this file (committed on `dev/ui4`;
  final hash in the coordinator's merge report)

## 8. What UI-4 deliberately does NOT do

- No text editing (UI-5): Monitor values are
  read-only; device name editing still waits.
- No chart/arc gauges (labels carry the values;
  see §1).
- No strip of test programs (coordinator call).
- No linkd, daemon-order, or wire change. One
  face robustness fix (§5) -- no face redesign.
- No confirmation dialog on Power (scr_power
  parity, see §1).
