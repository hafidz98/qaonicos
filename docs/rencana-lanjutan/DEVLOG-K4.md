# DEVLOG-K4 — FND-3 respawn coverage for face

Status: [TO FILL: PASS/FAIL + battery table]. Branch `dev/k4`,
worktree `qaonic_os-k4`, base = K3 merge (push 269).

## 1. The gap, precisely

FND-3's supervisor respawns HUNG daemons: `wd_supervise()` scans
runnable slots (state 1) at the scheduler switch points, and a slot
whose progress age passes warn (10 s) then hang (20 s) for face is
reclaimed + respawned under the 2-per-boot budget (bootlog ev 15).
What it never did: respawn a daemon that EXITS or FAULTS.
`sched_exit_switch()` marked the slot dead (`state = 0`,
`uprog_states = EXITED`) and the supervisor skipped dead slots
forever after.

For most daemons that is an annoyance; for face it wedges the
whole GUI: face is the ONLY display granter (SYS_DISPLAY_GRANT is
face-gated), so once face is dead, uiapp can never re-acquire the
token, and after uiapp's idle release the display has no renderer
at all. Face's own exit paths are real: flush failure
(`flush_frame` / `flush_black` -> `sys_exit(1)`, the UI-4 race fix
made it rare but did not remove it), INFO/geometry failure at
startup, and any user fault. The code itself said so
(`user/face/main.c`: "SYS_DISPLAY_FLUSH -> exit, no respawn").

Two related facts found while investigating (both shape the fix):

- Slots follow REGISTRATION order, progids don't. In dev mode sh
  registers first (slot 0, progid 6) and face registers on `gui`
  (slot 1, progid 0); in user mode face is slot 0. Any face test
  must key on progid, never slot.
- The spawn child can never false-positive the face check: its
  progid is -1 (`embed_progid`).

## 2. Design

### 2.1 Death-respawn, face-only, in `sched_exit_switch`

After `daemon_reclaim()` + `state = 0` + `uprog_states = EXITED`,
and before `wd_supervise()`, if the exiting daemon's progid is
`PROG_FACE`:

- Budget left (`wd_respawns < WD_MAX_RESPAWN`): increment,
  `wd_respawn()` (same machinery as the hang path: fresh Mach
  task/pmap/context in the same slot, `uprog_states` back to
  RUNNABLE), bootlog ev 15, print `watchdog: 'face'
  death-respawned (respawn N)`.
