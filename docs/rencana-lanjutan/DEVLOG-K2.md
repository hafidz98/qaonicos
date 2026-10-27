# DEVLOG-K2 — qabotd registers the IPC name `tools`

Track: board-less follow-up, item K2 (PLAN.md §K2).
Branch: `dev/k2`. Base: K1 merge (`3a8d47c`, push 264).
Status: implemented, verified end-to-end, battery split PROD/QA.

## 1. The gap K2 closes

`linkd`'s TOOL.DISPATCH forwarding (C6L-5) was verified only
against the `ulinktools` stub, because qabotd never registered
the IPC name `tools`. The kernel reservation already mapped
`tools` → progid 2 (qabotd) under D-IPC-2, enforced by
kernel-assigned spawn identity — but no live owner ever
claimed it, so every peer dispatch on a GUI boot degraded to
`tools_unavailable` unless a test stub happened to be running.

K2 makes qabotd the real `tools` server: peer → linkd →
IPC `tools` (SYS_RPC 12, id 0x3101, 5 s timeout) → qabotd →
policy gate → result back via DISPATCH.RESULT. Nothing in
LINKD-CONTRACT, linkd's forwarding logic, or the
reserved-name enforcement changed; the stub keeps working
and is still used by legs that need it.

## 2. Design

### 2.1 Registration (startup + tick recovery)

At startup qabotd allocates a port and registers it:

```
uipc_port_alloc() + uipc_register("tools", port)
```

Success prints `QABOTD: registered tools`. Failure prints
`QABOTD: FAIL register tools` and the daemon continues
degraded (console-only); the 5 s tick (`QD_TICK_MS`) retries
while the lookup fails. A persistent daemon is never killed
for a registration failure.

Recovery rule (the coexistence contract): the tick
re-registers **only when `uipc_lookup("tools")` fails**.
It never steals a live entry — whether the live owner is
qabotd's own port or a stub's. Consequences:

- Stub alive (same progid 2): the stub's REGISTER replaces
  qabotd's entry (same-owner replace + generation bump per
  IPC-4); qabotd's tick sees a live lookup and does nothing.
- Stub death: IPC-3 teardown clears `alive`; qabotd's tick
  re-registers within ≤ 5 s and prints
  `QABOTD: tools re-registered`. The transient gap is
  contract-correct: dispatches in the gap degrade to
  `tools_unavailable`, exactly as LINKD-CONTRACT specifies
  for an unanswered dispatch.
- IPC4 legs are unaffected: the stub can still steal and
  serve; qabotd never steals a live entry (see §5 for the
  generation-baseline adaptation this forced).

Send-right hygiene (analyzed, no leak): the tick's lookup
returns a send name every 5 s, but Mach send rights
coalesce per (space, port) — repeated lookups return the
same entry. In production (no stubs) the tick holds exactly
one send-right entry for qabotd's own port. Dead-name
entries can only accumulate from stub spawn/death cycles
(test-only, bounded by test activity, not production).

### 2.2 Main loop: blocking readline → poll loop

The old loop blocked in `readline()` on the console, which
could never serve IPC. The new loop is a poll loop that
preserves the observable console behavior exactly (one
`qabot> ` prompt per line, face IDLE at prompt, empty line
= silent):

