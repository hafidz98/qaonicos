# DEVLOG-UI8 — strip dead programs + status-bar icon font

Two user-ordered jobs (2026-10-27) in one branch,
strip first, then icon-font revision. Worktree `qaonic_os-ui8`, branch
`dev/ui8`, base = UI-7 merge (push 258).
This log grows across the two commits: commit 1
covers the strip (sections 1-2, 4-6 here); the
icon-font sections arrive with commit 2.

Job 1 executes the UI-6 strip audit (DEVLOG-UI6
§2) for Tier C + Tier A, on the user's one-line
order "hapus semua yg program mati GK kepakai".
Job 2 replaces the UI-7 hand-drawn status-bar
masks with a real icon font, on the user's
feedback that the masks "masih ga bagus".

## 1. Job 1 — strip (Tier C + Tier A)

Method (same as UI-1..UI-7): `llvm-objcopy-18 -O
binary` on `mach3.elf`; headroom = 4,194,304 −
binary size. Census: `mach3-port/build/<prog>.bin`
(pad-bss included, so .bin size = embedded size).

### 1.1 Baseline (pre-strip, = QA image)

Kernel binary 4,014,648 B, headroom 179,656 B
(UI-7 post-change figure, reproduced exactly).
This build is preserved as `logs-ui8/qa-kernel.elf`
and is the QA image for the evidence below.

### 1.2 Tier C — utls pair: 1,511,528 B, removed

| Program | Bytes | Status |
| --- | --- | --- |
| utlscli | 753,884 | dead, removed |
| utlskeep | 757,644 | dead, removed |

