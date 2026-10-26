# DEVLOG-UI5 — LVGL text editor + polish (UI-5)

Branch `dev/ui5`. UI-5 replaces the Settings name
detail's honest "Edit in UI-5" note (UI-3) with a
real keypad-driven text editor, and polishes focus,
BACK, idle, and status-bar behavior across all LVGL
screens. No new features beyond the editor + polish;
no screen redesign; no strip (coordinator decision,
as before); linkd untouched (93.9% cap).

## 1. What was built

### Editor design choice: char-picker, not textarea+keyboard

The brief allowed either an LVGL textarea +
button-matrix keyboard or a char-picker. The
char-picker was chosen, for three reasons:

1. **It reuses proven code.** The old hand-rolled
   `scr_textedit.c` picker already solved
   keypad-only text entry (charset, cycling,
   cursor, delete/append, OK-save, BACK-cancel),
   and the UI-3 PIN picker already proved the
   LVGL raw-key pattern for cycling input. The
   UI-5 editor is that picker re-hosted in LVGL:
   same 44-char charset
   (`" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!.,:-?"`),
   same key semantics (UP/DOWN cycles the
   character at the cursor, LEFT/RIGHT moves it,
   LEFT at position 0 deletes the last character,
   RIGHT at the end appends `'A'`, OK saves,
   BACK cancels).
2. **It costs no new vendored widgets.** UI-1
   vendored button + label only; textarea needs
   the textarea widget and a keyboard needs
   buttonmatrix + keyboard, each with its own
   `.text` and its own keypad-group interaction
   model to validate. The picker needs none of
   that: labels + the existing raw-key mode.
3. **It fits headroom.** The whole phase costs
   2,156 B (see §2); a keyboard widget family
   would cost an order of magnitude more for a
   worse keypad experience (the research note
   itself warned a full keyboard driven by 4-way
   keys is slow).

The one deliberate deviation from the old picker:
the old picker confirmed on OK from any position
with no save step; the LVGL editor's OK *writes*
`cfg-name` through `uilvgl_lfs_write` (the same
port helper the Settings screen already uses) and
returns to the detail, which re-reads the file --
so the detail always shows the stored value, never
a stale buffer.

### Editor details (`user/uiapp/lv_settings.c`)

