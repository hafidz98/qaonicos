# DEVLOG-UI6 — Cleanup + size audit (final UI phase)

Scope: audit and cleanup ONLY. No product code change, no pixel
change, no SYS_STAT change, no daemon-table change. The strip
itself is NOT executed here — §2 is a proposal for the
coordinator/user decision. Harness fixes (§3) are test-only.

Worktree: `~/workspace/qaonic_os-ui6`, branch `dev/ui6`, base
`a9bb593` (UI-5 merge). Build: `cd mach3-port &&
source ~/workspace/toolchain/env.sh && ./build-md.sh` →
"MD: 43 ok, 0 failed" + "LINK OK" (baseline, before any
harness edit; harness edits touch only `scripts/`, so the
kernel binary is unchanged by them).

## 1. Size audit (measure, don't strip)

Method (same as C6L-3/4/5/UI-1..UI-5): `llvm-objcopy-18 -O
binary` on `mach3.elf`; headroom = 4,194,304 − binary size.
Census method (same as DEVLOG-UI1 §4): `mach3-port/build/
<prog>.bin` file sizes (padded sizes as embedded).

### 1.1 Headroom series

| Phase | Kernel binary | Headroom | Notes |
|---|---|---|---|
| UI-1 | 3,994,188 | 200,116 | trimmed baseline (UI-2 gate) |
| UI-2 | 3,997,528 | 196,776 | +3,340 |
| UI-3 | 4,005,424 | 188,880 | +7,872 |
| UI-4 worktree | 4,013,364 | 180,940 | +7,940 |
| UI-4 merged | 4,013,140 | 181,164 | +224 `__FILE__` path artifact |
| UI-5 worktree | 4,015,520 | 178,784 | +2,196 |
| UI-5 merged | 4,015,336 | 178,968 | per UI-5 merge report |
| UI-6 worktree (this phase) | 4,015,560 | 178,744 | baseline, no product change |

UI-6 worktree vs UI-5 merged: −224 B. That is the same
`__FILE__` path-string class seen at UI-3/UI-4 (worktree
path `qaonic_os-ui6` vs the merge tree's path strings
embedded in the binary), not a product change: UI-6
touches no product source, and the rebuild after the
purge proof (§4) reproduces exactly 4,015,560 B.

Trajectory: UI-2/3/4/5 cost 3.3 / 7.9 / 7.9 / 2.2 KB.
At those rates the UI track fits with wide margin; no
phase in the track crossed zero, and no structural
decision (strip / littlefs images / bigger partition)
was ever forced by headroom. The strip below is
therefore an optional hygiene/insurance move, not a
rescue.

Other sizes (this build): `uiapp.bin` 443,904 B;
`linkd.bin` 61,604 B (embed cap 65,536 → 94.0%; do NOT
grow linkd — same standing rule as C6L-5).

### 1.2 Test-program census (re-measured)

| Program | Bytes | Referenced by |
|---|---|---|
| ulinktest | 54,188 | verify-c6l1.py |
| ulinkbr | 55,212 | verify-c6l2.py |
| ulink3 | 51,444 | verify-c6l3.py |
| ulinkaud | 56,756 | verify-c6l4.py |
| ulink5 | 22,872 | verify-c6l5.py |
| ulink5d | 18,728 | verify-c6l5.py (daemon idx 5) |
| ulinktools | 11,592 | verify-c6l5.py |
| ulink5die | 9,004 | verify-c6l5.py |
| uipcaud | 18,400 | verify-ipc5.py |
| uipcld | 12,132 | verify-ipc2.py, verify-ipc4.py (daemon idx 3) |
| uipkpark | 9,792 | verify-ipc2.py |
| uipknd | 9,308 | verify-ipc2.py |
| uipcregexit | 5,002 | verify-ipc4.py |
| uipcregfull | 5,589 | verify-ipc4.py |
| uiptoolssrv | 11,464 | verify-ipc2.py, verify-ipc4.py |
| **Family total** | **351,483** | |

UI-1 family total was 350,883; the +600 B drift is
exactly +40 B per program — the `__FILE__`/build-path
artifact class, not growth in test logic.

Second tier (re-audited, same method as UI-1):

| Program | Bytes | Status |
|---|---|---|
| utlscli | 753,884 | dead (see below) |
| utlskeep | 757,644 | dead (see below) |
| **Pair total** | **1,511,528** | (+80 vs UI-1, same artifact) |