Re-verified by grep (this tree): NO
`scripts/verify-*.py` references `utlscli` or
`utlskeep`. Removed: both build sections in
`build-md.sh`, the `gen/*_img.c` embed steps, the
`extern` declarations in `user.c`. (Their map
entries were already commented out, "Nonaktif",
and stay as comments.) Kept: the sources
(`user/utlscli.c`, `user/utlskeep.c`) buildable on
demand — restore the deleted sections from git
history (this commit's parent) or build the .bin
standalone with the `init.ld` pattern in
`build-md.sh` loop 1. Battery impact: ZERO.

One catch found while deleting: the utlscli
section also built `ca_pem.o` (pem2c.py from the
TLS test CA), and qabotd's link line still needs
it. The pem2c + ca_pem.o steps moved into the
TLS lib section (1d3), their natural home; the
TLS objects themselves are untouched.

### 1.3 Tier A — embed-map family: 320,623 B, removed

| Program | Bytes | Battery leg that referenced it | Disposition |
| --- | --- | --- | --- |
| ulinktest | 54,188 | C6L1 | leg runs on the QA image only |
| ulinkbr | 55,212 | C6L2 | leg runs on the QA image only |
| ulink3 | 51,444 | C6L3 | leg runs on the QA image only |
| ulinkaud | 56,756 | C6L4 | leg runs on the QA image only |
| ulink5 | 22,872 | C6L5 | leg runs on the QA image only |
| ulinktools | 11,592 | C6L5 | leg runs on the QA image only |
| ulink5die | 9,004 | C6L5 | leg runs on the QA image only |
| uipkpark | 9,792 | IPC2 | leg runs on the QA image only |
| uipknd | 9,308 | IPC2 | leg runs on the QA image only |
| uiptoolssrv | 11,464 | IPC2, IPC4 | leg runs on the QA image only |
| uipcregexit | 5,002 | IPC4 | leg runs on the QA image only |
| uipcregfull | 5,589 | IPC4 | leg runs on the QA image only |
| uipcaud | 18,400 | IPC5 | leg runs on the QA image only |

Removed: loop-1 list entries, the five
individual build sections (ulinktest, ulinkbr,
ulink3, ulinkaud, ulink5), the Phase C list
entries, the `extern` declarations, the `hooks[]`
entries, and the `uprogs[]` ulinktest entry in
`user.c`. Kept: all sources, plus a QA build
flag (below) so the programs stay buildable
in-tree.

No kernel-feature coverage is silently dropped:
every Tier A leg still runs — on the QA image,
which retains Tier A. The production image loses
only the ability to RUN those test programs, not
the kernel features they exercise (link layer,
bridge, wire v2, AUD, linkd client, IPC parks,
registry, AUD-rate queue — all still present in
the production kernel; only their test drivers
are gone).

### 1.4 QA build flag (the durable mechanism)

`QA=1 ./build-md.sh` builds the QA image: loop 1
+ Phase C include the Tier A programs, and
`user.c` compiles with `-DQA_TIER_A`, which
enables the Tier A `extern` declarations, the
`hooks[]` entries, and the `uprogs[]` ulinktest
entry. Default (QA unset/0) is the stripped
production image. Tier C is NOT behind the flag:
nothing references utls, so no QA leg needs it;
Tier C stays restorable from git history only
(§1.2). The flag is documented here and in the
`build-md.sh` comments, not in a separate script,
so it cannot drift from the build.

Evidence rule (UI-6 audit, followed): full
battery on the QA image + smoke + non-Tier-A
legs on the stripped production image (§5).

### 1.5 Totals

Tier C 1,511,528 + Tier A 320,623 =
**1,832,151 B stripped**. Headroom before
179,656 B → after [TO FILL from run] (objcopy
method). Tier B (30,860 B) deliberately NOT
stripped — see §2.

### 1.6 What the strip did NOT touch

`face`, `uiapp`, `qabotd`, `linkd`, `sh`
(production daemons), `uipcld` + `ulink5d`
(Tier B, §2), `SYS_STAT` semantics, the UI-5
Memory-row guard, any screen behavior, the
name-editor revert (UI-7).

## 2. Tier B impact brief (NOT executed — needs its own decision)

Tier B = `uipcld` (12,132 B, daemon_images
index 3) + `ulink5d` (18,728 B, index 5),
**30,860 B** total. Both are marker-gated
test daemons (dormant in production boots:
uipcld needs `/data/iptoolsc-test == "1"`,
ulink5d needs `/data/link5-test == "1"`), but
they are load-bearing in the daemon table.

### 2.1 Battery legs that need them

- IPC2: concurrent SEND/WAKE proof runs between
  the uipcld daemon and the uiptoolssrv mock
  `tools` server. Without uipcld there is no
  concurrent client daemon for this proof.
- IPC4: marker-gated client daemon (uipcld) for
  registry + concurrent-client legs.
- C6L5: ulink5d is the second linkd client for
  demultiplexing, EVT fan-out, the qlimit-5
  voice-rate measurement (its ok markers), and
  the client-death probe (ulink5die sends one
  raw CMD and exits; linkd must keep serving
  ulink5d).

Removing Tier B therefore does NOT just delete
two programs: it deletes the harnesses for
concurrent-client and second-client kernel
coverage. A Tier B removal must re-point that
coverage (new harness daemons) or explicitly
drop those legs — it cannot silently keep the
battery green.

### 2.2 Progid shift (the mechanical part)

Current `daemon_images[]`: face 0, uiapp 1,
qabotd 2, uipcld 3, linkd 4, ulink5d 5, sh 6
(+ spawn slot 7, NDAEMON 8). After removal:
face 0, uiapp 1, qabotd 2, linkd 3, sh 4
(+ spawn slot 5, NDAEMON 6).

### 2.3 Code that keys on progid (the checklist)

A Tier B removal PR must change ALL of these
together, or the image breaks:

1. `daemon_images[]` itself (remove 2 entries)
   + `NDAEMON` 8 → 6 + its comment.
2. `ipc_glue.c` `reg_reserved[]` (D-IPC-2):
   `{ "tools", 2 }` unchanged (qabotd stays
   index 2), but `{ "link", 4 }` MUST become
   `{ "link", 3 }` — otherwise linkd (now
   progid 3) CANNOT claim its reserved name
   and all of linkd IPC dies.
3. `user.c` watchdog deadline switch
   (`launch_daemon`): `case 6: /* sh */`
   (6000/12000 ticks = 60/120 s) MUST become
   `case 4` — otherwise sh falls into `default`
   (1500/3000 = 15/30 s) and the shell, which
   blocks on input, gets watchdog-killed after
   30 s idle. This is the least obvious coupling
   and the easiest to miss. (Better fix for that
   track: key deadlines by daemon name/role,
   not progid.)
4. `SH_IMAGE_IDX` is computed
   (`NDAEMON_IMAGES - 1`) — safe. `daemons[]`
   sizing, spawn slot (`NDAEMON - 1`),
   `wd_imgidx` respawn identity, and
   `launch_daemon_idx` all follow indices —
   safe, but re-verify by grep in that PR.
5. `hooks[]` progid overrides are unaffected:
   uiptoolssrv/ulinktools carry progid 2
   (qabotd index, for the `tools` claim) and
   ufaultd/uhangd carry PROG_UIAPP (1) — none
   of those indices move. `PROG_NTP` (2) is
   defined but unused — no impact.
6. Docs: DEVLOG-IPC4, DEVLOG-C6L5, and any
   text naming "linkd index 4 / ulink5d index 5
   / sh index 6" need index updates (docs only).

### 2.4 One-line approval form

If the user later orders "hapus Tier B juga",
execute §2.3 items 1–4 + re-point or drop the
§2.1 legs + full battery. Estimated shape: a
small kernel-adjacent track, not a strip.

## 4. Size

- Baseline (UI-7 tree, pre-strip): kernel
  binary 4,014,648 B, headroom 179,656 B.
- Post-strip (production, QA=0): kernel binary
  2,182,024 B, headroom 2,012,280 B (measured
  reduction 1,832,624 B vs the 1,832,151 B program
  sum; the 473 B delta is alignment/padding from
  the removed objects). Verified: no Tier A/C
  _img.o in obj-md, no Tier A/C symbols in the
  ELF (llvm-nm); Tier B (uipcld, ulink5d) still
  present; linkd 61,604 B (94.0% cap, unchanged);
  uiapp 442,992 B (unchanged).
- QA image (QA=1): kernel binary 2,503,112 B,
  headroom 1,691,192 B. Cross-check: pre-strip
  4,014,648 - Tier C 1,511,528 = 2,503,120,
  vs measured 2,503,112 (8 B alignment).
  llvm-nm confirms all 13 Tier A symbols +
  Tier B present, Tier C absent. Build log
  logs-ui8/qa-strip-build.log ("MD: 43 ok,
  0 failed" + "LINK OK"); kernel preserved
  as logs-ui8/qa-strip-kernel.elf.
- Post-strip + font figures arrive with commit 2.

## 5. Battery

### 5.1 QA image full battery (pre-strip build)

Run 1 (killed mid-FND3 by a VM/runtime restart,
not by a test): UI1 25/25, UI2 38/38, UI3 78/78,
UI4 72/73, UI5 72/73, FND1 PASS, FND2 21/21.
Remainder run: FND3 46/46, FND4 36/36, FND5
33/33, C6L1 29/29, C6L2 36/36, C6L3 72/72,
C6L4 60/60, C6L5 55/55, IPC2 34/34, IPC4 58/58,
IPC5 20/20, ONB1 PASS, ONB2 PASS, SHELL PASS,
SAFEMODE PASS, UI4 rerun 73/73, UI5 rerun
71/73. Standalone UI5: 72/73. Debug UI5 run:
72/73. Full logs: logs-ui8/qa-*.log,
qa-rest-summary.log, qa-ui5-standalone.log,
qa-ui5-dbg.log.

### 5.2 UI5 bound flake: proven harness delivery loss

UI4's run-1 failure ("sleep grant starts the
shell menu", 72/73) was a harness observation
race: every later check passed (Power opened,
Sleep marker, token released), so the menu
demonstrably worked; the rerun passed 73/73.

UI5 failed only the 40-char bound checks in
every run (72/73, 71/73, 72/73 standalone,
72/73 debug). The debug run captured the
serial: after a chunked set that reported
"name saved", bare `setup` printed
"  name: " + 32 A's. The check expects 39
(the shell name buffer holds 40 bytes incl.
NUL, so 39 is the max storable) — 32 = 40
minus exactly one 8-char chunk: chunked
delivery lost a whole chunk to PL011 RX FIFO
overflow while the guest drained slowly under
load, and the shell saved the short name
anyway ("name saved" does not validate
length). A quiet-host probe of the identical
chunked send passed cleanly (39 A's
read back). The detail dark check (> 20)
passes for any longish value and does not
discriminate length — the readback is the
length proof. Verdict: harness delivery
flake (the documented PL011 drain class),
NOT a product regression: storage, display,
and short-name writes all pass in the same
runs, and the code is the UI-7 merge (which
passed 73/73 in its rerun4). The bound
harness gets a retry-until-readback loop in
commit 2 (test-only, same verdict checks).

### 5.3 Production image: smoke + non-Tier-A legs

Chain run (44 min, full logs logs-ui8/
prod-*.log, prod-summary.log): UI1 25/25,
UI2 38/38, UI3 78/78, UI4 73/73, UI5 72/73
(bound readback only, §5.2 class), FND1
PASS, FND2 7/20, FND3 9/45, FND4 36/36,
FND5 33/33, ONB1 PASS, ONB2 finale pair
only, SHELL PASS, SAFEMODE PASS. Smoke
mapping: boot (every leg) + shell (SHELL)
+ GUI menu (UI2) + FAT ls /sd (SHELL) +
littlefs (FND4) — all green.

FND2/FND3 chain failures are NOT a strip
regression: both fail at the same first
step, "web login creates PIN + session",
while their serial legs pass (console
reclaim, fault isolation, spawn codes).
A scratch boot of the production kernel
(probe-weblogin.py, copies of the shared
images, port 18081) logs in cleanly: POST
/login pin=246810 -> 303 + qaon_sess
cookie + Location /settings. So the
production HTTP/PIN path works and the
shared PIN is 246810; the chain failures
are environmental (hostfwd/port contention
or a lingering QEMU at that point in the
chain — the documented chain-load class).
Per the standing rule they were re-run
standalone before any verdict; the rerun
outcome is recorded below.

Standalone reruns on the production image
(build/mach3.elf verified identical to
prod-kernel.elf by cmp): FND2 PASS 21/21,
FND3 PASS 46/46, ONB2 PASS (logs
prod-fnd2-rerun.log, prod-fnd3-rerun.log,
prod-onb2-rerun.log). The production
battery is therefore green except the
UI5 bound readback flake (§5.2).

Tier-A legs cannot run here by design;
they run on the QA image (§5.1). Font
battery arrives with commit 2.

## 6. Honest limitations / follow-ups

- Live radio state wiring for the icons (UI-7
  follow-up, unchanged by the strip; the font
  work in commit 2 keeps it deferred).
- Tier B removal (§2) needs its own decision.
- The structural fix (daemon images from
  littlefs, DEVLOG-UI6 §2) subsumes the strip
  + QA flag; still its own kernel track.
- The UI-6 flake follow-ups stand: `pin_entry`
  per-key marker verification (loss downstream
  of sh); the kernel VM accounting root
  (negative `vm_page_free_count`, DEVLOG-UI5
  §7); FND-3 respawn not covering face; blk
  probe retry/robustness.
- The QA flag keeps Tier A buildable, but the
  production image can no longer run C6L/IPC
  legs: any future leg that needs a Tier A
  program must run on a QA=1 build, by
  construction.