- New view `V_EDIT`, driven by the shell loop
  exactly like the picker views: the button
  callback only marks state, `lvset_tick()`
  rebuilds, raw-key mode owns the EV queue while
  editing (`uilvgl_raw_keys(1)` on build, off on
  every exit path plus `lvset_exit()` plus the
  shell's per-grant defensive reset).
- Entry: the name detail gains an "Edit name"
  button (focus starts on it, matching the PIN
  detail's Change/Set focus); the "Edit in UI-5"
  note line is gone. Focus marker
  `UILV3: focus editname`.
- Display: title, value line (wrapping label, so
  a 40-char name renders on two lines), a
  `POS c/len CHAR x` line (`_` in append
  position, mirroring the PIN picker's `POS c/len
  DIGIT d` line -- no pixel cursor is needed),
  hint lines, and a message line for
  save-failure/empty feedback.
- Cursor starts at the end (like the old picker:
  `te_pos = slen(buf)`). The stored value is
  clamped to the cap on entry.
- Markers: `UILV5: edit name` (every rebuild, so
  each keypress is observable), `UILV5: edit
  saved <value>`, `UILV5: edit cancel`.
- Empty buffer cannot be saved: OK on an empty
  buffer shows "Empty - not saved" and stays in
  the editor. This avoids the len-0 lfs-write
  edge entirely; clearing a name stays a
  web/shell operation (documented limitation,
  not a silent behavior).
- Cap: `EDIT_MAX` = 40, the web form's bound
  (`form_get(..., 40u)`). One nuance, documented
  rather than hidden: the web form's `form_get`
  stores at most 39 chars (`o + 1 < max`), and
  the shell `setup` display reads with a 40-byte
  buffer (39 chars), so a 40-char name set on
  the device is stored intact and shown in full
  on-device (Settings detail reads 47, Monitor
  reads 47) but shows truncated in those two
  legacy display fields. That is a pre-existing
  display limit, not data loss, and the device
  editor is no worse than `setup name` here (the
  shell write path has no truncation either).

### Polish fixes (all covered by UI5-VERIFY checks)

- **P1 — Monitor BACK focus marker.**
  `lv_monitor.c`: the Monitor BACK button was the
  only LVGL button without a focus callback, so
  focus on it was unobservable. It now prints
  `UILV4: focus monback` like the shell menu and
  Power buttons.
- **P2 — BACK-button / BACK-key marker parity.**
  `lv_shell.c`: the list BACK button (Settings),
  Monitor BACK button, and Power BACK button
  returned to the menu silently, while the BACK
  key printed `UILV3: back menu` /
  `UILV4: back menu`. The tick-exit branches now
  print the same marker as the key branches, so
  the same user action is observable the same
  way regardless of path.
- **Verified consistent, not changed:** status
  bar (top layer, 1 s clock timer) renders on
  every LVGL screen (menu, settings list, name
  detail, editor, Monitor, Power -- each asserted
  by a clock-band dark-pixel check); idle exit
  (30 s guest) fires from the editor and releases
  (raw keys reset the idle timer, like any other
  input); BACK walks one level up everywhere
  (detail -> list -> menu -> release) and the
  editor's BACK cancels to the detail. The old
  leaf screens keep their own full-frame
  rendering (the status bar is an LVGL top-layer
  object and does not persist under them) --
  pre-existing behavior, out of scope.

### What UI-5 deliberately does NOT do

- No textarea/keyboard widgets (see §1).
- No Monitor/Power redesign (UI-4 screens stand;
  only the P1/P2 markers changed).
- No strip of test programs (coordinator call).
- No linkd, face, daemon-order, or wire change.
- No PIN behavior change (UI-5 never calls the
  PIN syscalls; the battery's PIN guard proves
  246810 survives the phase).

## 2. Size (objcopy-binary method, same as UI-1..UI-4)

| | kernel binary | headroom | uiapp.bin |
|---|---|---|---|
| UI-4 baseline (fresh worktree) | 4,013,364 | 180,940 | 441,708 |
| UI-5 (editor + polish) | 4,015,520 | 178,784 | 443,864 |
| UI-5 + honest Memory row (§7) | 4,015,560 | 178,744 | 443,904 |
| UI-5 total cost | +2,196 | -2,196 | +2,196 |

Trajectory: 200,116 (UI-1) -> ~196,776 (UI-2) ->
188,880 (UI-3) -> 180,940 (UI-4) -> 178,744
(UI-5). The whole phase fits inside uiapp, as
expected. (The task brief cites UI-4's
merged-tree measure 181,164 B; the 224 B delta
to the fresh-worktree 180,940 B is the known
`__FILE__` path-string artifact, not a real
difference.) No strip is needed for UI-6 on size
grounds; the strip proposal stays a coordinator
decision.

## 3. Verification

### UI5-VERIFY (new, `scripts/verify-ui5.py`)

- Pre-change (UI-4 kernel, main-tree ELF):
  **FAIL 54/81** (27 FAILs, all UILV5/polish
  checks: no Edit button, no editor, no polish
  markers). Two checks passed spuriously under
  desync and were hardened before the
  post-change run: "save/cancel returns to the
  detail" used a trailing-200/500-byte window
  that matched a stale marker (now a
  pos-before-key wait), and "re-grant detail
  differs" accepted any diff (now a band,
  100 < fd < 20000, so a full-frame desync diff
  fails instead of passing). The verdict was
  decisive regardless.
- Post-change run 1: **79/81**. The two FAILs
  were both script bugs, diagnosed from the
  serial log, not device bugs:
  - *Setup flow reaches finale.* The four
    `setup done` sends after `gui` left no echo
    and no output in the daemon-launch storm,
    while every later shell command landed
    fine. Fix: `setup_flow` now probes sh
    readiness first (`setup status` must print
    `  name: `) and requires sh's own
    `setup: done` output or the finale marker
    per attempt, instead of blind sends.
  - *Idle token released.* Idle exit and release
    arrived in a single reader chunk, so a fresh
    pos for the release wait missed a release
    that had already arrived. Fix: one pos for
    both waits (the release comes after idle
    exit in the stream, so the shared pos finds
    it even in the same chunk).
- Post-change run 2: desync cascade (bound save
  FAIL, then Power opens FAIL with a black dump,
  then the idle section) under heavy guest lag;
  killed, fix-state run (name qaon, tz +0700,
  unprovisioned, PIN set -- `logs-ui5/fix-state.py`,
  scratch only, NOT committed), and rerun as
  the discriminator.
- Post-change run 3: **80/81**. The single FAIL
  was the PIN guard: after the idle test the
  face re-granted uiapp at once (same class as
  UI-4's "stolen grant" note -- face behavior,
  not a UI-5 regression: UI-5 touches no
  face/grant path), and in that window the
  probe line was consumed by qabotd (which
  echoes each char as it reads, and `shell_cmd`
  mistakes that echo for sh's echo and gives
  up). Fix: a `shell_probe` helper that only
  accepts the real output and retries raced
  lines, used for the PIN guard.
- Post-change run 4: **PASS 81/81**. Editor
  round trip (AB -> ABC via three UPs, save
  marker, shell read-back, re-grant detail
  diff 212, shell restore), BACK-cancel
  (KeepMe unchanged), 40-char bound (appends
  refused, exactly 40 saved), polish (monback
  focus, three button-BACK parity markers,
  clock band on all six screens), idle exit
  from the editor + release, PIN untouched,
  teardown (qaon/+0700/unprovisioned), no
  QEMU left. uiapp.bin=443,864, kernel
  binary=4,015,520, headroom=178,784.

### UI3-VERIFY update (same branch)

UI-3's name section asserted the "Edit in UI-5"
note era: it now asserts the Edit button (blue
band 100-124) alongside BACK (130-174). Same
class of change as the UI-3 UI2-VERIFY update
and the UI-4 FND1 update: the product changed,
so the gate follows it.

### Visual evidence (coordinator-readable dumps)

Standing rule from the coordinator (2026-10-26):
besides pixel-count asserts, the coordinator
reads every new screen's screendump before
merge. UI-5's run-4 originals live in
`~/workspace/logs-ui5/` (its own LOGDIR, never
shared with another phase's logs -- the
`logs-ui4/` staleness lesson: a shared dir let
a face frame from another run sit under a
Power-screen name). The canonical, clearly
named copies are in `~/workspace/logs-ui5/dumps/`
(PPM original + PNG conversion each); the
run-4 originals were NOT overwritten or deleted
to make them, and no later UI-5 run writes into
`dumps/`:

- `dumps/ui5-editor.ppm` / `.png` -- the NEW
  screen: the char-picker editor open on "AB"
  (status bar 19:01 guest clock, "Edit device
  name", value AB, "POS 3/2 CHAR _" append
  position, UP/DN + L/R + LEFT@0/RIGHT@end +
  OK/BACK hints). Copy of run-4 `ui5-edit.ppm`.
- `dumps/ui5-name-detail.ppm` / `.png` -- the
  CHANGED screen: the name detail showing AB
  with the blue "Edit name" button focused and
  the blue BACK button; the "Edit in UI-5" note
  is gone. Copy of run-4 `ui5-detail-ab.ppm`.

Other run-4 dumps still in `~/workspace/logs-ui5/`
(not new screens, listed for completeness):
`ui5-menu.ppm`, `ui5-list.ppm`,
`ui5-detail-abc.ppm` (detail after saving ABC),
`ui5-monitor.ppm`, `ui5-power.ppm`. All are
240x240 RGB.

## 4. Battery (worktree; full logs per leg)

Full logs: `~/workspace/logs-ui5/battery-<leg>.log`
(latest run per leg). Earlier attempts are kept as
`battery-ui3-rerun1.log`, `battery-ui3-rerun2.log`,
`battery-ui4-rerun1.log`, `battery-c6l2-partial.log`
(killed leg), `ui3-maintree.log` (+
`ui3-maintree-serial.log`) and `ui3-rerun-serial.log`,
`ui3-rerun2-serial.log`. UI5 itself (81/81) is not a
battery leg; it is §3.

Chain 1 (`battery.sh`, from 04:10): UI1 25/25, UI2
38/38, UI3 FAIL 66/78, UI4 FAIL 68/73, C6L1 29/29.
Killed 04:31 by a VM replacement mid-C6L2 (partial
log preserved; no process survived, disk recovered
with fix-state PASS before resuming).

Resume 1 (`battery-resume.sh`): UI3 rerun #1 FAIL
65/78, UI4 rerun #1 FAIL 69/73. Chain then paused
after the UI4 leg for a control experiment (below).

Control experiment (main-tree UI-4 kernel, same
UI3 script): FAIL 71/78 -- 1 expected FAIL (the
Edit-button check: the old kernel still shows the
"Edit in UI-5" note) + 6 PIN restore-flow FAILs
with the same idle-exit signature in the serial
(mid-entry idle exit, release, key flood into sh).
The PIN desync reproduces WITHOUT any UI-5 code:
it is a host-stall timing artifact, not a UI-5
regression. UI-5 touches no picker, idle-timer,
EV-queue, scheduler, or serial code (verified by
diff: editor + name-detail button, Monitor focus
callback, marker-only shell changes).

Resume 2 (`battery-resume2.sh`, remaining legs):
C6L2 36/36, C6L3 72/72, C6L4 60/60, C6L5 55/55,
IPC2 34/34, IPC4 58/58, IPC5 20/20, FND1 PASS,
FND2 21/21, FND3 46/46, FND4 36/36, FND5 33/33,
ONB1 PASS, ONB2 PASS (verdict line; exit code
ignored per standing rule), SHELL PASS, SAFEMODE
PASS, UPDATE PASS, FIRSTBOOT FAIL (below).

FIRSTBOOT in-chain FAIL was a state precondition,
not a regression: the script assumes boot 1 starts
unprovisioned but does not reset the disk itself,
and the UPDATE leg left it provisioned. Standalone
after fix-state: FIRSTBOOT-VERIFY PASS.

Retries to green (desyncs move between runs and
sections; nothing fails deterministically):
UI3 rerun #2 FAIL 56/78 (22 FAILs; earliest an
idle exit at the menu -> Settings transition, pin
changed/verified/mismatch all zero -- the picker
never ran, the run desynced before it).
UI4 rerun #2 PASS 73/73 (in-chain 68/73 failed
Monitor BACK + Power; rerun #1 69/73 failed only
the shutdown fresh boot; rerun #2 clean).
UI3 rerun #3 PASS 78/78 (in-chain 66/78, rerun #1
65/78, rerun #2 56/78, main-tree control 71/78).

Final battery verdict: every leg green on its
latest run (UI1 25/25, UI2 38/38, UI3 78/78,
UI4 73/73, C6L1 29/29, C6L2 36/36, C6L3 72/72,
C6L4 60/60, C6L5 55/55, IPC2 34/34, IPC4 58/58,
IPC5 20/20, FND1 PASS, FND2 21/21, FND3 46/46,
FND4 36/36, FND5 33/33, ONB1 PASS, ONB2 PASS,
SHELL PASS, SAFEMODE PASS, UPDATE PASS,
FIRSTBOOT PASS). UI3/UI4 needed retries: the
battery ran through a period of severe host
stalls (guest-time jumps trip the correct 30 s
idle exit mid key-burst); the mechanism and the
main-tree control are documented above so the
retries are evidence, not hand-waving.

Final shared-disk state after all runs + a closing
fix-state: PIN 246810, unprovisioned, healthy
(fail_count 0), no `qemu-system-arm` left.

## 5. Commits (branch `dev/ui5`, no merge, no push)

- `175f7ce` -- ui5: verify script (FAILs
  pre-change) + UI3 script follows Edit button
- `58bfc2e` -- ui5: LVGL name editor + polish
  (focus/back-menu parity)
- `1bd5475` -- ui5: dev log (editor + polish,
  battery, desync analysis)
- (this follow-up) -- ui5: honest Memory row
  (§7) + dev-log §7 and visual-dump list
- dev log: this file (committed on `dev/ui5`;
  final hash in the coordinator's merge report)

## 6. Follow-ups for the coordinator (not done here)

- The post-idle spontaneous re-grant (face
  re-grants uiapp immediately after an idle
  release) is worth a face-side look: it is
  harmless for shell flows (sh keeps the
  console), but it makes post-idle shell probes
  race qabotd's console reads. UI-5 works
  around it in-script (`shell_probe`); a
  product-level answer belongs to a face/harness
  pass, not UI-5.
- UI-6 owns: the test-program strip decision,
  the remaining single-send hardening in
  verify-ui3.py, and any general harness
  cleanup (both already on the coordinator's
  list from UI-4).
- Battery lesson (new, from this phase): under
  host stalls the guest clock jumps and the
  correct 30 s idle exit fires mid scripted
  key-burst, desyncing LVGL legs (UI3/UI4) in
  ways that move between runs. The control
  (same failure on the UI-4 kernel) and the
  clean retries are the discriminator; a future
  harness pass could make long scripted bursts
  resilient (e.g. re-grant + resume instead of
  fail-fast) rather than touching the product
  idle behavior, which is correct.

## 7. Memory row: impossible value -- investigation + honest display

Finding (coordinator, visual read of the Monitor
screen): "Memory: 199872/65536 KB" -- used larger
than total, physically impossible. Added as a
bounded UI-5 polish item.

### The arithmetic (exact)

SYS_STAT (`mach3-port/src/ext/user.c`, handler
for syscall 57) computes, as designed:

- `mem_total_kb = mem_size / 1024` = 65536 (64 MB)
- `total_pages = mem_size / 4096` = 16384
- `mem_used_kb = (total_pages -
  (unsigned)vm_page_free_count) * 4`

199872 / 4 = 49968 used pages, so
`(unsigned)vm_page_free_count` = 16384 - 49968
mod 2^32 = 4294933712, i.e. `vm_page_free_count`
= -33584 as the signed `int` it is declared as
in both `user.c` and `http.c`. The formula is
faithful; the INPUT is wrong. The kernel struct
layout vs ulib layout was already verified
identical (coordinator's check), so this is not
a display-layout bug: Mach's free-page counter
itself went negative.

### What the counter does (traced, not assumed)

- Definition: single BSS symbol in upstream
  `kernel/vm/vm_resident.c:347` (verified unique
  in the linked ELF: `407dab10 B
  vm_page_free_count`; the ARM port never touches
  the free list directly).
- Bootstrap is sane: every boot log prints
  "vm_page_bootstrap: 10152 free pages"
  (16384 - 10152 = 6232 pages held by the kernel
  image + page tables + stolen memory).
- MI accounting is balanced on inspection:
  `vm_page_grab` decrements + unlinks,
  `vm_page_release` increments + links (and
  panics on double-free), the contiguous
  allocator decrements exactly npages for npages
  removed. The ARM `pmap_startup`
  (`md/arm/pmap.c`, MACHINE_PAGES override)
  follows the same balanced pattern (steal page
  structs, `pmap_next_page` + `vm_page_init` +
  `vm_page_release` in reverse).
- No port-side caller of `vm_page_create`,
  `vm_page_grab`, or `vm_page_release` exists
  outside the MI code. uiapp persists across
  token grants (outer `for(;;)` in
  `user/uiapp/main.c`), so there is no per-grant
  task-teardown leak to blame.

### Reproduction attempts (all negative)

Scratch probes in `~/workspace/logs-ui5/memfix/`
(never in the repo, never touching run-4 dumps):

- `memprobe.py`: boot -> shell qfetch 27884 KB
  (free 9413, sane) -> provision -> gui ->
  Monitor marker 29872/65536 KB -> release ->
  spawns -> 40 s idle -> Monitor again ->
  final qfetch, all 29872 KB. Stable.
- `memsoak.py`: 40 grant cycles (grant ->
  Monitor -> BACK -> release) + spawn bursts +
  a 200-key burst, sampling every 10 cycles:
  29872 KB at every sample. Zero drift.

So ordinary UI use, grants, spawns, and key
traffic do NOT move the counter. The -33584
value was not reproduced in ~15 minutes of
probing. Candidates not yet exercised: very
long uptime (a slow leak would need hours at
any plausible rate -- 43736 net pages is 175 MB
of phantom grabs on a 64 MB machine, which no
steady workload can hold), qabot LLM prompt
loops, heavy web/IPC/linkd traffic, or a rare
one-off (list corruption would also panic or
corrupt data before reaching -33584 cleanly).
That is the VM rabbit hole the coordinator's
brief names: finding the trigger needs a
long-soak + workload bisection pass of its own,
not a UI-5 side quest.

### Choice: honest display, SYS_STAT untouched

`user/uiapp/lv_monitor.c` `collect()`: when
`st.mem_used_kb > st.mem_total_kb`, the Memory
row shows `-` (unknown) instead of the
impossible `used/total KB` -- mirroring the
Disk row's `-` for unknown and the CPU row's
`-`. Cost +40 B, all in uiapp (headroom
178,784 -> 178,744). Deliberately NOT changed:

- SYS_STAT's meaning, formula, or struct: both
  sides (kernel + ulib) plus qfetch (`sh.c`),
  `umon`, and web metrics (`http.c`) still see
  the raw numbers. Changing the field meaning
  would have forced updates across all of them
  for no gain while the counter itself is wrong.
- The old hand-rolled Monitor (`scr_mon.c`):
  it is dead code (no reference from
  `user/uiapp/main.c` or headers); the LVGL
  Monitor is the only live Monitor surface.

### Verification of the guard (both directions)

- Impossible in, `-` out: a temporary
  kernel-only hack forced SYS_STAT to report
  exactly 199872 KB (reverted immediately
  after; `git diff` confirms `user.c` clean).
  On that build, shell qfetch printed
  "Memory: 199872 / 65536 KB" (proving the
  forced value reached userspace) while the
  LVGL Monitor printed "UILV4: mon Memory: -"
  on both Monitor opens. The marker text IS
  the label text (same `vals[]` array), so the
  screen shows "Memory: -".
- Normal path unchanged: after revert +
  rebuild (byte-identical size to the guard
  build: 4,015,560 / 178,744), a fresh probe
  shows Monitor "Memory: 29872/65536 KB" and
  qfetch 27884 -> 29872 KB, exactly as before
  the guard. No full UI5-VERIFY re-run was
  done for this one-row display change: it
  would have overwritten the run-4 dumps in
  `logs-ui5/` that §3 preserves, and it
  asserts no Memory value (UI5-VERIFY's
  Monitor checks are clock band, monback
  focus, and BACK-to-menu; UI4-VERIFY only
  asserts the Memory row is PRESENT, which
  "Memory: -" satisfies).

### Open item for a later track

The kernel accounting root is still unknown.
If it ever reproduces, the first bisect axes
are: uptime length, qabot prompt count, and
web/IPC/linkd volume -- plus a boot-time vs
late-run comparison of `vm_page_free_count`
itself (it is only observable today via the
derived SYS_STAT number). Until then the
Monitor refuses to display an impossible
number, and qfetch/web still show raw SYS_STAT
-- that asymmetry is documented here, not
hidden.
