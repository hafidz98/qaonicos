# DEVLOG-UI3 — LVGL Settings screen (UI-3)

Branch `dev/ui3`. UI-3 replaces the shell's Settings
placeholder with a real LVGL submenu: Device name,
Timezone, PIN. Monitor stays an honest UI-4
placeholder; Power stays wired to the existing
hand-rolled power screen (UI-2 coordinator follow-up).

## 1. What was built

### Settings submenu (`user/uiapp/lv_settings.c/h`)

The shell loop (`lv_shell.c`) gains a `KIND_SETTINGS`
entry. Button callbacks only mark state; the loop's
`lvset_tick()` rebuilds views, exactly like the
placeholder rule from UI-2. Views, all rebuilt on
the single screen object:

- **List**: Device name / Timezone / PIN / BACK
  (menu-style 200x24 buttons, group focus).
- **Device name detail**: current `cfg-name` value +
  an honest "Edit in UI-5" note. Free text cannot
  be edited properly with keypad-only input yet, so
  UI-3 shows the value and defers editing rather
  than faking it.
- **Timezone detail**: title `Timezone: <current>`
  plus six option buttons (`+0000 UTC`, `+0700 WIB`,
  `+0800 WITA`, `+0900 WIT`, `+0600`, `+1000`).
  Focus starts on the stored value (first option
  when the stored value is not in the list). OK
  writes `cfg-tz` and rebuilds the detail so the
  title shows the new value.
- **PIN detail**: `PIN: Set` / `PIN: Not set`
  (syscall 103 sub 3) plus Change/Set PIN and BACK.
- **PIN picker** (raw-key mode, see below): length
  select (4..16, UP/DOWN, OK confirms), then digit
  entry (UP/DOWN cycles the digit, LEFT/RIGHT moves
  the cursor, OK advances, OK on the last position
  confirms, BACK cancels). Digits are masked
  (`*`, cursor `_`) with a `POS c/len DIGIT d`
  line so the picker stays usable. Flow: old PIN
  (when set) -> verify -> new PIN -> confirm ->
  set. Wrong old PIN reports "Wrong PIN", a locked
  PIN reports "PIN locked", a mismatch returns to
  the new entry. Only numeric PINs can change on
  the device; anything else still needs the web UI
  (stated on screen in the dev log, not hidden).

### Source of truth

Device name and timezone read/write the same
littlefs files the web settings page and the shell
`setup` command use (`cfg-name`, `cfg-tz`), through
new port helpers `uilvgl_lfs_read` /
`uilvgl_lfs_write` (syscalls 96/97; the UI-1 static
`lv_lfs_read` was promoted, not duplicated). The
web form limits (name 40, tz 16 chars) still bound
what the device can display; the device writes
only curated tz values (5 chars). The SD-card NVS
(`user/cfg/cfg.c`) is unrelated app state
(BLE/WiFi/KV) and was not touched.

### PIN through the kernel, not around it

- Syscall 103 sub 2 (set) and sub 3 (present)
  already existed and are reused as-is.
- **New: sub 4 = verify** (`mach3-port/src/ext/
  user.c`): a thin dispatch line calling
  `webui_pin_verify` with the same 4..16 bounds as
  sub 2. The tri-state return is preserved
  (1 ok, 0 wrong, -1 locked) so the UI can tell a
  wrong PIN from a lockout. The SHA-256 hash, the
  constant-time compare, and the 5-fail throttle
  (30 s doubling, 900 s cap) therefore apply
  exactly as they do for the web login. No new
  PIN logic exists anywhere in userspace.
- `docs/SYSCALL-ABI.md` needs no change: its
  table lists `SYS_WEBUI` by number only (subs
  are not enumerated there).

### Raw-key mode (`lv_port.c/h`)

The group model cannot express digit cycling:
UP/DOWN are group navigation (PREV/NEXT), not
data. So the port gains `uilvgl_raw_keys()` /
`uilvgl_take_key()`: while raw mode is on, the
keypad indev reports nothing and drains nothing,
and the picker drains raw EV codes itself. BACK
in raw mode is a plain key for the picker to
consume as cancel -- it deliberately does NOT set
the exit edge, so the shell loop cannot
double-handle it. Raw mode is turned on when a
picker view builds and off on every picker exit
path plus `lvset_exit()` plus `uilvgl_shell_begin`
(defensive reset per grant). Raw keys update the
idle timestamp, so picker activity resets the
30 s idle exit like any other input.

### Known gaps (honest, not hidden)

- **Device clock ignores `cfg-tz`.** Both
  `fmt_clock` implementations (old `ui.c`, LVGL
  `lv_shell.c`) hardcode WIB (+0700) when NTP time
  is available. Changing the timezone persists
  correctly and reads back, but the displayed
  clock does not move yet. Honoring `cfg-tz` in
  the clocks is a small follow-up (parse `+HHMM`,
  fall back to WIB on malformed values); it was
  left out of UI-3 because it changes face and
  old-screen clock behavior and deserves its own
  verify, not a silent ride-along.
