# DEVLOG-IPC5 — Track close-out: sizing, fan-in verdict, documentation backfill

Date: 2026-10-24. Branch: `dev/ipc5` (worktree
`~/workspace/qaonic_os-ipc5`, base = IPC-4 merge `9d1dfb9`).
IPC-5 is a close-out, not a feature phase: the only code
change is a test-only probe program plus its hook; there
is no kernel-behaviour change, no new syscall, no ABI
change. Everything else in this phase is measurement and
documentation.

## 1. What IPC-5 had to answer

The IPC plan's deferred items, carried from IPC-1..IPC-4:

1. Queue-limit sizing at AUD rates (D-IPC-3 consequence:
   IPC-5 budgets 4 KB messages).
2. The port-set fan-in verdict for C6L-5's linkd.
3. Documentation backfill: the IPC surface in
   `docs/SYSCALL-ABI.md`, the D-ARCH-4 supersession in
   the architecture docs in its final form, and the
   wiki digest layer.
4. Track close-out with the exact hand-off to C6L.

## 2. Phase table — the whole IPC track

| Phase | What landed | Verify |
|---|---|---|
| IPC-1 | Glue (`ipc_glue.c`), syscalls 10/11/25, 24-byte header + flat body ≤ 4096 B, test rendezvous | 29/29 — retired at IPC-4; assertions now inside verify-ipc4.py |
| IPC-2 | Blocking `SYS_RECV` (r3 timeout), generalised WAITING as the third switch point, EPARK resume channel | 57/57 → 34/34 post-migration (pair round-trips moved to verify-ipc4.py) |
| IPC-3 | Teardown in `daemon_reclaim` (event 18, EPEER −12), SURGICAL verdict | 30/30 — retired at IPC-4; assertions now inside verify-ipc4.py |
| IPC-4 | Kernel registry (8 entries, generations, reserved `tools`/`link`), SYS_RPC 12 / SYS_RPC_USER 23, mock `tools` server, `uipc.h` v1 FROZEN | 58/58 |
| IPC-5 | Sizing measurement, fan-in verdict, doc backfill, close-out (this log) | 20/20 (`scripts/verify-ipc5.py`) |

No phase after IPC-4 changed kernel behaviour. The
IPC-1/IPC-3 scripts were retired with the rendezvous,
not because their assertions went untested: the
registry + RPC paths they covered are exercised by
verify-ipc4.py's 58 checks and verify-ipc2.py's 34.

## 3. The probe: `user/uipcaud.c`

Test-only embed-hook program ("embed:uipcaud", NOT in
`uprogs[]`), wired into BOTH program loops in
`mach3-port/build-md.sh` plus the hook entry in
`mach3-port/src/ext/user.c`. It self-sends through
register + lookup — the same public path the sh `ipc`
builtins use — paces by `sys_uptime()` (SYS_UPTIME 67,
100 Hz ticks, 10 ms granularity), and prints one
greppable `AUD:` line per phase plus `AUD: DONE`.
Any expectation failure prints `AUD: FAIL <phase>`
and exits 1. One compile fix during wiring: the
helper `fill()` collided with a header symbol and was
renamed `fill_tx()`.

Phases (AUD rate = 16 kHz × 16 bit = 32 KB/s):

- **A — stall-fill at qlimit 16**, 4096 B frames paced
  8/s. Expect fill=16, drained=16, elapsed ~2 s.
