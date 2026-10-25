# DEVLOG-UI2 — LVGL shell / status bar / menu

Phase: UI-2 (second phase of the UI track).
Branch: `dev/ui2` (worktree `~/workspace/qaonic_os-ui2`).
Base: UI-1 merge (`a3c17bd`).
Status: (filled at the end: DONE / verified / merged by coordinator).

Spec: `docs/riset-ui-framework/RINGKASAN.md` §7 (UI-2 =
shell + status bar + menu, proof screen) as refined by
the UI-2 brief (menu entries wired to existing screens
where they exist; Settings/Monitor/Power as honest
placeholders for UI-3/UI-4).

## 0. Gate: headroom baseline reconciliation (done FIRST)

### The discrepancy

UI-1's dev log reports trimmed headroom **200,116 B**
(objcopy-binary method, worktree build). The merged
main-tree build, independently re-measured by the
coordinator with the same method
(`llvm-objcopy-18 -O binary` on `mach3.elf` vs the
4,194,304 B boot partition), is **4,064,196 B**,
i.e. headroom **130,108 B**. Same sources, same
method, **70,008 B** apart.

### Finding

The UI-1 worktree figure was correct; the main-tree
figure was a dirty-build artifact. Evidence, all
measured by UI-2:

- Fresh worktree build of the UI-1 merge sources:
  binary **3,994,188 B**, headroom **200,116 B** —
  exactly UI-1's figure, reproduced.
- Main-tree binary, re-measured by UI-2:
  **4,064,196 B** — the coordinator's figure, confirmed.
- Object comparison (`build/*.o` and `build/obj-md/*.o`,
  main vs fresh worktree): the main build dir holds
  exactly 9 extra `_img.o` files, all retired IPC test
  programs — `uipccli`, `uipcdclean`, `uipcdfault`,
  `uipcdrain`, `uipcdsetup`, `uipcdwedge`, `uipcsrv`,
  `uipkcli`, `uipksrv` — plus their stale `build/*.o`
  and `build/gen/*_img.c` byproducts.
- Mechanism: the final link globs the whole obj-md
  dir (`MD_OBJS=$(find "$OBJMD" -name '*.o' | sort)`),
  while Phase C compiles only an explicit program list
  and never cleans obj-md. Retired programs' `_img.o`
  files therefore stay linked forever. The stale
  symbols (`uipccli_img`, `uipcsrv_img`, `uipkcli_img`)
  are present in the main `mach3.elf` and absent from
  the fresh worktree `mach3.elf` — the link, not the
  sources, is the difference.
- Sizes: the 9 stale `_img.o` files sum to 76,552 B
  on disk (ELF headers/symtab included); their linked
  image payloads + alignment account for the 70,008 B
  binary delta. The same stale-object class bit
  DEVLOG-IPC4's headroom figure (880,888 B, not
  reproduced at IPC-5) — this is the third time, so
  the fix below is structural, not a cleanup.

### Fix (UI-2 commit 1, small + proven)

`mach3-port/build-md.sh`, before Phase C:

```sh
rm -f "$OBJMD"/*_img.o
```

Phase C rebuilds every listed image right after, so
this only deletes outputs about to be regenerated —
or that no longer exist. Proven by planting the 9
stale objects in the worktree obj-md and rebuilding:
binary back to **3,994,188 B**, stale symbols absent.
No other change in that commit.

### True baseline (objcopy-binary method, throughout)

| Metric | Value |
|---|---|
| UI-1 kernel binary (clean) | 3,994,188 B |
| UI-1 headroom | **200,116 B** |
| Main-tree dirty binary (artifact, retired) | 4,064,196 B |
| Main-tree dirty headroom (artifact, retired) | 130,108 B |

The coordinator's next main-tree build (with the
purge) will land on the clean figure; the 130,108 B
number must not be quoted again.

### Related note (same class, no instance today)