- Every turn: `uipc_recv(port, &qd_msg, UIPC_MSG_MAX, 100)`
  — park ≤ 100 ms. The park is watchdog-safe (FND-3
  progress stamps stay fed: recv is a non-YIELD syscall,
  same shape as linkd's loop) and never a 30 s spin.
- Console drain: non-blocking chars (SYS_READ_CONSOLE is
  non-blocking; sh owns the console in dev mode via
  SYS_CONSOLE_TAKEOVER, so qabotd's console reads return −1
  whenever sh is the reader — see §6). Newline runs the
  task exactly as before: THINKING → `qb_run_task` with
  `qd_ask_confirm` → SPEAKING → final + status.
- `sys_yield()` each turn.

### 2.3 Dispatch serving (`qd_serve_dispatch`)

One reply per dispatch, never exits. The linkd contract
shapes every decision:

- Request: tool NUL (≤ 63) + args-json NUL (≤ 512), RPC id
  must be 0x3101. Unknown id → `refused` (the stub FAILs
  here; qabotd answers, so a stray id never wedges a peer).
  Malformed body (missing NULs, overlong) → `bad_args`.
- Tool name copied capped at 31 chars: registry names are
  ≤ 10 chars, so truncation cannot fake a tool.
- Args-json parser (loose, documented limits): `"key":number`
  / `"key":"string"` pairs, ≤ 4 args, key ≤ 15, value ≤ 47
  (the QB_ARG caps). Overlong value → `bad_args`, never
  silent truncation. No escape handling in strings;
  unknown value shapes are skipped; negative numbers are
  not parsed (skipped pair → the tool reports a missing
  argument → `tool_error`).
- Policy gate (Q1–Q9, from `tools.c` + `policy.c`):
  unknown tool → `refused`; SAFE → ALLOW → run the real
  `t->exec` (output capped 512) →
  `{"ok":true,"result":"<escaped k=v output>"}` (escapes
  `"` and `\`, drops control/non-ASCII, hard-capped so
  total JSON ≤ ~800 < the 1024 reply budget — the RPC
  length is both send size and reply budget, and a larger
  reply is E2BIG and lost); CONFIRM → `needs_confirm`,
  never executed; BLOCK → `refused`; exec failure (bad /
  missing args, backend failure) → `tool_error`.
- Per-dispatch serial marker:
  `QABOTD: dispatch <tool> <ok|needs_confirm|refused|bad_args|tool_error>`.

Policy behavior per tool class (the verify proves each):

| Class | Example | Peer result |
|---|---|---|
| SAFE (real exec) | get_info, gpio_read | `{"ok":true,"result":"..."}` with real guest values |
| CONFIRM | gpio_write, file_write | `{"ok":false,"error":"needs_confirm"}` — never executed |
| BLOCK | self_destruct, unknown | `{"ok":false,"error":"refused"}` |
| Unanswered / dead | (no server, stub-death gap) | `{"ok":false,"error":"tools_unavailable"}` via DISPATCH.RESULT |

Why CONFIRM is a refusal, not a prompt: qabotd's console
loop is effectively inert in dev mode (sh owns the
console), so a peer dispatch cannot ask on the console
without wedging. The stub precedent (C6L-3 §5) is the same
answer. Interactive confirmation UX is ONB scope, not K2.

### 2.4 What did NOT change

- LINKD-CONTRACT (FROZEN), linkd forwarding, reserved-name
  enforcement, daemon order, SYS_STAT, the Memory guard,
  the (removed) name editor, the icon font, Tier B daemons.
- linkd binary: unchanged, still 61,604 B (94.0% cap).
- The stub: untouched, still QA-only, still serves 4 and
  exits. Its window is now also the coexistence proof.

## 3. verify-k2.py (test-first, 21 checks)

`scripts/verify-k2.py` (LOGDIR `~/workspace/logs-k2/`,
harness copied from verify-c6l5.py) needs the QA=1 image
(`ulinktools` is QA-only). Flow: boot → `qaon>` → `link`
before gui → unavailable → marker link5-test=1 → `gui` →
wait `QABOTD: registered tools` + `LINKD: registered link`
+ `ULINK5D: ready` → Phase 1 (pre-stub snapshot of the
peer transcript; ulink5d iterates TEST.SCRIPT every ~2 s,
each iteration = 4 peer DISPATCH units + 1 EVT):

- qabotd-shaped get_info:
  `RESULT \d+ \{"ok":true,"result":"uptime_ms=` —
  string form, which the stub's object form
  `{"result":{"uptime_ms` does NOT match, so it proves
  the real server.
- qabotd-shaped gpio_read readings ≥ 2 and ALL EQUAL —
  value-agnostic CONFIRM non-execution proof: the scripted
  gpio_write val=1 would flip pin 3 if executed.
- needs_confirm present; refused present (self_destruct);
  NO tools_unavailable in the phase-1 snapshot
  (discriminator: a live consumer serves everything).
- linkd still UP after refused/unknown dispatches.
- Phase 2 (stub window): spawn ulinktools → registered
  tools → done 4 → exit 0.
- Phase 3 (recovery): `QABOTD: tools re-registered`
  ≤ 90 s after stub exit; fresh qabotd-shaped get_info
  RESULT after recovery.
- Teardown: marker cleared, no peer/QEMU left.

### 3.1 Pre-change evidence (K1 kernel, QA build)

K2-VERIFY **FAIL (12/21)** — exactly the designed shape
(log `~/workspace/logs-k2/k2-prechange.log`):

- PASS: boot, link-before, marker, linkd registers,
  ulink5d ready, linkd UP, stub window (started / done 4 /
  exit 0 — the stub works on K1), marker cleared,
  no leftovers.
- FAIL: qabotd registers tools, all phase-1 qabotd-shaped
  checks (get_info arrive, get_info present, gpio equality,
  needs_confirm, refused, NO tools_unavailable), recovery +
  post-recovery.
- The pre-change transcript holds 2,688 tools_unavailable
  lines (every dispatch degraded). Run took 562 s (QA
  build ~65 s + verify ~497 s — absence proofs burn full
  timeouts by design).

### 3.2 Post-change evidence (dev/k2 kernel, QA build)

K2-VERIFY **PASS (21/21)**
(log `~/workspace/logs-k2/k2-postchange.log`).
Serial markers from the run (57 iterations each — the
phase-1 + post-recovery windows):

- 57 × `QABOTD: dispatch get_info ok`
- 57 × `QABOTD: dispatch gpio_read ok`
- 57 × `QABOTD: dispatch gpio_write needs_confirm`
- 57 × `QABOTD: dispatch self_destruct refused`

Real values observed (not stub echoes):

- gpio_read pin 3 → `val=0` in 57/57 results. The mock
  GPIO registers init 0, and the scripted gpio_write val=1
  never executed — otherwise values would flip to 1.
- get_info → real increasing uptime_ms (5900, 7140, …)
  and real `mem=28464/65536KB` from sys_stat.
- 24 tools_unavailable lines in the full transcript, all
  in the stub-death gap (phase-1 snapshot is clean by
  construction; recovery followed) — the contract-correct
  transient, not a regression.

QA binary: 2,525,456 B, headroom 1,668,848 B (QA image is
bigger than PROD because it includes all 13 Tier A
programs — expected).

## 4. C6L5 in the qabotd era

`scripts/verify-c6l5.py` was updated minimally (same class
of change as §5: a test-era premise retired, semantics
preserved):

- Added, after the "linkd registers name link" check:
  `check("qabotd registers name tools", ...)`.
- Replaced the no-tools check (`transcript B shows
  tools_unavailable`) with a qabotd-era check (`transcript B
  shows qabotd-served get_info (string-form result)`).
- Rationale (also in the script header): the no-tools
  leg's premise — no consumer at boot — is exactly what K2
  closes. On the K2 kernel, pre-window iterations are
  served by qabotd, so verbatim C6L5 could never pass that
  check again. The stub window is RETAINED (it still proves
  linkd forwarding + stub coexistence). No-tools
  degradation coverage moved to verify-k2 (pre-change run
  + stub-death transient). No tools_unavailable assertion
  in the post-stub-death gap (timing-dependent: gap ≤ 5 s
  vs ~2 s iteration cadence — flaky by construction).
- Result: C6L5-VERIFY **PASS (56/56)** — 55 + the new
  qabotd-registers check, no-tools leg replaced.

## 5. IPC4 in the qabotd era (generation baseline + stub race)

The first QA-chain IPC4 run failed **44/58** — all 14
failures in the boot-2 flow cascade. Two layered causes,
both in the test program, not the kernel:

1. **Absolute generations.** `user/uipcld.c` asserted
   `gen == 1` for the first lookup, `== 2` after death,
   `== 3` for incarnation 2, and exited on mismatch. On
   the K2 kernel qabotd's GUI-start registration is gen 1,
   so the stub's incarnation 1 observes gen 2.
2. **The deeper race (found on the rerun).** The first
   adaptation (baseline-relative assertions only) still
   failed at the baseline parse — because the client's
   bare `lookup_retry()` succeeds immediately against
   qabotd's live registration at GUI time, before the
   stub ever runs: the client printed its lookup line
   before the script's window opened and sent its echo
   RPCs to qabotd (which refuses unknown ids), so it
   exited before any flow ran. The wake-count and
   flow-time checks then failed for want of a flow; the
   server's deadline-wakes check failed because the
   parked server hit deadlines with no client. Boot 1,
   teardown, and boot 3 all passed; fail_count=0,
   marker 0 — clean state.

Fix (semantics preserved):

- `user/uipcld.c`: a new `wait_stub_gen()` gate before
  phase 1 waits for the generation to reach ≥ 2 — the
  stub's same-owner REGISTER bump past qabotd's gen 1 —
  so no RPC goes out until the stub owns the name
  (wall-clock 120 s via sys_uptime, same shape as
  lookup_retry). Phase 1 records `base_gen` (requires
  ≥ 1); phase 5 requires `base_gen + 1` (death bump);
  phase 6 requires `base_gen + 2` (re-register bump).
  Header comment updated to the gated, baseline-relative
  flow.
- `scripts/verify-ipc4.py`: the three script checks parse
  the baseline N from `UIPCLD: lookup tools gen=N` and
  assert N+1 / N+2. Docstring updated.
- Rerun (QA rebuild with the fix):
  IPC4-VERIFY **PASS (58/58)** — log
  `~/workspace/logs-k2/qa-ipc4-rerun2.log`.
  (The first rerun, on the pre-gate binary, failed the
  same cascade — log `~/workspace/logs-k2/qa-ipc4-rerun.log`
  — which is what exposed cause 2.)

This is the same lesson as §4, one level deeper: when a
real owner claims a name the tests used to own first,
test programs must not only assert relatively — they
must also wait for their intended server to own the
name before speaking. The kernel's bump semantics
(replace → +1, death → +1, re-register → +1) are
unchanged and are now asserted relatively.

## 6. Honest limitations

- **Single-threaded stall.** While a console task runs
  (`qb_run_task`, up to 30 s), IPC is unserved and
  dispatches time out at linkd (5 s) → tools_unavailable
  per contract. Verify dispatches while qabotd is idle at
  prompt. A threaded or queued qabotd is a later track,
  not K2.
- **Dispatch data cap.** Tool args through DISPATCH are
  limited by the QB_ARG caps: a file_write data value is
  ≤ 47 chars. Longer values → `bad_args`.
- **Args parser limits.** No escape handling in JSON
  strings; unknown value shapes skipped; negative numbers
  not parsed (skipped pair → `tool_error` on missing arg).
- **Tick recovery gap.** After stub death, qabotd
  re-registers within ≤ 5 s; dispatches in the gap degrade
  to tools_unavailable (contract-correct, proven in §3.2).
- **Console CONFIRM stays refused.** Peer CONFIRM tools
  answer needs_confirm and never execute; no console
  prompt is attempted (sh owns the console in dev mode).
  Confirm UX is ONB scope.
- **get_info cpu quirk (pre-existing, untouched).** The
  tool output faithfully reports sys_stat, including
  `cpu=4294967295%` (unsigned −1: no idle accounting —
  the same reason the Monitor shows CPU `-`). SYS_STAT
  semantics were deliberately not touched per the task
  constraints.
- **No kernel change.** K2 is userspace-only (`user/
  qabotd.c` + test programs + verify scripts). Daemon
  order, linkd, SYS_STAT, Memory guard, icon font, Tier B
  all untouched.

## 7. Battery (split PROD / QA, per the K1 lesson)

### QA=1 image

| Leg | Result |
|---|---|
| K2 verify | 21/21 (post-change); 12/21 pre-change |
| C6L1 | 29/29 |
| C6L2 | 36/36 |
| C6L3 | 72/72 |
| C6L4 | 60/60 |
| C6L5 (updated) | 56/56 |
| IPC2 | 34/34 |
| IPC4 (adapted) | 58/58 on rerun2 (first run 44/58, gen cascade + stub race, §5; first rerun same 44/58 on the pre-gate binary, which exposed the race) |
| IPC5 | 20/20 |

Note: C6L1–C6L5, IPC2, and IPC5 ran on the pre-gate QA
binary; the gate change touches only `uipcld` (the IPC4
test client, used by no other leg), so their code paths
are identical on the final binary. IPC4 rerun2 ran on the
final QA binary.

### PROD image — chain run

| Leg | Chain | Rerun(s) |
|---|---|---|
| K1 | 7/7 | — |
| UI5 | 73/73 | — |
| UI4 | 71/73 (tz-restore pair) | 73/73 |
| UI3 | 77/78 (teardown PIN restore) | 72/78 (5678-flow per-key loss); rerun2 78/78 |
| UI2 | 32/38 (Wi-Fi quartet + token pair) | 32/38 identical; controlled 38/38; after-UI3 38/38 (see §7.1) |
| UI1 | 25/25 | — |
| FND1 | PASS | — |
| FND2 | 7/20 (web-login cascade) | 21/21 |
| FND3 | 9/45 (web-shell cascade) | 46/46 |
| FND4 | 36/36 | — |
| FND5 | 33/33 | — |
| ONB1 | PASS | — |
| ONB2 | PASS | — |
| SHELL | PASS | — |
| SAFEMODE | PASS | — |

Chain-failure analysis (all are harness/state cascades,
none in K2 product code — K2 touches only qabotd + test
programs):

- **UI3 teardown → PIN poisoning.** UI3's teardown
  `setup pin 246810` failed (single serial attempt under
  chain load), leaving PIN 1234 on disk. Its own product
  checks all passed (77/78); only the restore check
  failed.
- **FND2 7/20, FND3 9/45.** Both root at the same
  poisoning: their first web login with 246810 failed,
  and every web-shell-dependent check cascaded ([blocked:
  no finale] markers in FND2). Their non-web halves
  passed (FND2 console reclaim 7/7; FND3's non-web
  legs). A post-chain probe boot confirmed the PIN is
  back to 246810 (ONB1/ONB2's flows restored it), so the
  reruns need no manual fix. Reruns green (21/21, 46/46).
- **UI4 71/73.** The two tz-restore checks — the known
  UI4 tz-set timeout class under chain load. K2 touches
  no tz code; rerun green (73/73).
- **UI3 rerun1 72/78.** A different signature: the 5678
  round trip failed (per-key loss in the PIN picker —
  the known open follow-up on pin_entry per-key
  verification), cascading through 6 checks. Teardown
  still restored 246810. Chain run had passed all six.
- **UI2 32/38 twice, then 38/38 — see §7.1.**

### 7.1 UI2: state-dependent, not qabotd (A/B + control)

UI2 failed identically twice on K2 (chain + rerun: the
Wi-Fi handoff quartet + token-release pair; the serial
shows the injected Wi-Fi OK followed by the shell's 30 s
idle exit, so the OK never reached the LVGL indev). That
looked like a K2 regression — until the controls:

- **K1 A/B:** scratch worktree at the K1 merge
  (`3a8d47c`), PROD build, UI2 standalone — all 6
  checks PASS (the run died from a runtime service
  restart before its verdict, after 31 PASS / 0 FAIL,
  so no verdict line exists; the 6 checks are the
  evidence). Scripts, lv_shell, lv_port, and face
  sources are byte-identical between K1 and K2 (diff
  verified) — the only userspace difference in play is
  qabotd.c.
- **K2 control:** after resetting the shared disk to a
  known state (`setup reset` + `setup pin 246810`,
  verified NOT provisioned / PIN set), UI2 on the K2
  kernel passes **38/38**.

Conclusion: qabotd does not break UI2. The failures
track chain context, not K2 code — and the follow-up
pair rules out the last state suspect: UI2 run
immediately after a green UI3 run passes 38/38, so
UI3's end state alone does not break it either. Final
UI2 tally on K2: 32/38 (chain), 32/38 (rerun), 38/38
(controlled after reset), 38/38 (after green UI3).
The exact chain-context trigger (host load timing vs a
stale state left by a flaked predecessor leg) is not
isolated, but no K2-code mechanism exists: injected
keys go to `evq` via syscall 105, and only the display
holder pops it (uiapp during the menu); qabotd's
console drain is inert while sh owns the console, and
its IPC footprint when idle is one registry lookup per
5 s plus 100 ms recv parks. Battery lesson carried
forward: a leg that fails twice identically in-chain
still earns a controlled standalone run before any
regression verdict — and a same-score rerun is not, by
itself, proof of determinism when the chain context
itself repeats.

### Headroom (objcopy-binary method)

- PROD (worktree, this branch): binary 2,204,572 B,
  headroom **1,989,732 B** — vs K1 baseline 1,999,216 B:
  K2 costs 9,484 B (qabotd's service code; uipc was
  already linked into ulib.o).
- QA (worktree, final binary): 2,525,472 B, headroom
  1,668,832 B (includes all 13 Tier A programs).
- linkd unchanged: 61,604 B (94.0% cap).

## 8. Files changed

- `user/qabotd.c` (+414/−52) — registration, poll loop,
  dispatch serving, policy-gate replies (the K2
  implementation).
- `scripts/verify-k2.py` — new, 21 checks (test-first).
- `scripts/verify-c6l5.py` — qabotd-era checks (§4).
- `scripts/verify-ipc4.py` + `user/uipcld.c` —
  stub-generation gate + baseline-relative generations
  (§5).
- `docs/rencana-lanjutan/DEVLOG-K2.md` — this file.
