# DEVLOG-UI7 — editor revert + status-bar icons

Two user-ordered revisions (2026-10-26) in one branch,
editor revert first, then status-bar icons. No other
product change. Worktree `qaonic_os-ui7`, branch
`dev/ui7`, base = UI-6 merge (push 254).

## 1. Revision 1 — remove the on-device name editor

User decision: name editing is WEB CONFIG ONLY.

### 1.1 Scope (surgical, not a UI-5 revert)

Removed from `user/uiapp/lv_settings.c` (+ header):

- The whole editor: `V_EDIT` view, `EDIT_MAX` /
  `edit_charset` / `edit_buf` / `edit_len` / `edit_pos` /
  `edit_msg` state, `edit_ch_count`, `edit_ch_idx`,
  `edit_cycle`, `to_name_detail`, `edit_key`,
  `build_edit`, `edit_enter_cb`, the `V_EDIT` tick
  case + raw-key drain branch, the `V_EDIT` on-back
  branch, and the edit-state clearing in `lvset_exit`.
- The Edit button + editor entry from the Settings
  name detail. The detail goes back to value display
  + a web note: `Edit via web settings` at row 100,
  BACK at row 130, focus starts on BACK.

Kept deliberately (the user removed only the editor):

- UI-5's shell polish (focus / BACK parity across
  menu, Settings, Monitor, Power).
- UI-5's honest Memory-row guard in `lv_monitor.c`
  (SYS_STAT impossible -> `-`).
- The PIN digit-picker, timezone list, and all other
  Settings views untouched. The old hand-rolled
  `scr_textedit` screens (LLM model/key, wifi
  password) are unrelated and untouched.
- No `UILV5:` marker remains anywhere in `user/`
  (verified by grep after the cut).

### 1.2 Verify updates

- `scripts/verify-ui3.py`: the name-detail check
  goes back to the note: `Edit button rendered
  (blue)` replaced 1-for-1 by `web note rendered
  (dark)` (rows 100-124, threshold 100).
  Denominator unchanged at 78.
- `scripts/verify-ui5.py` (UI-7 revision, same file
  and `UI5-VERIFY` verdict string): editor checks
  replaced by web-note + BACK-focus + no-editor
  checks; the rename round trip now goes through
  the shell (`setup name`, same cfg-name the web
  page writes) with re-grant display proof; the
  BACK-cancel section becomes BACK-button parity;
  the 40-char bound is set via the shell (with a
  responsiveness probe first, per the UI-6 flake
  note about long bursts in the post-release
  window) and displayed in the detail; the idle
  section idles out of the detail instead of the
  editor; the final check asserts NO `UILV5:`
  marker anywhere in the serial log.
  Denominator: 81 (UI-5) -> 73 (UI-7 revision).

## 2. Revision 2 — status-bar icons

User decision: logos, not text. Inactive wifi/BT =
icon with a slash through it. Open-source icons
only, licenses recorded.

### 2.1 Why not LVGL built-in symbols

Checked first, as instructed, and rejected on
evidence, not assumption: the vendored LVGL subset
compiles button + label widgets only (131 .c files;
image / line / arc / canvas are header-only), and
the Montserrat 14 font's sparse cmap (62 entries)
contains NO glyph for `LV_SYMBOL_WIFI` (0xF1EB),
`LV_SYMBOL_BLUETOOTH` (0xF293), or any battery
symbol (0xF240/0xF244). Built-in symbols cannot
render on this port.

### 2.2 Rendering choice: port-painted 1-bit masks

`lv_port.c` composites the icons into every
flushed area overlapping the top bar, clipped to
the flushed rectangle, in `flush_cb` before
forwarding to `SYS_DISPLAY_FLUSH`. This reuses
the old hand-rolled strip's glyph shapes (`ui.c`
`icon_wifi` / `icon_bt` / battery outline) for
parity between old and LVGL screens, and matches
the face renderer's procedural approach. Cost:
point tables only, no font, no image, no new
widget. The shapes follow the canonical wifi-arcs
/ bluetooth-rune / battery-outline forms (cf.
Lucide `wifi`, `bluetooth` -- ISC license, verified
via public sources 2026-10-26; the masks here are
hand-drawn point tables in the repo's own style,
not copied SVG data). Material Symbols
(`wifi_off`, `bluetooth_disabled`, Apache 2.0) was
the alternative candidate; Lucide's slash-through
convention is what the OFF rendering follows.

Geometry (240x24 bar, right-aligned, 6 px margin,
same as the old strip): wifi 15x11 at (174, 6),
bluetooth 8x11 at (194, 6), battery 20x8 outline +
nub at (210, 8). Icon gray 0x808080 (RGB565
0x8410), the same dim gray as the text label the
icons replace. OFF = glyph + 1-px diagonal slash
(wifi: 15 steps, BT: 8 steps). Battery is
outline-only, never a fake level (no ADC).