`MI_OBJS` is also a `find` glob over `obj/`; a
retired MI source would linger the same way. No
retired MI file exists today, so no fix is bundled
here — but the next track that retires a kernel
source should extend the purge or move the link to
an explicit list. Stale `build/gen/*_img.c` and
per-program `build/*.o` files are harmless litter
(Phase C compiles only the explicit list; program
links use explicit paths) and are left alone.

## 1. What was built

### 1.1 Port lifecycle refactor (`user/uiapp/lv_port.c/h`)

UI-1's display + keypad indev + focus group setup
was inline in the smoke run. UI-2 extracts it:

- `uilvgl_shell_begin()` — lv_init once, then a
  fresh display (240x24 RGB565 partial strip),
  keypad indev, and focus group per run; returns
  the group. Also (re)initialises the BACK edge
  flag, the last-input timestamp, and the flush
  failure count.
- `uilvgl_shell_end()` — deletes indev, then group,
  then display, in that order (indev first: it
  holds the group pointer). The smoke run previously
  leaked one indev per grant; it now uses begin/end
  too, and UI1-VERIFY still passes (see §5).
- `uilvgl_take_back()` — consumes one recorded BACK
  press (the keypad callback records BACK as an
  edge, not a level). Whoever owns the current
  screen decides what BACK means: the smoke screen
  exits on it (unchanged); the shell goes back a
  level or releases.
- `uilvgl_last_input_ms()` — last keypad press, for
  the 30 s idle rule.
- `uilvgl_flush_failures()` — failed-flush count,
  reported by the shell at exit, never fatal.

### 1.2 Shell (`user/uiapp/lv_shell.c/h`)

One screen object, rebuilt on every navigation step
(menu <-> placeholder), so no screen leaks across
switches. The focus group is emptied before every
rebuild (removing grouped objects is not something
to rely on when deleting them).

- Status bar on the top layer (`lv_layer_top()`):
  a flat light-gray 240x20 bar, live clock label
  left (HH:MM, wall clock WIB when set, else
  uptime — same rule as `ui.c` fmt_clock, logic
  duplicated, not shared, so the old UI is
  untouched), and one dim right label
  `WiFi BT [    ]`. The radio icons stay dim and
  the battery stays an outline: no radio state
  exists under QEMU and no ADC level exists (the
  v4 honest-state rule). A 1 s LVGL timer refreshes
  the clock; the timer is deleted before teardown
  (timers outlive displays, so deleting the
  display first would leave a dangling fire).