Dead-weight re-verification (grep, this tree): NO
`scripts/verify-*.py` references `utlscli` or
`utlskeep`. The only references are the `extern`
declarations in `mach3-port/src/ext/user.c` (marked
SEMENTARA), the commented-out `embed:` map entries
("Nonaktif"), their own sources, and one comment in
`user/uqabotr.c`. The pair IS still embedded:
`mach3-port/build/gen/utlscli_img.c` /
`utlskeep_img.c` exist and their `_img.o` link via
the `obj-md` glob (the UI-2 lesson) even though no
map entry points at them.

Embedding mechanism (for the proposal): `build-md.sh`
runs `embed.py` per program (`.bin` → `gen/<name>_img.c`,
cap 65,536 each) and `user.c` maps `embed:<name>` →
image; daemon images live in the separate
`daemon_images[]` table. Stripping therefore has two
different shapes (§2).

## 2. Strip proposal (NOT executed — coordinator/user decision)

### Tier C — utls pair: 1,511,528 B, lowest risk, biggest prize

Remove: the `utlscli`/`utlskeep` build loops in
`build-md.sh`, the `gen/*_img.c` embed steps, the
`extern` declarations in `user.c`, and the stale
`_img.o` (Phase C purge covers the rebuild, §4).
Keep: the sources (`user/utlscli.c`, `user/utlskeep.c`)
buildable on demand (a QA/dev build flag, or manual
build) — the F1/Q2b TLS investigations they served are
closed, but the code is the only record of that work.
Battery impact: ZERO — no verify script references
either program, so the full battery runs unchanged on
the stripped image. This tier alone restores headroom
to ~1.69 MB.

### Tier A — embed-map test programs: 320,623 B, low risk

All family programs EXCEPT the two daemon-table ones
(§2, Tier B): 351,483 − 12,132 (uipcld) − 18,728
(ulink5d) = 320,623 B. Remove: the `embed.py` steps
in `build-md.sh` + the `embed:<name>` map entries in
`user.c` (+ purge). Keep: sources + a QA image that
retains them (build flag selecting a QA image, or a
separate QA build) — because EVERY one of these 13
programs is referenced by at least one battery script
(table in §1.2), the full battery CANNOT run on a
Tier-A-stripped production image. Evidence after a
strip must therefore be two-part: full battery on the
QA image + boot/smoke (shell, GUI, settings) on the
production image. Alternatively load them from
littlefs at QA time — that is the structural fix
below, not a strip flag.
Risk: low (map + build only, no daemon table, no
kernel semantics), but the QA/production image split
must be a deliberate build decision, not an accident.

### Tier B — daemon-table test daemons: 30,860 B, medium risk, separate decision

`uipcld` (12,132 B, progid 3) and `ulink5d` (18,728 B,
progid 5) are `daemon_images[]` entries, marker-gated
but load-bearing in the table: removing them shifts
every later progid (linkd 4→3, sh 6→4, spawn slot
7→5, NDAEMON 8→6). Reserved IPC names are enforced
via kernel-assigned spawn identity (D-IPC-2:
`tools` → progid 2, `link` claimed by linkd), so a
renumbering must update the reserved-name table and
every progid check together — kernel-adjacent work,
not a strip. Do NOT bundle Tier B into Tier A.
Defer Tier B until a daemon-table change is already
scheduled for another reason, or until the structural
fix below lands.

### Structural fix (subsumes all tiers, NOT started here)

Load daemon images from littlefs instead of embedding
them (the research proposal from DEVLOG-UI1 §4). It
removes the whole embedded-test-program question and
benefits every daemon, but it is kernel work in its
own right (image loading, fallback if littlefs is
damaged, boot-order consequences) and is NOT part of
UI-6. Recommended sequencing if the coordinator wants
the bytes: Tier C now (free), Tier A with a QA-image
flag when the build wants a production/QA split,
Tier B only with a daemon-table change, structural
fix as its own track.

### What the strip must NOT touch

`face`, `uiapp`, `qabotd`, `linkd`, `sh` (production
daemons), `SYS_STAT` semantics, the UI-5 Memory-row
guard, any screen behavior. Total on the table if all
three tiers ever land: 351,483 + 1,511,528 =
1,863,011 B — but Tiers A+B cost the battery's
ability to run on the production image, which is why
they stay proposals, not actions.

## 3. Harness cleanup (test-only)

### 3a. verify-ui3.py: all key sends injection-verified

Before (UI-4 era): `key()`, `send_keys()`,
`grant_menu()` hardened, but 20 single-attempt raw
`ser.sendall(b"key ...")` sends remained: 3 in helpers
(`goto_settings` OK, `release_menu` BACK ×2) + 17 in
the script body (OK/BACK before detail/list/PIN
markers). Each was a silent byte-loss site of the
class that caused the UI-4-era 63/76 + 47/76
episodes.