- **Device name editing is UI-5.** Free-text
  entry needs the textarea/keyboard (or a
  char-picker) that UI-5 owns.
- **Non-numeric PINs cannot change on device.**
  The digit-picker only enters 0-9; a PIN with
  other characters must change via the web UI.
  The verify path still accepts any stored PIN
  for comparison -- only entry is digits-only.

## 2. Size (objcopy-binary method, same as UI-1/UI-2)

| | kernel binary | headroom | uiapp.bin |
|---|---|---|---|
| UI-2 baseline (fresh worktree) | 3,997,552 | 196,752 | 428,128 |
| UI-3 | 4,005,424 | 188,880 | 435,824 |
| UI-3 cost | +7,872 | -7,872 | +7,696 |

Trajectory: 188,880 B remain. At UI-2/UI-3 rates
(3.3 KB / 7.9 KB per phase), the remaining phases
(UI-4/5/6) fit with wide margin; the UI-1
projection (crossing zero ~UI-4/5) is no longer
the pacing risk it looked like before the
baseline correction, and no strip is needed for
UI-3. The strip proposal (350,883 B test family
+ 1.5 MB utls pair) stays a coordinator decision,
not bundled here. Linkd untouched (93.9% cap).

## 3. Verification

### UI3-VERIFY (new, `scripts/verify-ui3.py`)

- Pre-change (UI-2 kernel): **FAIL 24/76**.
  No `UILV3` string exists anywhere in the base
  tree (verified by grep), so every marker wait
  fails by construction; the 24 passes are harness
  checks (boot, shell setup, no FAULT, size
  evidence) plus two spurious band checks (a
  BACK-blue band and a title-dark band that happen
  to exist on the placeholder screen). The verdict
  is decisive regardless.
- Post-change: **PASS 76/76, first run** -- menu,
  settings-list walk, name detail (value + UI-5
  note), tz change +0700 -> +0800 with shell
  `setup` read-back and re-grant focus proof, and
  the full PIN round trip: old 1234 verifies, new
  5678 set + confirmed, wrong 1234 reports wrong
  (exactly one fail, no lockout), old 5678
  verifies (digit assembly proof), 1234 restored.
  0 FAULT, 0 uiapp FAIL, 0 flush failures, no
  QEMU left. uiapp.bin=435,824, kernel
  binary=4,005,424, headroom=188,880.

The script drives: known state via the shell
(`setup name TestDev`, `setup tz +0700`, `setup
pin 1234`); menu -> settings list walk; name
detail (value + UI-5 note); tz change +0700 ->
+0800 with shell-`setup` read-back and re-grant
focus proof; PIN round trip 1234 -> 5678 ->
wrong-1234 (exactly one fail) -> 5678 verifies ->
1234 restored (digit assembly proof). Teardown
leaves the disk unprovisioned; no QEMU left.

### UI2-VERIFY update (same branch)

UI-2's script asserted the Settings placeholder
(`UILV2: placeholder settings`, BACK band
160-204). UI-3 intentionally replaces it, so the
section now asserts the settings list
(`UILV3: settings list`, BACK band 102-126,
`UILV3: back menu`). Same class of change as
the FND script updates in UI-2's coordinator
follow-up: the product changed, so the gate
follows it. Result: **PASS 38/38** (battery chain,
with the updated Settings section: settings list,
BACK band 102-126, `UILV3: back menu`).
Note the pre-change UI2-VERIFY run is not
meaningful on the UI-3 kernel for the Settings
section alone (the rest of UI2-VERIFY still
applies: menu walk, Wi-Fi round trip, release,
idle exit).

## 4. Battery (worktree; chain + standalone reruns)

### Chain run (before the precondition fix)

PASS, in order: FIRSTBOOT, UI1 25/25, UI2 38/38
(updated Settings section), C6L1 29/29, C6L2 36/36,
C6L3 72/72, C6L4 60/60, FND4 36/36, FND5 33/33,
ONB1, SHELL, SAFEMODE, UPDATE, no QEMU left.

FAIL / UNKNOWN in the chain, all diagnosed below
(no failure hidden, none called a regression
without a standalone rerun):

- C6L5 53/55 (2 checks, names lost to `tail -1`
  chain output).
- IPC2, IPC4 (boot 2 only), FND2, FND3: login
  cascades -- every check downstream of the web
  login failed together.
- IPC5 19/20 (1 check, name lost to `tail -1`).
- FND1: single check `bootlog has ev=12
  'software reset' for the requesting boot` -- the
  known battery-order flake (failed once before
  when UI2+UI1 ran first in the same chain).