- **B — stall-fill at the DEFAULT qlimit** (no
  SETQLIMIT call), 1024 B frames paced ~31/s (the same
  32 KB/s in the C6L plan's wire frame size). Expect
  fill=5 — the default proven by count, not asserted.
- **C — sustained loopback at the default qlimit**,
  4096 B × 240 frames over ~30 s at 8/s, send +
  blocking recv (deadline 2000 ms) per slot. Expect
  recv=240, drops=0, rate 6.0–11.0 frames/s.
- **D — 1 KB round-trip cost**, 200 × send + poll
  recv unpaced. Expect completion; the ms number is
  tick-quantized (10 ms) and is reported as such.
- **E — deadline-park cadence**, 100 × park(10 ms)
  on an empty port. Expect etime=100, ~100 iters/s
  (80–130) — the cadence the fan-in poll-loop
  fallback relies on.

## 4. Test-first evidence, including a real stall

Pre-change baseline (IPC-4 merge kernel, main-tree
build): **IPC5-VERIFY FAIL 7/20** — all 13 probe
assertions fail (no `AUD:` lines; the probe program
and hook do not exist on that kernel), harness legs
pass.

First post-change run: **IPC5-VERIFY FAIL 12/20**.
Phases A and B passed; C/D/E produced nothing for
the full 240 s wait, with no `AUD: FAIL` line and no
kernel fault. Two debug runs (serial sampled every
10 s) showed the same shape: A and B lines, then
silence — and, at ~40 s, a second boot selftest
block, i.e. the guest had rebooted mid-probe.

Root cause (probe design, not glue): FND-3's
class-(b) never-yields detector fires after 30 s
with no scheduler activity while a slot is current,
and phase C spans ~30 s. The probe's pacing loop
busy-polled `sys_uptime()` — syscalls that return to
the same task are NOT scheduler activity, and the
probe never yields, exits, or parks during the
paced wait. At ~30 s the watchdog reset the guest
(the reboot the debug runs caught). The poll-first
theory (blocking recv parking on a queued message)
was checked against `sys_recv_block()` and disproven
before the debug runs: it polls first and returns
immediately on any non-EEMPTY result.

Fix (probe only): `wait_until()` now yields each
iteration (`sys_yield()` waits one 100 Hz tick, so
slot quantization stays 10 ms), and phase C prints
a progress marker every 60 frames so a stall is
diagnosable from serial alone. Final run:
**IPC5-VERIFY PASS 20/20**.

Lesson carried to C6L-5 (recorded here so linkd
does not relearn it): a daemon that spins without a
switch for 30 s is reset, however many syscalls it
makes. A parked linkd (blocking recv) is safe —
parks ARE switches. A spinning linkd is not: its
wait loops must yield or park.

## 5. Sizing: measured numbers and the default decision

Measured (worktree kernel, `AUD:` lines):

| Phase | Measured |
|---|---|
| A (qlimit 16, 4096 B, 8/s) | fill=16, elapsed_ms=2000, drained=16 |
| B (default, 1024 B, ~31/s) | fill=5, elapsed_ms=160 |
| C (default, 4096 B, 240 frames) | sent=240 recv=240 drops=0 elapsed_ms=29880 rate 8.0 frames/s |
| D (200 × 1 KB round trips) | elapsed_ms=90, 450 µs/round-trip (tick-quantized: 90 ms / 200) |
| E (100 × park 10 ms) | etime=100, elapsed_ms=1000, 100 iters/s |

Derived (v1 cap 4096 B, header 24 B, wire total
≤ 4120 B):

- Per-port kernel memory ceiling at qlimit 16:
  16 × 4120 = **65,920 B (~64.4 KiB)**.
- Per-port ceiling at the default qlimit 5:
  5 × 4120 = **20,600 B**.
- Per-system ceiling = Σ over live ports of
  (qlimit × 4120 B). Anchor: all 8 registry ports
  at max 16 → 8 × 65,920 = **527,360 B (~515 KiB)**,
  still small against 64 MB RAM, but 3.2× the
  default budget per port.
- Stall absorption: at 4096 B frames a 16-deep
  queue fills in 2.0 s stalled (measured A); a
  5-deep queue fills in 0.625 s (5 × 125 ms; the
  160 ms measured at 1024 B frames is the same
  arithmetic in smaller frames). The sender on a
  full queue observes **EFULL (−4)**; sends never
  block — poll semantics, by design.
- Round-trip cost is sub-millisecond (~450 µs
  quantized); the park cadence is exactly 100/s
  at a 10 ms deadline.

**DEFAULT DECISION: keep the default qlimit at 5.**
Reasons, each with a number:

1. A matched-rate pair needs no queue depth at all:
   phase C sustained 240/240 with zero drops over
   ~30 s *through the default 5-deep queue* at full
   4096 B frames. The queue absorbs bursts and
   stalls, not steady state.
2. 625 ms of stall absorption at full frames
   exceeds any scheduling gap in the cooperative
   scheduler (a daemon's stall is bounded by its
   own yields; a linkd RPC round-trip is
   sub-millisecond). A consumer stalled longer
   than that is wedged, and FND-3 already kills
   wedged daemons — buffering 2 s of audio for a
   dead consumer is waste, not robustness.
3. Raising the default would multiply per-port
   kernel memory 3.2× (20,600 → 65,920 B) for
   every port in the system, for a case no
   measurement justifies.
4. The per-port override already exists and is
   proven: `SETQLIMIT` (phase A) lets any port
   with a *measured* need raise itself to 16.
   C6L-5 may raise linkd's AUD port to 16 for
   2 s of stall absorption at a known cost of
   66 KB for that port — that is a C6L-5
   decision with a number attached, not a new
   default.

## 6. Fan-in verdict: port-set DEFERRED

**Verdict: v1 does not need a port-set.** The
per-port blocking receive + the registry is
sufficient for C6L-5's linkd, for these reasons:

- Linkd's fan-in collapses to one queue: clients
  send RPCs to the registered name `link`, and
  UART peer events arrive as messages on the same
  queue. The IPC-4 mock `tools` server is the
  reference shape — one port, one loop, dispatch
  on message id (echo `0x3000+s`, structured
  dispatch `0x3100`).
- The one case a port-set would serve — two
  genuinely separate queues (e.g. AUD data vs
  control/RPC) — has a documented fallback that
  is measured, not hypothetical: block-with-
  deadline on one port + poll the other per loop
  iteration. Phase E measures that cadence at
  exactly 100/s with a 10 ms deadline, which is
  finer than any control-plane latency the link
  needs.

**C6L-5 consequence (exact):** linkd multiplexes
on a single port from its first commit. If linkd
later needs two ports (the named trigger:
separating AUD data from control/RPC to avoid
head-of-line blocking behind 16 AUD frames), it
uses the block-with-deadline + poll fallback
above — no IPC change required. The cost of no
port-set, stated plainly:

- One shared queue depth budget per port
  (16 × 4120 B ≈ 66 KB at max qlimit).
- FIFO head-of-line latency between AUD frames
  and RPCs on that queue. In steady state this
  is ~0 (phase C proves the queue stays
  near-empty at matched rates); in the
  pathological stall case an RPC queued behind
  a full 16-frame AUD queue waits up to
  16 × 125 ms = 2 s. That bound is the price
  of the single-port design, and it is the
  reason the two-port fallback exists.

Port-set is not rejected forever: it is deferred
until a consumer measures head-of-line latency
that the fallback cannot absorb. No port-set
code was written in this phase, per the plan.

## 7. Headroom, re-measured (and a correction)

Method (IPC-1's reconciled method, unchanged):
raw `llvm-objcopy-18 -O binary` size vs the
4,194,304 B boot partition; bss is RAM-only and
never counted.

| Build | Raw | Headroom |
|---|---|---|
| IPC-4 merge, fresh build (`9d1dfb9`) | 3,243,464 B | **950,840 B** |
| IPC-5 worktree (IPC-4 + probe) | 3,261,580 B | **932,724 B** |

IPC-5's own delta is +18,116 B — the probe
program, its 4 KB frame buffer, and the hook.
No kernel-behaviour code was added, so no other
growth is attributable to this phase.

**Correction:** DEVLOG-IPC4's figure (880,888 B)
does NOT reproduce from a fresh build of the
IPC-4 merge commit. The build directory it was
measured from contained 9 stale objects from
retired IPC-1..3 test legs (`uipccli`,
`uipcdclean`, `uipcdfault`, `uipcdrain`,
`uipcdsetup`, `uipcdwedge`, `uipcsrv`, `uipkcli`,
`uipksrv`) that the link step glob-picks up —
the main tree's `mach3-port/build/` still
contains them (build/ is gitignored, so the repo
is clean; only local build dirs are affected).
The fresh-build figures above supersede the
IPC-4 figure. Follow-up: build hygiene (see §10).

## 8. Documentation backfill (this phase)

- `docs/SYSCALL-ABI.md` — new IPC section: syscalls
  10/11/12/23/25 with arguments, timeout encoding,
  registry semantics (8 entries, generations,
  reserved names, eviction restarts at generation
  1, EDEST on dead-owner lookup), port lifecycle
  (IPC-3, event 18, surgical teardown, task /
  vm_map / pmap leak still bounded), full error
  table −1..−13 (−2 never reused, −11 EPARK
  internal only). The reserved-numbers table now
  states nothing is reserved (IPC numbers
  implemented; future numbers by append, principle
  P2). A new "implemented but not yet backfilled"
  table lists 27 further numbers by their verified
  `user.c` define names only (78, 79/80, 82–88,
  89–92, 93–97, 98 subs 0–11, 99, 100/101, 102,
  103, 104, 105) — semantics NOT restated, pointed
  at their track plans/dev logs. Older sections
  predate the English-artifact rule and are
  pending translation (noted in the doc).
- `docs/ARSITEKTUR.md` — §1 model paragraph (IPC
  ports live via `ipc_glue.c`); "only crossing /
  no message passing" bullet (message passing is
  the second sanctioned mechanism; per-service
  pattern remains for existing services);
  "poll-shaped / no blocking syscalls" bullet
  (blocking `SYS_RECV` is the one sanctioned
  exception per D-IPC-1, third switch point);
  net_pump paragraph (four call sites, verified
  by grep, at yield/exit/IPC-wait switch points);
  littlefs §4.3 verdict (D-ARCH-4 reversed the
  IPC premise, so an FS server is now *possible*,
  but KEEP stands on the R5 ground alone; no
  FS-server decomposition planned); `user.c`
  line-count claim (2,270 → 3,475 lines at IPC-5;
  `ipc_glue.c` 827 lines); both ~287 KB headroom
  claims replaced with the IPC-5 re-measured
  figure + retraction note.
- `docs/ARSITEKTUR-SISTEM.md` — component map:
  littlefs row corrected (persistent since FND-4,
  not "FND-4 makes it persistent"); new Mach IPC
  row (registry + RPC, cap, qlimits, reserved
  names, frozen v1, third switch point); §8
  forward table: IPC row added, C6L row updated
  (Q-A3 answered, not pending), committed order
  updated (FND → IPC → C6L → UI); §9 final-form
  close-out paragraph (track DONE, contract
  frozen, remaining open questions Q-A2, Q-A4..Q-A7).
- `wiki/docs/syscall-abi.md` — staleness warning
  rewritten (IPC surface backfilled, Table 2's
  78–105 still pending); key facts (poll-shaped
  + one blocking form; IPC live with numbers,
  cap, qlimits, reserved names, frozen v1);
  IPC row added to Table 1; "28-row backlog"
  language replaced with the Table 2 backlog.
- `wiki/docs/kernel-userspace-boundary.md` — key
  facts (Mach IPC live; two mechanisms; one
  blocking form; headroom re-measured each
  track); R13 (re-measured figure).
- `wiki/docs/architecture.md` — key facts (message
  passing as second mechanism; three switch
  points; headroom re-measured); "what Mach
  provides" paragraph (IPC numbers live).
- `wiki/docs/glossary.md` — Daemon row (daemons
  are not IPC servers by virtue of being daemons,
  but qabotd serves `tools` and linkd will serve
  `link`); Cooperative-scheduling row (three
  switch points); Net-pump row (switch points);
  Embed row (re-measured headroom).
- `wiki/docs/display-ui-face.md`,
  `wiki/docs/tracks-roadmap.md`,
  `wiki/docs/networking-c6.md` — ~287 KB claims
  replaced with the IPC-5 re-measured figure +
  retraction note; "two switch points" corrected
  to the switch points (yield, exit, IPC-wait
  prologue).
- `docs/rencana-fondasi-tanpa-board/PLAN.md` —
  UI-1 go/no-go line corrected to the re-measured
  headroom.
- Grep audit (this phase, docs/ + wiki/): no
  remaining stale claim outside historical
  records. Historical `riset-*` research docs,
  DEVLOG-IPC1..4, and the IPC plan's IPC-1..4
  sections are intentionally untouched — dated
  snapshots; their as-of claims stand as history.
  The `1024 B frames` mention in the C6L plan is
  the wire AUD frame size, NOT the IPC cap, and
  is not stale. The two remaining "~287 KB"
  mentions in `docs/ARSITEKTUR.md` are the
  retraction sentences themselves.

## 9. "What IPC must not become" — compliance check

From the plan's guardrails, checked at close-out:

- IPC did not swallow existing services: no
  per-service surface was migrated; the registry
  holds exactly two reserved names (`tools`,
  `link`) and open names for future servers.
- No out-of-line memory: cap stays 4096 B
  inline; OOL remains deferred with no consumer.
- No port-set: deferred with a measured
  fallback (§6), not smuggled in.
- No new blocking form: blocking `SYS_RECV`
  remains the only one (D-IPC-1); sends never
  block.
- No new leak class: teardown is surgical and
  proven (IPC-3); the remaining task-object leak
  is pre-existing and bounded (§10).
- The kernel registry is 8 entries by design:
  names are a rendezvous mechanism, not a
  namespace — clients that need more names
  multiplex on one registered port.

## 10. Backlog carried forward (owners)

- **ramfs fd-table leak** (IPC-1 finding: fd-table
  slots keyed by task, never reclaimed,
  `RAMFS_MAX_TASKS` 16, no hook in
  `daemon_reclaim`). Owner: the next track that
  touches daemon lifecycle (FND-2-family
  follow-up); not IPC — no IPC code path touches
  ramfs slots.
- **task / vm_map / pmap leak on daemon death**
  (IPC-3 SURGICAL verdict: destroying all rights
  achieves every IPC-visible effect of
  `task_terminate`; freeing the task struct needs
  a `thread_terminate` audit on never-scheduled
  multiplexed threads). Owner: same lifecycle
  follow-up; bounded meanwhile by the FND-3
  respawn budget (2/daemon/boot).
- **Registry generation restart on eviction**
  (IPC-4 contract: an evicted dead entry
  re-registered later restarts at generation 1).
  Owner: **C6L-5** — linkd clients must treat
  generation as "changed since last lookup", not
  a monotonic clock across eviction, and must
  retry lookup on EDEST across the respawn
  window.
- **Build-dir stale objects** (§7 finding: retired
  program objects glob-linked into fresh binaries;
  ~70 KB measurement error at IPC-4). Owner:
  the next track that touches `build-md.sh` —
  clean stale program/`_img` objects or link from
  an explicit list. Not fixed in IPC-5 (minimal-
  change close-out rule; IPC-5's own measurements
  used a fresh worktree build).
- **SYSCALL-ABI Table 2 backfill (78–105)**.
  Owner: docs backlog, not IPC. IPC's own surface
  is complete; the 27 remaining numbers are listed
  by define name only in SYSCALL-ABI.md, honestly
  flagged, semantics not invented.

## 11. Track close-out and hand-off to C6L

**The IPC track is DONE.** IPC-1..IPC-5 are
implemented, verified, and documented. The hand-off
to C6L is exact:

- **Frozen contract:** `user/ulib/uipc.h` v1 —
  24-byte header, flat body ≤ 4096 B, total
  ≤ 4120 B, wire rounded to a multiple of 4 with
  zero padding, COMPLEX never set; syscalls 10
  SEND, 11 RECV (r3: 0 = poll, >0 = deadline ms,
  −1 = no deadline, bootlogged), 12 RPC, 23
  RPC_USER, 25 IPC_PORT (subs 0 ALLOC_RECV, 1
  DESTROY, 2 SETQLIMIT 1..16, 3 REGISTER, 4
  LOOKUP, 5 REG_GEN); errors −1..−13 (−2 never
  reused, −11 EPARK internal only); reply ids
  must never be 71 (Mach send-once death notice
  → EPEER).
- **Names:** `tools` — served by qabotd (kernel-
  assigned progid 2; non-owner claim → EPERM
  −13). `link` — reserved, unbound until C6L-5
  binds linkd's index; any claim refused until
  then.
- **Mock server dispatch shape (C6L-3's
  reference):** id `0x3000+s` echo (body XOR
  0xA5); id `0x3100` structured dispatch; id
  `0x3200` double-reply probe (second reply
  fails); id `0x3300` die without replying
  (client gets EPEER); id `0x3400` reply OK then
  exit. C6L-3's TOOL.DISPATCH is an IPC RPC to
  `tools` in this shape.
- **Queue profile for linkd (from §5):** default
  qlimit 5; may raise its AUD port to 16 via
  SETQLIMIT for 2 s of stall absorption at 66 KB
  for that port. Sustained AUD-rate traffic
  (8 × 4096 B/s) runs zero-drop through a
  5-deep queue for 30 s (phase C).
- **Fan-in (from §6):** single-port multiplex;
  block-with-deadline + poll fallback at a
  measured 100/s cadence if a second port is
  ever needed. No port-set.
- **Watchdog rule for linkd (from §4):** park or
  yield — never spin 30 s without a switch.
- **Client-restart signals:** peer death → EPEER
  (or the id-71 notice mapped to EPEER);
  dead-owner lookup → EDEST; generation restart
  on eviction (§10).

Files changed in this phase (worktree `dev/ipc5`):

- `user/uipcaud.c` (new, test-only), `user/ulib`
  untouched (probe uses the frozen `uipc.h` API),
  `mach3-port/build-md.sh` (uipcaud in both
  program loops), `mach3-port/src/ext/user.c`
  (embed-hook entry only).
- Docs: `docs/SYSCALL-ABI.md`,
  `docs/ARSITEKTUR.md`, `docs/ARSITEKTUR-SISTEM.md`,
  `docs/rencana-fondasi-tanpa-board/PLAN.md`,
  `wiki/docs/syscall-abi.md`,
  `wiki/docs/kernel-userspace-boundary.md`,
  `wiki/docs/architecture.md`,
  `wiki/docs/glossary.md`,
  `wiki/docs/display-ui-face.md`,
  `wiki/docs/tracks-roadmap.md`,
  `wiki/docs/networking-c6.md`,
  `docs/rencana-ipc/DEVLOG-IPC5.md` (this log).
- `scripts/verify-ipc5.py` (new, 20 checks).
- Upstream untouched. Main tree untouched. No
  merge, no push, no watchdog-registry touch
  (the coordinator closes `track-ipc` after merge).

## 12. Battery + soak results

Battery, standalone in order (worktree disk,
post-IPC-5 kernel — IPC-5 changed no kernel
behaviour, so every leg below also re-validates
the IPC-4 kernel):

| Leg | Result |
|---|---|
| SHELL (`verify-shell.sh`) | PASS |
| SAFEMODE (`verify-safemode.sh`) | PASS |
| FIRSTBOOT (`verify-firstboot.sh`) | PASS standalone after a `setup reset` boot. In battery order it failed first (boot 1 requires an unprovisioned disk; the IPC-4 verify leg's web /settings POST had provisioned it) — the known precondition sensitivity from IPC-4, not a regression. The reset boot reported "provisioning cleared", fail_count=0; the standalone re-run passed. |
| UPDATE (`verify-update.sh`) | PASS |
| ONB1 (`verify-onb1.py`) | PASS |
| ONB2 (`verify-onb2.py`) | PASS (battery-order output was truncated by the harness's tail window plus host nanobind noise; standalone re-run passes cleanly) |
| FND1 (`verify-fnd1.py`) | PASS standalone. In battery order it failed one check ("bootlog has ev=12 'software reset' for the requesting boot") — the 16-record bootlog ring rotated the ev=12 record out before the dump in battery-order state; standalone it passes. Same class as the documented battery-order flakes (IPC-2 battery: FND-3 45/46 on an SD/fat one-shot timing flake), not an IPC-5 regression: IPC-5 changed no kernel code path the check touches. |
| FND2 (`verify-fnd2.py`) | PASS (21/21 checks) |
| FND3 (`verify-fnd3.py`) | PASS (46/46 checks) |
| FND4 (`verify-fnd4.py`) | PASS (36/36) |
| FND5 (`verify-fnd5.py`) | PASS (33/33) |
| IPC2 (`verify-ipc2.py`) | PASS (34/34 checks) |
| IPC4 (`verify-ipc4.py`) | PASS (58/58 checks) |
| IPC5 (`verify-ipc5.py`) | PASS (20/20 checks), twice: standalone and as the battery tail |

Soak (`scripts/soak-test.py`, fresh log — the
previous `logs/soak-results.log` was archived to
`logs/soak-results-pre-ipc5.log` first, per the
resume trap): **SOAK: PASS, fails=0** — 100/100
cold boots reaching the shell prompt with the core
PASS markers and no panic/FAULT, 30/30 random
power cuts (SIGKILL mid-boot), final clean boot
with littlefs + bootstate intact. Link-shaped
scenario note: the standard soak proves
boot/power-cut stability; the sustained AUD-rate
IPC traffic scenario at QEMU-feasible scale is
phase C of verify-ipc5.py (240 frames @ 8 × 4096
B/s, zero drops over ~30 s), which passed as the
battery tail. A soak-length IPC traffic run is not
meaningful in QEMU beyond that window — the 30 s
window already exceeds any single
cooperative-scheduler stall budget by an order
of magnitude, and longer runs would only repeat
the same loop.