### 2.3 State: QEMU defaults, live wiring deferred

`uilvgl_set_radio_icons(wifi_on, bt_on)` in
`lv_port.c`, called by the shell's
`build_status_bar()` with (0, 0). Painting is
enabled only by that call, so the UI-1 smoke
screen stays icon-free. Live state wiring
(`uproto` WIFI.STATUS / BLE.STATUS -- the same
calls the wifi/ble screens already make) is a
documented follow-up, NOT wired here: a blocking
RPC in the menu-build path would stall menu
appearance on linkd trouble, and QEMU has no radio
anyway, so OFF/OFF is the true state for every
verification run. The follow-up is a one-line
change at the call site once a poll/cache design
exists.

### 2.4 Verify updates (icons)

`verify-ui5.py` gains an `icons_ok()` shape check
per LVGL dump (menu, list, detail, Monitor,
Power): wifi + BT slash diagonals fully gray
(sampled every 2nd x), wifi glyph points (dot +
outer arc) gray, BT glyph points (spine + tips)
gray, battery corners gray with an empty interior.
Mid-gray = 100 < r < 200 with channel spread <
30: icon gray (~132) passes, bar bg (~222) and
clock glyphs (near-black) do not. The clock-band
dark checks are kept alongside. The slash itself
is proven visually from the dumps (coordinator
visual gate), not by counts alone.

## 3. Size

- Baseline (UI-6 tree, pre-change): kernel binary
  4,015,560 B, headroom 178,744 B.
- Post-change (revert + icons): kernel binary
  4,014,648 B, headroom 179,656 B (+912 B vs
  baseline -- the revert saved more than the icon
  code cost). uiapp.bin 442,992 B. Objcopy-binary
  method throughout, same as the UI-6 series.

## 4. Battery

Worktree battery, logs in `~/workspace/logs-ui7/`
(full log per leg, never truncated). Shared dev
disk restored to healthy / unprovisioned, PIN
246810, before and after the battery
(`fix-state.py`, one-shot serial-shell script,
not committed).

### 4.1 Test-first evidence (pre-change)

- Revised UI3 script on the OLD (UI-6) kernel:
  FAIL 77/78 -- the single failing check is the
  web-note check (old kernel has the Edit button,
  no note).
- Revised UI5 script on the OLD kernel: FAIL
  60/73 -- all 13 failures are UI-7 checks (icon
  renders x5, BACK focus, web note, OK-on-BACK,
  no-editor, back/bound detail opens expecting
  `nameback` focus, no-UILV5-markers). Everything
  else passed.
- Post-change (revert + icons): UI5-VERIFY PASS
  73/73, UI3-VERIFY PASS 78/78. All icon check
  lines True on all 5 dumps (wifi-slash, bt-slash,
  wifi-glyph, bt-glyph, batt-corners, batt-empty).
  Note dark=342 (threshold 100). Clock dark 126
  (menu/list/detail) / 116 (monitor/power).

### 4.2 Worktree battery table

| Leg | Result |
| --- | --- |
| UI5 (revised, 73) | 72/73, then reruns (see §4.3) |
| UI4 | 72/73, rerun PASS 73/73 |
| UI3 (revised, 78) | PASS 78/78 |
| UI2 | PASS 38/38 |
| UI1 | PASS 25/25 |
| FND1 | PASS |
| FND2 | PASS 21/21 |
| FND3 | 45/46, rerun PASS 46/46 |
| FND4 | PASS 36/36 |
| FND5 | PASS 33/33 |
| C6L1 | PASS 29/29 |
| C6L2 | PASS 36/36 |
| C6L3 | PASS 72/72 |
| C6L4 | PASS 60/60 |
| C6L5 | PASS 55/55 |
| IPC2 | PASS 34/34 |
| IPC4 | PASS 58/58 |
| IPC5 | PASS 20/20 |
| ONB1 | PASS |
| ONB2 | PASS |
| SHELL | PASS |
| SAFEMODE | PASS |
| No QEMU left | PASS |