After (this phase): all 20 converted to the
injection-verified `key()` pattern (send + wait
`key: injected` 5 s + resend once, then needle wait).
Conversion rules, kept uniform so the diff reviews
cleanly:
- Body sites: `pos = len(serial)` line KEPT as the
  window anchor (later `wait_from(pos, ...)` reuses
  in the tz-detail and PIN sections depend on it),
  the send + `wait_from` pair replaced by one
  `key(ser, k, needle, timeout)` inside the same
  `check()` call.
- `goto_settings`: `return key(ser, "ok",
  "UILV3: settings list", 20)`.
- `release_menu`: both BACKs via `key()`; the outer
  `pos` anchor is kept from before the second BACK
  so the final `wait_from(pos, "uiapp: token
  released", 20)` keeps its original window.
- The 4 remaining raw sends are `grant_menu`'s own
  resend logic: each send is immediately followed by
  a `wait_from(pos, "key: injected", 5)` — verified
  by construction, not single-attempt. Nothing
  single-attempt remains in the script.
- Denominator UNCHANGED: 78 checks (79 `check(`
  matches including the `def check` line). No check
  added, none removed; the teardown PIN-246810
  restore check from the UI-4 hardening stays.

Test-first evidence (unit demonstration, real code):
the actual `wait_from`/`key`/`send_key` functions
were extracted from the edited script via `ast` and
run against a fake serial that drops the FIRST send
of each key (PL011 byte-loss simulation):
old single-attempt pattern → False; new `key()` →
True (resend lands); clean channel → True first
try. Full-script proof is the UI3 battery leg (§5).

### 3b. verify-c6l4.py: missing logs-c6l4/ no longer crashes

Before: `os.listdir(os.path.join(ROOT, "logs-c6l4"))`
at module level, before QEMU boot. A fresh worktree
lacks `logs-c6l4/` (the UI-1 battery lesson), so the
script died with `FileNotFoundError` before doing
anything — demonstrated pre-change by moving the
directory away and running (traceback, no QEMU).
After: `os.makedirs(..., exist_ok=True)` before the
cleanup loop. Post-change proof: the C6L4 battery
leg (§5) is run with `logs-c6l4/` removed first —
the script recreates it and proceeds.

### 3c. verify-onb2.py: exit code now reflects the verdict

Before: the script printed `ONB2-VERIFY: PASS/FAIL`
and fell off the end — exit code always 0, even on
FAIL. That is why every battery table since C6L-3
has carried the "read the verdict line, ignore the
exit code" caveat.
Pre-change proof (forced-fail copy, run in-tree so
ROOT resolves): all 4 real checks PASS, verdict line
`ONB2-VERIFY: FAIL ['forced-demo']`, exit code 0.
After: `sys.exit(0 if not fails else 1)` appended
after the verdict print (verdict line kept as the
human signal). Post-change proof: the ONB2 battery
leg (§5) exits 0 on PASS; the forced-fail copy of
the FIXED script exits 1 (see §5 notes).

## 4. Hygiene: Phase C purge still active

`build-md.sh` line 747 still purges first:
`rm -f "$OBJMD"/*_img.o` before recompiling the
explicit program list (the UI-2 fix `47f35e3`).
Proof this phase: planted `obj-md/stale_test_img.o`,
rebuilt → file gone, build green, `*_img.o ok (16
programs)`, kernel binary reproduces exactly
4,015,560 B. No stale-artifact regression. (An
earlier mis-plant in `build/` instead of
`build/obj-md/` survived a rebuild — correctly, since
the purge targets `obj-md` only; that file was
removed by hand and is not part of the proof.)

Note for the coordinator: `~/workspace/logs-ui3/
fix-state.py` is STALE — it hardcodes
`~/workspace/qaonic_os-ui3` (a removed worktree) as
ROOT, so its QEMU never starts on current trees.
It was NOT used this phase (UI3-VERIFY's own
setup-reset + teardown PIN restore normalizes the
disk instead). Either repoint it at the current
tree or retire it; it is not committed either way.

## 5. Battery (standalone, full logs in ~/workspace/logs-ui6/)