- Budget exhausted: `wd_fatal_reset("face")` (bootstate watchdog
  mark + PSCI reset, exactly the hang path's fatal rung) so the
  next boot converges to safe mode; never a reset loop. The
  `wd_respawns = 0` reladder after the fatal call mirrors the
  hang path for the PSCI-refuses case.
- `wd_respawn()` failure: same fatal rung.

The budget is SHARED between the hang path and the death path:
2 respawns per boot total for face, whatever the mix. Other
daemons keep their exact FND-3 contract (no death-respawn for
them; the block is progid-gated). The block is inside
`#if WD_ENABLED`, like the rest of the supervisor, so
`WD_ENABLED=0` reproduces the old behavior.

The fresh face joins normal rotation via the existing
`pick_next()` below — deliberately NO ctx surgery: the exiting
frame must not resume the new instance (contrast the hang path,
where the current daemon's frame IS reused because the caller
yielded). `pick_next()` wraps around (`i = 1..NDAEMON`), so
even a lone fresh face is picked.

Forward declarations for `wd_respawn` / `wd_note` were added next
to the existing `wd_supervise` / `wd_fatal_reset` declarations
(both are defined later in the file than `sched_exit_switch`).

### 2.2 Display-token + face-request hygiene on face death

`daemon_reclaim()` already handled face-died-holding-display
(token stays PROG_FACE-keyed home; sleep request, queued input,
and pending face request cleared) and uiapp-died-holding (token
home + queue dropped). What it did NOT do: clear a pending face
request when face died while UIAPP held the token (neither holder
sub-branch runs, and the "posted BY the dying daemon" branch only
fires when the author died). A respawned face would then consume
a dead instance's request.

K4 adds, in `daemon_reclaim()`, after the holder block: on ANY
face death (`progid == PROG_FACE`), drop a pending face request
(print `reclaim: dropped pending face request (face died)`).
The request is one-way polled (no waiter blocks on it), so the
requester re-posts if still relevant. Token semantics for uiapp
are untouched: a face death while uiapp holds leaves the token
with uiapp, and the respawned face waits (its loop yields on
`sys_display_status() != 0`) until uiapp releases.

### 2.3 Test hook: single-shot kill/wedge marker in face

`user/face/main.c` polls littlefs `face-kill` ~1 Hz, in ALL
states, BEFORE the holder check (so a non-holder face also dies
— required for the uiapp-holds direction):

- `"1"`: consume (overwrite `"0"`) then `sys_exit(1)`.
- `"2"`: consume (overwrite `"0"`) then yield forever (no
  further syscalls → progress freezes → hang path).

Consumption-before-effect means a respawned face never re-arms
itself; a stale arm left by a crash costs at most one death or
wedge + respawn. Precedent: UI-1's `/data/ui1-test` marker.
Armed from the shell via `put /data/face-kill <v>` (web shell
post-GUI, per the FND-3 transport rule).

### 2.4 What was NOT changed

- FND-3 hang ladder, deadlines, budget, and events for all
  daemons (face's hang deadlines 10/20 s unchanged).
- Daemon order, `daemon_images[]`, display token semantics,
  linkd, SYS_STAT, Memory guard, name editor (removed), icon
  font, Tier B daemons, upstream (pristine).
- No new syscall. No new kernel resident (R14: the fix reuses
  the existing supervisor machinery).
- No architecture exception needed (ARSITEKTUR-SISTEM.md §2
  read before finalizing): the change serves P7 ("no daemon
  death wedges") and preserves P5 (one display owner — the
  respawned face waits for the token, never steals it) and
  P4 (policy in userspace, arbitration in the kernel). The
  supervisor + reclamation machinery it extends is the
  extension-layer component §3.1 already documents as built.

## 3. Verification — `scripts/verify-k4.py`

One QEMU process, in-place resets (FND-3 pattern). Post-GUI
shell traffic goes through the web shell; key injection goes
through the web shell too (`key menu` -> `key: injected` on
serial, with a typed-serial resend fallback). Per-boot phases
(fresh 2-respawn budget each boot):

- A (boot 1, face holds): provision to finale (idle eyes) ->
  kill -> death-respawned (respawn 1) -> starting print -> eyes
  again -> bootlog ev=15 -> grant works -> BACK -> eyes again.
- B (boot 2, uiapp holds + hygiene): grant menu -> post face
  request (`face 1 K4PENDING`) while uiapp holds -> kill ->
  death-respawned (respawn 1) -> starting print -> clear print
  (`reclaim: dropped pending face request`) -> uiapp still in
  menu -> BACK -> eyes -> grant again.
- C (boot 3, budget): kill -> respawn 1 -> kill -> respawn 2
  -> grant works.
- D (boot 4, past budget): kill -> respawn 1 -> kill ->
  respawn 2 -> kill -> budget exhausted + FATAL -> new boot
  reason=3, SAFE MODE -> reboot -> normal boot, fail_count=0,
  face alive after recovery.
- E (boot 5, hang path): wedge -> warn -> HANG ->
  hang-respawned (respawn 1, NOT death-) -> starting -> grant.
- Cleanup: setup reset -> NOT provisioned, fail_count=0,
  PIN 246810 login works, name qaon + tz +0700 restored.

Stale-frame caveat (documented in the script): a screendump
after face death still shows the last flushed frame, so eye/menu
pixels alone never prove liveness. Every respawn check pairs
pixels with a liveness signal only a live face produces: the
`face: Qabot server starting` print and a working grant (a dead
face cannot grant).

### 3.1 Pre-change FAIL evidence (hook-only scratch worktree)

Scratch worktree `qaonic_os-k4base`, branch `dev/k4base`, base
push 269, containing ONLY the face hook (`user/face/main.c`
copied from `dev/k4`; `verify-k4.py` copied; `disk-images/`
symlink recreated). Five runs were needed because the script
itself had three bugs, each found by a run and fixed before
the next; the kernel never changed between runs.

- Run 1 — FAIL 38/67. Every death-respawn, budget, and
  grant-after-respawn check failed (face stays dead, no
  ev=15, no grant). Two script bugs surfaced: (a) phase E
  failed wholesale because phase D's internal recovery
  reboot invalidated the web session cookie, so phase E's
  `web_cmd("reboot")` was silently rejected — fixed with
  `reboot_via_web()` (fresh login before every inter-phase
  reboot). (b) Eye-pixel checks passed on a dead face's
  last frame (stale frame), while grant checks failed —
  confirming the stale-frame caveat empirically.
- Run 2 — killed by a VM replacement mid-phase-D (SIGTERM,
  `uptime` 1 min after). Partial log matches run 1 for
  phases A–D. The kill also left a stale `/data/face-kill
  = "1"` on the shared disk, which would have killed the
  next run's first face: fixed with a marker clear after
  every phase's login, before its `gui` (race-free: face
  has not started yet).
- Run 3 — FAIL 42/67. Same death failures; phase E failed
  at "wedge marker consumed" with boot 5 and GUI up. Cause
  undetermined from the data (the script saved no serial
  transcript then). A standalone wedge repro (fresh boot,
  provision to finale, arm `"2"`) later passed
  consumed → warn → hang → hang-respawn on the hook-only
  kernel, proving the mechanism works; phase E was made
  self-contained (own `provision()`) to match the repro
  instead of depending on phase D's leftover state.
  Run 3 also false-PASSED the phase-A bootlog check (see
  run 4).
- Run 4 — FAIL 49/68. Death checks failed; phase E passed
  fully (hang path already covered face, as expected).
  The phase-A bootlog check false-PASSED again, and this
  time it was root-caused with a transcripted phase-A
  repro: `bootlog_dump()` waited for the `"qaon>"` prompt,
  but a stale prompt from the previous web command can
  land after `pos` and satisfy it instantly, returning a
  near-empty base dump; the after dump then "revealed" an
  ev=15 record from an earlier boot (run 4's base race
  surfaced the wedge repro's record; the repro-A run
  surfaced run 4 phase E's record — same mechanism both
  times). Fixed two ways: the dump now waits for its own
  header-stated record count (`bootlog: N records` →
  exactly N `boot #` lines), and the check additionally
  requires the record's boot number to equal the current
  boot (newest number in the baseline dump). The reader
  thread now also tees the full serial transcript to
  `logs-k4/verify-k4-serial.log` for future diagnosis.
- Run 5 — FAIL 48/68, the definitive pre-change evidence.
  All 20 death/budget checks fail (A: 4 including the
  bootlog check, now correctly failing with the scoped
  rule; B: 5; C: 3; D: 8). Phase E passes fully. Log:
  `logs-k4/verify-k4-PRECHANGE5.log` (+ serial transcript
  `logs-k4/verify-k4-serial-PRECHANGE5.log`).

Net pre-change verdict: the script FAILS on the hook-only
kernel exactly where the gap is (death-respawn, budget,
grant-after-death) and PASSES exactly where FND-3 already
covered face (hang path, phase E).

### 3.2 Post-change PASS evidence

`dev/k4`, production build (MD 44 ok, LINK OK):
**K4-VERIFY: PASS (68/68)**, zero failures, no QEMU left.
Log: `logs-k4/verify-k4-POSTCHANGE.log` (+ serial transcript
`logs-k4/verify-k4-serial-POSTCHANGE.log`). Every phase
passed, including the discriminators that failed pre-change:

- A: kill while face holds → `death-respawned (respawn 1)`
  → starting print → eyes again → NEW ev=15 record for
  this boot → grant works → BACK releases to eyes.
- B: kill while uiapp holds → death-respawned → starting
  print → `reclaim: dropped pending face request (face
  died)` → uiapp still in menu (blue) → BACK releases to
  respawned face eyes → grant works again. The respawned
  face waited its turn (token stayed with uiapp) and the
  dead instance's request was not consumed.
- C: two deaths in one boot → respawn 1 → respawn 2 →
  grant works.
- D: third death in one boot → budget exhausted + FATAL →
  new boot reason=3, SAFE MODE → reboot → normal boot,
  fail_count=0, face alive after recovery.
- E: wedge → warn → HANG → hang-respawned (respawn 1,
  not death-) → starting → grant works (hang path
  unchanged).
- Cleanup: setup reset → NOT provisioned → reboot →
  fail_count=0 → PIN 246810 login works → name `qaon`,
  tz `+0700`, shell_on restored.

## 4. Battery (split PROD / QA=1, K1 lesson)

Per-leg logs: `~/workspace/logs-k4/battery-prod/` and
`~/workspace/logs-k4/battery-qa/` (scratch, not committed).
Runner: `logs-k4/run-battery.sh` (resume-safe; a leg
recorded exit 0 is skipped on re-run).

PROD image legs (production build, `dev/k4` kernel):

| Leg | Result |
|-----|--------|
| K1 | PASS 7/7 |
| UI5 | FLAKY, not a K4 regression (see note) |
| UI4 | PASS 73/73 (on 2nd standalone rerun) |
| UI3 | PASS 78/78 |
| UI2 | PASS 38/38 |
| UI1 | PASS 25/25 |
| FND1 | PASS |
| FND2 | PASS 21/21 |
| FND3 | PASS 46/46 |
| FND4 | PASS 36/36 |
| FND5 | PASS 33/33 |
| ONB1 | PASS |
| ONB2 | PASS |
| SHELL | PASS |
| SAFEMODE | PASS |
| K4 (own verify) | PASS 68/68 (§3.2) |

QA=1 image legs (Tier A test programs are QA-only):

| Leg | Result |
|-----|--------|
| K2 | PASS 21/21 |
| C6L1 | PASS 29/29 |
| C6L2 | PASS 36/36 |
| C6L3 | PASS 72/72 |
| C6L4 | PASS 60/60 |
| C6L5 | PASS 56/56 |
| IPC2 | PASS 34/34 |
| IPC4 | PASS 58/58 |
| IPC5 | PASS 20/20 |

Notes:

- K2 runs on the QA image, not PROD: `ulinktools` is
  QA-only (K3 established this; the task's "where
  applicable" qualifier covers it). No PROD attempt was
  made for K2.
- C6L5 reports 56/56 from its own verdict line; the task
  text's 55/55 is the stale expectation K3 already noted.
  The script verdict is authoritative.
- UI4: chain 71/73 (tz-restore readbacks) → rerun 72/73
  (glyph-exact clock) → 2nd rerun PASS 73/73. UI5: chain
  72/73 → rerun 71/73, all failures shell-readback /
  long-burst checks. A hook-free base-kernel control
  (push 269) then ran UI5 → FAIL 70/73 including the
  identical "shell still reads KeepMe" check, and UI4 →
  PASS 73/73. Conclusion: both suites are flaky in
  documented classes (UI5 long-burst loss, UI4 tz-set
  timeout / timing-sensitive rendering) on base and K4
  alike; no failure set overlaps between runs, and no
  K4 code path executes in these suites (no face death
  → neither the reclaim branch nor the death-respawn
  branch runs). UI5 never reached 73/73 in any run this
  session on either tree — reported honestly, not
  claimed as a pass.
- Final shared-disk state verified after the battery
  (PROD rebuild + check boot): NOT provisioned, name
  `qaon`, tz `+0700`, PIN 246810 login works,
  fail_count=0, no QEMU or peer process left (the one
  `pgrep -f` hit was the checking shell's self-match).

## 5. Honest limitations / follow-ups

- Death-respawn is face-only by design (brief scope). Other
  daemons still stay dead on exit/fault; extending it is a
  contract change for them, not a bug fix.
- Respawn still leaks the dead instance's Mach task/VM
  structures (FND-3's documented gap, bounded by the budget).
- The `face-kill` marker is a test hook in production code
  (UI-1 precedent). Worst case from a stale arm: one death or
  wedge + respawn; the budget + fatal rung bound any abuse
  loop to safe mode, never a brick.
- Class-(b) (never-yields) face spin still ends in the IRQ
  terminal rung (reset + safe mode), not a respawn — IRQ
  context cannot run respawn machinery (FND-3 limitation).
- `wd_respawns` is shared between hang and death paths: a boot
  that already hang-respawned face twice has no death-respawn
  budget left. Intended (one budget, conservative).