- ONB2: verdict UNKNOWN (its verdict line was
  buried under a Python nanobind warning in the
  `tail -2` chain output; ONB2's exit code does
  not reflect its verdict -- known harness defect,
  not fixed here).

### Root cause: shared-disk precondition, not UI-3 code

UI3-VERIFY leaves PIN 1234 on the shared dev disk
(it restores its own test PIN; there is no lfs
unlink syscall to delete a PIN). The FND/IPC/ONB
web-login scripts all log in with the shared PIN
246810 and assert 303 + session cookie, so with
PIN 1234 present every login failed, no session
was created, and every downstream web-shell check
failed with it. IPC4's ~18 min chain time is the
same cause: every boot-2 wait (up to 180 s) timed
out in turn. UI-3 changed nothing in the
http/webui/login paths (kernel sub 4 is purely
additive).

A second contamination rode along: IPC4's teardown
(marker back to 0) needs the web shell, so it
failed too, leaving the uipcld marker
`/data/iptoolsc-test` at 1 (setting it to 1 via the
serial shell had succeeded; only the reset needs
the web session).

Remediation was a one-shot serial-shell script
(scratch only, `~/workspace/logs-ui3/fix-state.py`,
NOT committed -- it boots the worktree kernel and,
pre-gui with no session needed, runs `setup pin
246810`, `put /data/iptoolsc-test 0`, and reads
both back). Its first version failed with "no
prompt" because its serial reader thread returned
on `socket.timeout` while the socket had a 0.3 s
timeout -- the reader died on the first idle
period. Fixed to continue on timeout (matching the
verify scripts' reader pattern); the fixed script
reports FIX-STATE: PASS. It was re-run once more
after the reruns below, leaving the shared disk in
the known-good battery preconditions (PIN 246810,
marker 0) and no QEMU behind.

### Standalone reruns (full output, after the fix)

| Leg | Result |
|---|---|
| IPC2 | PASS 34/34 |
| IPC4 | FAIL 57/58, then PASS 58/58 on rerun (see flake note) |
| FND2 | PASS 21/21 |
| FND3 | PASS 46/46 |
| C6L5 | PASS 55/55 |
| IPC5 | PASS 20/20 |
| FND1 | PASS (ev=12 flake confirmed again) |
| ONB2 | PASS (verdict line read from full log, exit code ignored) |

Every chain failure except IPC4's single check
resolved to a PASS once the PIN/marker
preconditions were restored -- including FND3's
unmarked A/C/D readings from the chain, which
were all login cascade after all. C6L5's 53/55
and IPC5's 19/20 likewise pass standalone; the
reset uipcld marker is a plausible contributor to
IPC5's chain miss, unproven.

### IPC4 57/58 flake note (diagnosed, then cleared)

The single failing check was `boot 1: shell
responsive at the end (fail_count=0)`. The serial
log shows why: boot 1's one-shot launch failed
with `ufs FAIL: fat_read/verify` and `ufs FAIL:
eksekusi perintah` (`user_launch_init: 2 FAILs` --
missing `/fat.out` and `/.ufs_done`), so the
healthy mark was skipped and `fail_count` stayed
1. That is the documented one-shot SD/fat timing
flake class, not a UI-3 regression: boot 2 and
boot 3 in the same run passed (boot 2's one-shots
passed, healthy mark applied, teardown marker
back to 0), and a straight rerun passed 58/58.
No code changed for this; the rerun is the
discriminator, per battery practice.

### Battery-ordering lesson for later phases

Any verify script that changes the shared PIN
must restore 246810 in its teardown, or the
battery must run a PIN-reset step between that
script and the web-login legs -- same class of
lesson as FIRSTBOOT's provisioned-disk gap and
C6L4's `logs-c6l4/` fresh-worktree gap (both also
hit in this phase: the worktree needed the
`disk-images/` symlink and a created `logs-c6l4/`
before battery runs). Chain output must keep full
logs per leg, not `tail -1`: the C6L5/IPC5 check
names were unrecoverable from the chain run and
had to be re-derived from standalone reruns.

## 5. Commits (branch `dev/ui3`, no merge, no push)

- `79e39f0` -- ui3: verify script (FAILs pre-change)
  + UI2 script follows Settings list
- `ddf4384` -- ui3: LVGL Settings screen
  (name/tz/PIN) + SYS_WEBUI PIN verify sub
- dev log: this file (committed on `dev/ui3`;
  final hash in the coordinator's merge report)

## 6. What UI-3 deliberately does NOT do

- No Monitor/Power LVGL screens (UI-4).
- No text editing (UI-5): name editing, and any
  generalization of the picker, wait for it.
- No clock behavior change (see gaps).
- No strip of test programs (coordinator call).
- No linkd, face, daemon-order, or wire change.