| Leg | Result | Notes |
|---|---|---|
| UI1 | PASS 25/25 | |
| UI2 | PASS 38/38 | |
| UI3 (converted script) | PASS 78/78, FAIL 72/78, PASS 78/78 | see flake note below |
| UI4 | PASS 73/73 (rerun) | first attempt SIGTERM'd by a runtime restart at 45/0, no verdict — not a test failure |
| UI5 | FAIL 79/81, FAIL 79/81, PASS 81/81 | see flake note below |
| FND1 | PASS | no ev=12 flake this time |
| FND2 | PASS 21/21 | |
| FND3 | PASS 46/46 | |
| FND4 | PASS 36/36 | |
| FND5 | PASS 33/33 | |
| C6L1 | PASS 29/29 | |
| C6L2 | PASS 36/36 | |
| C6L3 | PASS 72/72 | |
| C6L4 | PASS 60/60 | run with `logs-c6l4/` removed first — makedirs fix proven in the real run |
| C6L5 | PASS 55/55 | incl. the death-window check |
| IPC2 | PASS 34/34 | |
| IPC4 | PASS 58/58 | |
| IPC5 | PASS 20/20 | |
| ONB1 | PASS | |
| ONB2 | PASS 4/4, exit 0 | forced-fail copy of the fixed script exits 1 with FAIL verdict |
| SHELL | PASS | |
| SAFEMODE | PASS | |

End state: no `qemu-system-arm` left, no mock-c6 peer
left; the final UI3 run's teardown check confirms PIN
246810 restored + disk unprovisioned.

### Flake note A — UI5 40-char bound (79/81 twice, then 81/81)

Both failing runs failed the same two bound checks:
the editor saved 26 A's (run 1), then 27 A's (run 2),
not 40. Mechanism, proven by experiment (scratch
script `/tmp/ui6-burst-test.py`, NOT committed):
the editor saves exactly what it loads (26→26,
27→27); the shell write path has no truncation
(DEVLOG-UI5); the loss is upstream — the guest's
echo of `setup name <40 A's>` shows only 26/27 A's
arrived, i.e. the long argument was shortened in
transit while the guest was busy after GUI release
activity. Control: 5/5 burst trials of the same
command from an idle shell stored the full 40
(displayed 39 = the known shell-display cap).
`shell_cmd` cannot detect this: a truncated name
still prints `setup: name saved`. This is the
"desync cascade under heavy guest lag" class
DEVLOG-UI5 run 2 already saw — a harness-timing
flake, not a UI-5 product bug and not a UI-6
regression (UI-6 touches neither UI5 code nor its
script). Follow-up (not UI-6 scope): `shell_cmd`
should verify long arguments (e.g. echo read-back)
or pace long sends; the bound test's
"RIGHT/UP at cap refused" checks also pass
spuriously on any rebuild marker, so they cannot
distinguish refused vs moved.

### Flake note B — UI3 PIN-burst drain stall (run 2: 72/78)

Run 2's cascade starts at "old PIN 5678 verifies":
the 30-key 5678 old-entry burst printed only 13
`pin entry old` markers, then `UILV2: idle exit`
(30 s) and token release, while the script's
remaining keys kept echoing `key: injected` with no
picker response. This refines the UI-4-era diagnosis
(18 markers, then idle exit): injection verification
now proves every key reached and was parsed by sh,
so the loss is DOWNSTREAM of sh — the picker
stopped draining mid-burst (EV-queue drop or
picker-drain stall under load), not PL011 input
loss. The stall sits inside `pin_entry` /
`send_keys`, code UI-6 did not touch, on a path
the same converted script passed twice (78/78,
78/78). Follow-up (not UI-6 scope): `pin_entry`
should verify per-key picker markers (the picker
rebuilds once per processed key, so marker
counting is available) or pace bursts to the
drain rate; a product-side look at the picker
drain path is warranted only if the stall recurs
systematically — on current evidence (2/3 green)
it is load-dependent.

## 6. Honest limitations

- The strip proposal (§2) is measured and audited,
  not executed and not battery-proven on a stripped
  image. Tier A/B battery consequences are stated,
  not demonstrated — a strip PR must carry its own
  two-image battery evidence.
- The UI3 conversion proof at unit level uses a
  simulated drop; the full-script proof is the UI3
  battery leg (§5), which is 2/3 green — the one
  red run is the downstream picker-drain stall of
  flake note B, not an injection loss (every send
  is now verified + resent, and the stall is
  inside unconverted `pin_entry` bursts).
- ONB2's exit-code fix changes harness semantics
  only; any wrapper that previously relied on
  "ONB2 always exits 0" (none known) would now see
  failures — that is the intended behavior change.
- No product behavior was verified beyond the
  battery legs in §5: UI-6 changes no pixels by
  design, so no new screendumps are claimed here.