- Menu: 8 buttons, 200x24, from y=24 (below the
  bar) at 26 px pitch — 24 + 8*26 = 232, inside
  240 with no scrolling. Theme padding shrunk
  (2 px vertical) so the 14 px font is not clipped
  in a 24 px button. Focus starts on Qabot; DOWN
  = NEXT, UP = PREV (the port's keypad mapping);
  focus and activation print `UILV2:` markers.
- Menu entries, in order: Qabot (release), Wi-Fi,
  Bluetooth, LLM, Passkey (wired to the existing
  hand-rolled screens), Settings, Monitor, Power
  (honest placeholders).
- Placeholder screens: the entry name, `Coming
  soon`, and the phase promise (`UI-3`/`UI-4`) as
  its own line, plus a BACK button. BACK (key or
  button) returns to the menu; the menu rebuild
  refocuses Qabot.
- Loop: tick + timer handler + yield every turn
  (watchdog-safe by construction, same as the
  smoke loop); BACK handled contextually via
  take_back(); 30 s idle or token loss releases.

### 1.3 Main loop (`user/uiapp/main.c`)

Per token grant: the UI-1 smoke gate still runs
first (marker `/data/ui1-test` = "1"), so
UI1-VERIFY is unaffected. Otherwise the shell
runs; a wired action runs the corresponding old
screen via `run_old_screen()` — the old 33 ms
loop, extracted verbatim — until it pops itself
(empty stack, depth 0) and control returns to the
shell, or until release/idle/token loss releases
the token. The old menu and settings-list screens
are unreachable from the shell (superseded by it);
the leaf screens stay reachable until UI-3
rebuilds them, and are deleted in UI-6 per the
plan. Daemon order untouched; linkd untouched
(61,564 B, 93.9% of its cap — same as UI-1).

### 1.4 Navigation contract (UI-2, stated plainly)

- Shell menu -> wired leaf screen -> BACK -> shell
  menu -> BACK at root -> release to face.
- Shell menu -> placeholder -> BACK -> shell menu.
- Idle 30 s anywhere in the shell or an old screen
  -> release to face (the old auto-return rule).
- Old screens keep their own BACK semantics while
  active (pop to the shell only when their stack
  empties; a leaf that requests release still
  releases — none of the four wired leaves does).

## 2. Measurements (objcopy-binary method)

| Metric | Value |
|---|---|
| UI-2 kernel binary | 3,997,528 B |
| UI-2 headroom | **196,776 B** |
| UI-1 baseline (reconciled, §0) | 200,116 B |
| UI-2 cost | **3,340 B** |
| uiapp.bin UI-2 / UI-1 | 428,104 B / 424,764 B |

The whole UI-2 delta sits in uiapp (+3,340 B,
exactly the binary delta) — shell code + strings
only, no new LVGL widgets. (An interim figure of
3,997,360 B / 196,944 B was measured on the build
before the flush-count decimal print change; the
table above is the final build.)

## 2.1 A measurement footnote (build-path `__FILE__`)

UI-1 already noted a 64 B `__FILE__` build-path
artifact between trees. UI-2's figures are all
from the same worktree build, so they are
self-consistent; cross-tree comparisons should
still state the method, as §0 does.

## 3. Trajectory restated

With the CORRECTED baseline, UI-1's projection
stands unchanged: UI-2 costs 3,340 B (not the
70 KB the dirty baseline implied), the trajectory
still crosses zero around UI-4/UI-5 on current
trends, and the structural decision is due before
the crossing phase — not in UI-2. The strip
proposal below is restated, not bundled.

### Strip proposal (coordinator decision, NOT done here)

UI-1 measured the test-program family at 350,883 B
(ulink/uipc test programs, padded `.bin` sizes) and
the `utlscli`/`utlskeep` pair at 1,511,448 B with
no verify-script reference and no spawn hook
(usage audit still owed for the pair). Stripping
the family alone restores headroom to ~551 KB
after UI-1, covering the whole UI-2..UI-6
trajectory on current trends. UI-2 does NOT strip
anything: the decision, the QA-image story for the
stripped programs, and the rebuild + battery proof
belong to the coordinator, with the corrected
baseline in hand.

## 4. Verification

### UI2-VERIFY: PASS 38/38 (post-change)

`scripts/verify-ui2.py` boots, provisions
(`setup reset` → `gui` → QR → `setup done` →
finale), then exercises the shell end to end:

- Menu dump: light background (white 11,251 px vs
  the dark menu's 0), status-bar clock text in the
  top band, first button blue 4,721 px, last
  button blue 4,647 px.
- Navigation: 7× DOWN walks focus through all 8
  entries (qabot → wifi → ble → llm → passkey →
  settings → monitor → power), 2× UP walks back.
- Placeholder: OK on Settings → placeholder screen
  (BACK blue 7,067 px, frame diff 35,785 vs menu);
  BACK → menu rebuilt, focus Qabot again.
- Old-screen round-trip: DOWN → Wi-Fi, OK →
  hand-rolled Wi-Fi screen (dump dark, white 0);
  two BACKs → shell restarts (see the note quirk
  below), menu dump light again.
- Release: BACK at menu root → `UILV2: release` +
  token released to face; fresh grant restarts
  the shell; 30 s silence → `UILV2: idle exit` +
  token released.
- Cleanliness: 0 kernel FAULT, 0 uiapp FAIL,
  0 LVGL flush failures, no QEMU left running.

### Pre-change evidence: FAIL 16/38 on the UI-1 kernel

Same script, `KERNEL=<main-tree UI-1 mach3.elf>`,
`UI2_TSCALE=0.2`: no `UILV2:` markers, old dark
menu (white 0), every navigation / placeholder /
old-screen / release / idle check fails. The 16
passes are boot/flow/screendump mechanics and the
dark-menu consistency checks — the script is
discriminating, not vacuous.

### First post-change run: 31/38, all 7 FAILs diagnosed

The first post-change run failed 7 checks; every
one traced to the script, not the device, and the
script was fixed (device code untouched):

1. White threshold 20,000 (copied from UI-1's
   near-full-white smoke screen) is wrong for an
   8-blue-button menu: actual white is 11,251 px
   vs the dark menu's 0. Bar lowered to 5,000
   (3 checks: menu dumps 1/3/5).
2. Marker ordering: the menu rebuild's focus print
   comes BEFORE the "back menu" print in the
   stream, so a fresh wait window opened after
   "back menu" can never see it. Both markers are
   now checked in the segment captured before the
   key is sent (1 check).
3. The Wi-Fi screen's note quirk: on entry it
   shows a no-peer error note (no C6 peer in this
   run), and its handler eats the first keypress
   of any kind to dismiss the note — so the first
   BACK dismisses the note and only the second
   BACK pops. The script now sends two BACKs
   (1 check, plus the two checks it had derailed:
   menu-root release and its token print).
4. The idle-release check opened a fresh window
   after "idle exit" and missed the token print
   the reader thread had already appended. It now
   uses the pre-grant segment (1 check).

Reran after the fixes: **PASS 38/38**. The device
behaved correctly in all four cases; the fixes are
documented here so the next track does not
re-learn them.

### UI1-VERIFY regression: PASS 25/25

The smoke screen refactored onto begin/end still
passes fully, including the marker clear/readback
and the same 3,997,528 B / 196,776 B size line.

## 5. Battery

All scripts run standalone in the worktree (own disk
copies, not the shared main-tree disks). Battery order
first, then every non-pass rerun standalone with full
output, per the standing rule.

| Script | Battery order | Standalone |
|---|---|---|
| UI2-VERIFY (new) | — | PASS 38/38 |
| UI1-VERIFY | — | PASS 25/25 |
| C6L1 | PASS 29/29 | — |
| C6L2 | PASS 36/36 | — |
| C6L3 | PASS 72/72 | — |
| C6L4 | no verdict (crash) | PASS 60/60 |
| C6L5 | PASS 55/55 | — |
| IPC2 | PASS 34/34 | — |
| IPC4 | PASS 58/58 | — |
| IPC5 | PASS 20/20 | — |
| FND1 | FAIL (2 checks) | FAIL, same 2 checks |
| FND2 | FAIL 20/21 | FAIL, same check |
| FND3 | FAIL 44/46 | FAIL, same 2 checks |
| FND4 | PASS 36/36 | — |
| FND5 | PASS 33/33 | — |
| ONB1 | PASS | — |
| ONB2 | PASS | — |
| SHELL | PASS | — |
| FIRSTBOOT | FAIL | PASS |
| SAFEMODE | PASS | — |
| UPDATE | PASS | — |

### Resolved in standalone (not regressions)

- **C6L4**: the script crashes at startup listing
  `logs-c6l4/`, which does not exist in a fresh
  worktree (the known harness-precondition flake
  from UI-1's battery). After `mkdir logs-c6l4`:
  **PASS 60/60**. Proposal (coordinator, not
  bundled): `os.makedirs(..., exist_ok=True)` in
  the script.
- **FIRSTBOOT**: fails on a provisioned disk
  ("first-boot banner missing", "setup status
  should be NOT provisioned") — the known
  provisioned-disk precondition. After one boot
  running `setup reset` (confirmed `setup: NOT
  provisioned`): **PASS**. The script's own boot 3
  leaves the disk unprovisioned again.

### Reproduced in standalone: stale assertions, not regressions

Five checks fail identically standalone, and every
one is a stale assertion against the old menu /
old uiapp loop that UI-2 replaced by design. The
underlying behaviors all still pass; only the
pixel/marker expectations are stale. **No UI-2
code change is proposed for these** — the fixes
belong to the verify scripts, and they are the
coordinator's call (see proposals).

- **FND1 Phase C (2 checks)**: the script drives
  the OLD menu (`key menu`, `key down`, `key right`
  → POWER, `key ok`) and checks the old power
  screen's row bands. Under UI-2 those keys walk
  the LVGL shell menu instead, so the dump is the
  shell menu: bands [0, 94, 0, 121]
  (SHUTDOWN/RESTART/SLEEP/BACK) — SHUTDOWN and
  SLEEP NOW empty, hence the 2 failures. The
  FND1 syscalls are untouched: Phase D (shell
  `poweroff` → QEMU exits 0 + kernel log) passes,
  14/16 checks pass. **There is no UI path to
  scr_power in UI-2** (Power is a placeholder
  per the brief), so Phase C cannot be fixed
  until UI-4 rebuilds Power as an LVGL screen —
  or the coordinator wires the old scr_power
  behind the shell's Power entry (a brief
  deviation, but it would keep FND1 green; the
  wifi/ble/llm/passkey wiring sets the
  precedent that a real old screen behind a
  shell entry is not fake functionality).
- **FND2 last check**: after re-grant, the script
  counts old-menu selected-tile fill pixels
  (C_SEL ~24,45,74) and requires > 800. The
  re-granted screen is now the LVGL menu, so the
  count is **0**. Every reclamation behavior
  passes (fault isolation, reclaim logging, face
  eyes, re-grant + re-acquire). Proposal: count
  LVGL default-theme button blue instead of
  C_SEL — a one-assertion update, applicable
  now.
- **FND3 section B (2 checks)**: (a) "display
  token came home" greps for `died holding
  display` or the old `uiapp: idle -> release`.
  The serial log proves the token came home
  safely via the shell's idle path instead:
  `UILV2: idle exit` → `[dpy] RELEASE -> face`
  → `uiapp: token released`, followed by
  re-grant + re-acquire (both PASS). The
  watchdog behavior itself is intact (kill 2,
  exit 53405, no fault). Proposal: accept
  `UILV2: idle exit` as the idle-path marker.
  (b) "menu renders again after re-grant" is the
  same C_SEL pixel staleness as FND2 (count 0);
  same proposal.

### Net battery verdict

Every track's behavior passes except the five
stale assertions above, which fail deterministically
because the old menu is gone. UI-2 does not
regress any kernel, IPC, link, provisioning, or
supervisor behavior. The worktree disk is left
healthy and unprovisioned (FIRSTBOOT's boot 3
state); no QEMU left running.

## 6. Honest limitations

- Status-bar radio icons are dim by design (no
  radio state under QEMU); wiring linkd STATUS
  into the bar is later work, not a UI-2 gap
  papered over.
- The clock shows uptime-derived time until NTP
  syncs, same as the old status bar.
- SPI pacing is unverifiable in QEMU (as UI-1).
- Placeholder screens are labels, not
  functionality — by design, and labelled as such
  on screen.
- Old leaf screens still render hand-rolled and
  still call linkd (bridge without a peer fails
  bounded, as in C6L-4/5); peer-backed leaf
  functionality remains covered by C6L-5's verify,
  not re-proven here.
- UI-3..UI-6 are NOT built here.
- Battery consequence (see §5): FND1 Phase C,
  FND2's last check, and FND3 section B's two
  checks fail on stale old-menu assertions until
  their verify scripts are updated (FND1-C has no
  UI target until UI-4). This is the known,
  intended cost of replacing the menu, not a
  silent regression — the underlying behaviors
  all pass.