Visual gate (coordinator's own eyes, 2026-10-26):
PASSED. Menu dump: clock left; right side shows
wifi arcs with a clearly readable diagonal slash,
BT rune with slash, empty battery outline + nub;
no "WiFi BT []" text; menu buttons unchanged.
Detail dump: "Device name", "AB", "Edit via web
settings" note, blue BACK button focused; no Edit
button. Protected copies (made before the battery
overwrote `logs-ui5/` dumps):
`~/workspace/logs-ui7/dumps/ui7-menu.ppm/.png`,
`ui7-detail-ab.ppm/.png`, `ui7-power.ppm/.png`.

### 4.3 Battery failures -- all diagnosed, none a UI-7 regression

Three UI5 runs after the post-change PASS, three
different single-cause signatures; every UI-7
check (icons, web note, BACK focus, no-UILV5)
passed in every run.

1. Battery run, 72/73 -- `bound: detail renders
   the long value`: the stored name was 29 A's,
   not 40. The long `setup name` burst lost 11
   chars in the post-release window (the UI-6
   mechanism: 26, then 27 A's stored from 40).
   The display path is exonerated: the detail
   rendered the stored (short) value correctly
   (dark=576). The responsiveness probe does NOT
   prevent burst loss (it proves the shell
   answers, not that a 40-char burst arrives
   intact). The UI-6 follow-up (`shell_cmd`
   should verify long arguments) is real and
   still open.
2. UI4 battery run, 72/73 -- `timezone set`:
   the serial log proves `setup tz +0700` ->
   `setup: tz saved` DID happen; the check failed
   because `shell_cmd`'s 10 s timeout expired
   under battery load, then the early-False branch
   fired (command echo present, expected output
   not yet seen). Harness timing, not product.
   Rerun PASS 73/73.
3. FND3 battery run, 45/46 -- boot-3 fail_count
   != 0, but the run's cleanup check passed (disk
   left healthy, fail_count=0): the known
   fail_count carry-over pattern (an earlier boot
   missed its healthy mark under battery serial
   load). Rerun PASS 46/46.
4. UI5 rerun, 72/73 -- `setup flow reaches
   finale`: the payload appeared in the serial
   log but NO `setup status` echo followed; all
   3 probe sends vanished in the daemon-launch
   storm after `gui`, exactly the phenomenon
   documented in `setup_flow`'s own docstring
   ("bare setup-done sends right after `gui` can
   vanish without a trace... while every later
   shell command lands fine"). Everything after
   (including ALL UI-7 checks) passed. UI-7
   touches nothing in face / sh / `gui` /
   scheduler; the post-change run passed setup
   flow on the same kernel. Timing flake, not
   regression.
5. UI5 rerun2, 69/73 -- four failures, two
   mechanisms, both harness:
   - Re-grant cascade (3 checks: `Settings
     opens`, `name detail opens`, `detail
     differs`): the serial log proves every
     screen transition happened correctly
     (settings list -> detail -> list ->
     release). The script's sequential waits
     with fresh positions missed markers that
     had already arrived in the reader buffer
     during grant_menu recovery grants, and the
     continue-on-fail flow then desynchronized
     (an OK sent on the already-open detail
     acted as BACK, so the "differs" dump was
     taken of the settings list, not the
     detail -- out of band on the high side).
     Same single-chunk-arrival disease as the
     UI-5 run-1 idle/release lesson, now in
     the re-grant path. Harness pos-race, not
     product.
   - `bound: shell reads back the 40-char
     prefix`: the burst-loss mechanism of (1)
     again (second occurrence in four runs).
   Teardown completed clean (PIN untouched, no
   QEMU left). A fix-state run preceded rerun3.
6. UI5 rerun3, 72/73 -- `bound: shell reads back
   the 40-char prefix`: the burst-loss mechanism
   of (1) a third time (battery, rerun2, rerun3).
   Three occurrences in five runs made blind
   rerunning pointless, so the harness was fixed
   instead of the run being repeated: the bound
   set in `verify-ui5.py` now goes through a new
   test-only `shell_cmd_chunked()` (8-char chunks,
   0.3 s pauses, one line, newline last). Chunking
   only paces delivery -- the stored-value check
   still validates the full 40 chars, so a bad
   delivery fails loudly. No product code touched.
   This closes the UI-6 `shell_cmd` long-argument
   follow-up for this script (verification, not
   delivery pacing, was the UI-6 proposal; pacing
   is what the mechanism actually needed).
7. UI5 rerun4 (with the chunked bound set): PASS
   73/73. All bound checks pass, including the
   full 40-char read-back. Battery green: every
   leg PASS standalone or on rerun (UI4 rerun,
   FND3 rerun, UI5 rerun4), no UI-7 check failed
   in any run, no QEMU left running, shared disk
   left healthy / unprovisioned with PIN 246810.

## 5. Honest limitations / follow-ups

- Live radio state wiring for the icons (see §2.3).
- The UI-6 flake follow-ups stand: `shell_cmd`
  should verify long arguments (UI5 bound burst
  loss window); `pin_entry` needs per-key
  picker-marker verification (loss downstream of
  sh, not PL011 input loss).
- The kernel VM accounting root (negative
  `vm_page_free_count`, DEVLOG-UI5 §7) is still
  open; the Monitor guard stands.
- The UI-6 strip proposal (1.86 MB) is untouched
  by this phase (separate decision).
