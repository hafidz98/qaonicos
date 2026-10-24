# Plan: Real Mach IPC Track (IPC)

Date: 2026-10-24. Research: `docs/riset-ipc-mach/
RINGKASAN.md` (read it first — every design decision
below cites it). Mandate: D-ARCH-4 resolution + owner
sequencing, 2026-10-24 (`docs/ARSITEKTUR.md`,
"Resolutions"): real Mach IPC is built as its own
track **after FND (closed) and before C6L**, first
consumers linkd (C6L-5) and the TOOL.DISPATCH hand-off
(C6L-3). Per-service syscall surfaces remain the
pattern for all existing services; IPC is a second,
sanctioned mechanism for daemon-to-daemon traffic.

House rules that bind every phase: own worktree +
branch `dev/ipc<n>`; test-first (the phase's verify
script FAILs pre-change, PASSes post-change);
regression gate = full battery standalone
(`verify-shell.sh`, `verify-safemode.sh`,
`verify-firstboot.sh`, `verify-update.sh`,
`verify-onb1.py`, `verify-onb2.py`, `verify-fnd1.py`
… `verify-fnd5.py`) plus the soak where a phase
touches the scheduler; dev log per phase at
`docs/rencana-ipc/DEVLOG-IPC<n>.md`; coordinator
merges. No changes under `mach3-port/upstream/` —
ever. Expected change surface: `mach3-port/src/ext/
user.c` (+ possibly a new `src/ext/ipc_glue.c`),
`user/ulib/`, test programs under `user/`, `scripts/`,
`docs/`. An MI or MD change beyond this is a
stop-and-surface event (research §10).

## Design decisions (from the research)

- **D-PLAN-1: glue at the parameterised layer, never
  the trap.** Userspace reaches IPC through the
  house syscall dispatch (`user_syscall`); the glue
  calls the Mach object layer with explicit identity
  (`task->itk_space`, `task->map` of the *calling
  daemon*). `mach_msg_trap` is not wired; the MD trap
  ABI stays vestigial (research §2, §6).
- **D-PLAN-2: send never blocks; receive blocks only
  at the extension layer.** Mqueue calls always use
  timeout 0 (`MACH_SEND_TIMED_OUT` /
  `MACH_RCV_TIMED_OUT` are data, not waits). Blocking
  receive (IPC-2) is a scheduler WAITING state with a
  wake condition — the Q9 spawn-wait generalised — plus
  `net_pump()` in its prologue. No user-reachable path
  may call `thread_block` / `thread_will_wait`
  (research §3).
- **D-PLAN-3: inline only, cap 1024 B body, one
  header-carried reply right.** No OOL, no body
  descriptors, no port sets in v1 (research §4, §5.1;
  owner question D-IPC-3).
- **D-PLAN-4: the registry is kernel-resident and
  minimal.** Name → port + generation, fixed-size
  table in the glue, beside the existing arbiters.
  It stores no payloads, runs no policy (research
  §5.2, §8).
- **D-PLAN-5: death is a first-class IPC event.**
  `daemon_reclaim` gains an IPC teardown step;
  queued messages to a dead daemon are discarded,
  its registry entries are generation-bumped, waits
  on it complete with peer-died (research §5.3).
  This lands before any consumer depends on IPC.
- **D-PLAN-6: numbers as reserved, errors as house
  codes.** 10 = `SYS_SEND`, 11 = `SYS_RECV`,
  12 = `SYS_RPC`, 23 = `SYS_RPC_USER` per
  `docs/SYSCALL-ABI.md:134-135`; one new number from
  the free 25–29 block for `SYS_IPC_PORT` management
  sub-ops, ratified in IPC-1 and backfilled into
  SYSCALL-ABI.md in IPC-5 (research §6).

## Interface contracts (existing pieces)

### The daemon scheduler (owner of the wait state)
`mach3-port/src/ext/user.c`: daemon table (NDAEMON 6:
face, uiapp, qabotd, sh, ntp, spawn slot),
`ctx_save`/`ctx_load`, `pick_next()` (state 1 =
runnable only), switch points `sched_yield_switch` /
`sched_exit_switch` — both pump the network. Contract:
IPC phases extend this file's state machine; they do
not add a second scheduler. The spawn slot's WAITING
semantics (state 2, wake restarts `wd_last_progress`)
are the template for IPC waits.

### FND-3 watchdog (liveness accounting)
`wd_supervise()` skips daemons with `state != 1`;
`wd_irq_tick()` is the class-(b) backstop (no
scheduler activity 30 s with a user frame current →
reset). Contract: an IPC-waiting daemon is state 2,
exempt while waiting, progress clock restarted on
wake; waits always carry a deadline once IPC-2 lands;
the all-parked system must reach a kernel-idle state
in which class (b)'s precondition is false. Any phase
changing wait/wake behaviour re-runs verify-fnd3.py
and adds its scenario there or in its own script.

### FND-2 reclaim (death funnel)
`daemon_reclaim(idx)` in `sched_exit_switch` is the
single funnel for both death routes (clean SYS_EXIT,
user_fault). Contract: IPC teardown hooks there and
nowhere else; respawn (`wd_respawn` →
`setup_uprog_task` → fresh task/space) re-registers
from userspace init, not from the kernel.

### qabotd (future RPC server, C6L-3)
Tools live behind the policy gate (`user/qabot/
loop.c` + `policy.c`). Contract recorded for C6L:
TOOL.DISPATCH arrives as an IPC RPC to qabotd's
registered name; the gate is applied exactly as for
local calls. No qabotd change in this track beyond
what IPC-4's test server proves with a dummy tool.

### linkd (future IPC consumer, C6L-5)
Does not exist yet. Contract this track delivers to
it: a frozen `user/ulib/uipc.h` (register / lookup /
send / recv / rpc), a per-port queue-limit knob, and
documented backpressure + peer-death semantics — so
C6L-5 designs linkd's loop against finished semantics
instead of drafts.

## Phases

### IPC-1 — Minimal inline send/receive (poll-only)
Goal: two programs exchange real Mach messages
through the new syscalls; nothing blocks; nothing
else changes.
Scope:
- Glue in user.c (+ `SYS_IPC_PORT` sub-ops ALLOC_RECV,
  DESTROY, SETQLIMIT): resolve names in the caller's
  space, build a kmsg from the caller's buffer
  (`user_range_ok` window check, 1024 B cap),
  `ipc_kmsg_copyin` with the caller's space/map,
  `ipc_mqueue_send` timeout-0; receive mirrors it
  (`ipc_mqueue_receive` timeout-0 + copyout). Send
  rights between test programs are arranged by the
  *test-only* rendezvous: the glue's fixed test entry
  hands the server's send name to the client
  (hard-coded name slot, `#ifdef`-free but reachable
  only via the ALLOC path the test uses — replaced
  by the real registry in IPC-4; the rendezvous code
  is deleted there, not extended).
- ulib: `uipc.h` v0 — `uipc_port_alloc()`,
  `uipc_send()`, `uipc_recv_poll()`.
- Test programs (embed-hook pattern from FND-2/3 —
  production daemon table untouched): a server
  one-shot pair run sequentially via spawn —
  `uipcsrv` (alloc port, publish via rendezvous,
  poll-recv with yield, verify payload + sequence,
  reply on the header reply port, print PASS markers)
  and `uipccli` (resolve, send 100 sequenced
  messages, poll the replies, verify order/content).
  Because one-shots run sequentially, the pair runs
  as two *spawn-slot daemons* under the test harness
  (the uyield/uhangc precedent), not as boot one-shots.
Deliverables: glue + `uipc.h` v0 + `scripts/
verify-ipc1.py` + the two test programs.
Verification (QEMU): pre-change FAIL shape:
`SYS_SEND` returns the dispatch default error, no
IPC markers in serial. Post-change PASS: 100/100
messages round-trip with exact payloads and order;
negative probes asserted — send to a destroyed port
→ invalid-dest code; oversized body → too-large
code; recv on empty queue → empty code (no hang;
the script's global timeout is the tripwire for the
§8 no-`thread_block` rule); full queue → timed-out
code after exactly the queue limit.
Non-goals: no blocking, no registry, no death
semantics, no ABI doc backfill yet.
Exit criteria: verify-ipc1 PASS + battery green;
**and** the headroom reconciliation from research §7
(partition occupancy re-measured with the
update-image tooling; the 287 KB vs ~1.0 MB figures
reconciled in the dev log) — later phases spend
against the reconciled number.

### IPC-2 — Blocking receive (receive as a switch point)
Goal: a daemon can sleep inside `SYS_RECV` until a
message arrives, a deadline passes, or (IPC-3) its
peer dies — without any Mach thread ever blocking.
Scope:
- `SYS_RECV` gains wait semantics (timeout arg:
  0 = poll as IPC-1; >0 = ms deadline; −1 = no
  deadline, allowed but bootlogged). Implementation
  per D-PLAN-2: new daemon wait record (port,
  deadline tick, result slot); wait-switch reuses
  the yield prologue incl. `net_pump()`; the send
  path wakes waiters on the destination port after
  enqueue; the timer path (`wd_irq_tick`'s 100 Hz
  tick or the yield prologue — design in phase)
  expires deadlines. Wake restarts
  `wd_last_progress` (Q9 precedent).
- All-parked idle: when no daemon is runnable, the
  wait completes by idling in kernel context with
  interrupts on until a wake/deadline event —
  class-(b)'s precondition stays false; the idle
  loop still pumps the network on tick.
- ulib: `uipc_recv(timeout_ms)` becomes the default;
  `uipc_recv_poll()` stays for hot loops.
Deliverables: wait machinery + `scripts/
verify-ipc2.py`.
Verification (QEMU): ping-pong between the IPC-1 test
daemons with the server in blocking recv — same
100/100 result, and the server's progress clock
provably exempt while parked (a 60 s park produces
no watchdog warn/hang bootlog events, then a wake
delivers instantly); deadline probe: recv with a
500 ms deadline on a silent port returns timed-out
within tolerance while other daemons keep cycling
(observed via a second test daemon's counter);
all-parked probe: both test daemons parked ≥ 35 s →
no class-(b) reset, bootlog clean, first send wakes
the system; verify-fnd3.py still PASS (46/46 class)
and a new FND-3-adjacent assertion lives in
verify-ipc2.py so the watchdog story is regression-
locked in two places.
Non-goals: death completion (IPC-3) — a wait on a
port whose owner exits mid-phase-2 is completed by
deadline only, and the dev log says so.
Exit criteria: verify-ipc2 PASS + battery + soak
(scheduler touched → soak mandatory).

### IPC-3 — Port lifecycle: death, respawn, queued messages
Goal: daemon death is clean in IPC terms — no
black-holed messages, no corpse ports, no stranded
waiters — integrated with the FND-2/FND-3 ladder.
Scope:
- IPC teardown inside `daemon_reclaim()`: destroy
  the dead daemon's receive ports (queued kmsgs
  discarded by upstream port destruction), drop its
  registry-precursor/rendezvous state (full registry
  arrives IPC-4; here the test rendezvous entries
  die with their owner), record an IPC death note
  for observability (bootlog or counter, phase's
  choice, documented).
- Waiter completion: a daemon blocked in `SYS_RECV`
  awaiting an RPC reply (or in `SYS_RPC`, IPC-4 —
  here: waiting on a port whose *counterparty*
  died) is woken with a peer-died result. Tracking:
  the wait record gains the counterparty daemon idx
  when the wait is an RPC-shaped wait; plain recv
  waits are on the waiter's own ports and need no
  completion (its own death is impossible while it
  waits — it is not running).
- Sender truth: after the owner dies, sends to its
  port return invalid-dest (dead name) — asserted,
  not assumed; if upstream space-entry caching makes
  a stale send *succeed* once, the phase finds and
  fixes the teardown order (destroy before slot is
  marked dead).
- Respawn: a respawned daemon's fresh space +
  re-registration is exercised end-to-end (kill the
  echo server via the FND-3 fault hooks, let the
  supervisor respawn it, client re-resolves and
  completes a round-trip; zero pre-death messages
  delivered post-respawn).
- Explicitly evaluated and recorded in the dev log:
  surgical teardown vs adopting `task_terminate`
  wholesale (research §5.3) — the phase implements
  surgical; the wholesale question is answered with
  evidence either way.
Deliverables: teardown + completion + extended
verify-ipc2 or new `scripts/verify-ipc3.py`.
Verification (QEMU): kill-mid-conversation scenario
(server killed between request and reply → client
wait completes peer-died, no hang, no watchdog
escalation beyond the kill itself); send-after-death
→ invalid-dest on the first attempt; queue-drain
scenario (N messages queued to a daemon that is
then killed → all discarded, memory counters sane,
respawned instance starts with an empty queue);
battery + soak.
Non-goals: registry (IPC-4), policy on names.
Exit criteria: verify-ipc3 PASS + battery + soak +
the FND-3 respawn budget interaction documented
(teardown cost is O(ports + queued), bounded, and
runs inside the existing reclaim funnel's context).

### IPC-4 — Registry + RPC shape; ulib API freeze
Goal: the rendezvous product daemons will actually
use, and the request/reply pattern TOOL.DISPATCH
needs — then freeze the API C6L builds against.
Scope:
- Registry per D-PLAN-4: `SYS_IPC_PORT` sub-ops
  REGISTER (name ≤ 15 chars + receive port) and
  LOOKUP (name → send name in caller's space +
  generation). Fixed table (8 entries — face/uiapp/
  qabotd/sh/linkd/tools + slack); duplicate
  registration by the same owner replaces +
  generation++; by another owner → busy code.
- `SYS_RPC` (12): composed in glue — send request
  with a send-once reply right in the header
  (§4 pattern), then wait (IPC-2 machinery) on the
  caller's reply port with the caller's deadline;
  peer death completes it (IPC-3). `SYS_RPC_USER`
  (23): name-string form for one-shots (resolve +
  RPC in one call; for one-shots the wait is the
  documented bounded poll/yield form).
- Test server: the IPC-1 echo pair re-expressed on
  the registry + RPC (test rendezvous deleted);
  plus a mock "tools" server proving the C6L-3
  shape: request = small JSON-ish blob, reply =
  result blob, CONFIRM/BLOCK classes simulated by
  payload — the real gate stays in qabotd and is
  *not* re-implemented here.
- `uipc.h` v1 **freeze**: register / lookup / send /
  recv / rpc + error code table documented in the
  header. Post-freeze changes are breaking changes.
Deliverables: registry + RPC + `uipc.h` v1 +
`scripts/verify-ipc4.py`.
Verification (QEMU): two clients × one server
interleaved RPCs return correctly attributed
replies (send-once discipline: a second reply to a
consumed right fails); lookup-before-register →
not-found code; register-after-respawn generation
increments and a stale-generation client detects it;
name-table-full probe → busy/full code, no
corruption; battery green.
Non-goals: name access policy (D-IPC-2 recommends
flat v1), port sets, OOL.
Exit criteria: verify-ipc4 PASS + battery; the
`uipc.h` freeze note committed in the dev log and
in the header.

### IPC-5 — C6L hand-off hardening + track close-out
Goal: make the IPC track a finished foundation:
priced, documented, and safe for C6L to build on.
Scope:
- Queue sizing for the link profile: per-port
  SETQLIMIT exercised at linkd's expected load —
  AUD-shaped traffic (1024 B frames, ~31/s, bursts)
  between test daemons at the C6L-4 rates; measure
  drop behaviour at default limit 5 vs raised
  limits; the chosen linkd profile (limit + drop
  policy) written into the hand-off contract below.
- Fan-in verdict: linkd must hear {client requests,
  peer events} — decide with measurements whether
  poll-over-2-ports suffices or port sets get
  exposed as an additive mini-phase *inside C6L-5*
  (not here); the verdict + evidence go in the dev
  log so C6L plans against a decision, not a hope.
- Docs backfill: `docs/SYSCALL-ABI.md` (10/11/12/23
  + the IPC_PORT number marked implemented, with
  semantics and error codes), `docs/ARSITEKTUR.md`
  inventory/verdict rows for IPC (mechanism now
  live; scope note per §8 of the research),
  `docs/ARSITEKTUR-SISTEM.md` component map +
  runtime-flow addition, wiki syscall-abi +
  architecture pages.
- Track close-out dev log: as-built vs plan,
  measured numbers (latency of a blocking wake,
  round-trip cost at 1 KB, image delta), the
  reconciled headroom figure, and the updated
  §"what IPC must not become" compliance check.
Deliverables: sizing evidence + verdict + docs +
`DEVLOG-IPC5.md` (close-out).
Verification (QEMU): full battery + soak; a
link-shaped soak scenario (sustained AUD-rate IPC
traffic for the soak's QEMU-feasible window with
zero unexplained drops).
Non-goals: any C6L code.
Exit criteria: battery + soak green; docs merged;
hand-off contract (below) published in the close-out
log.

## Hand-off points to C6L (exact)

1. **C6L-3 (TOOL.DISPATCH):** the link→qabotd hand-off
   the C6L plan left as "new surface, designed in
   C6L-3" becomes: linkd sends an IPC RPC to the
   name `tools` (registered by qabotd); request/reply
   shapes per IPC-4's mock-tools proof; the policy
   gate applies unchanged inside qabotd. C6L-3's
   scope shrinks to the wire↔RPC translation.
2. **C6L-5 (linkd):** linkd is an IPC server from its
   first commit — registers `link`, serves client
   stubs over `uipc.h` v1 (frozen IPC-4), uses the
   IPC-5 queue profile for the AUD path, and treats
   peer-died + generation as its client-restart
   signals. The C6L-5 daemon-table growth (NDAEMON)
   is unaffected by this track.
3. **C6L-1 (wire freeze):** may proceed in parallel
   with IPC-4/5 — it touches no IPC surface — but the
   client-stub API freeze in C6L waits for `uipc.h`
   v1 (IPC-4 exit).

## Dependency summary

| Phase | Scope in one line | Depends on |
|---|---|---|
| IPC-1 | Poll-only inline send/recv + test pair | FND (closed) |
| IPC-2 | Blocking receive as a switch point | IPC-1 |
| IPC-3 | Death / respawn / queue lifecycle | IPC-2, FND-2/3 |
| IPC-4 | Registry + RPC; `uipc.h` v1 freeze | IPC-3 |
| IPC-5 | Sizing, fan-in verdict, docs, close-out | IPC-4 |

## Open owner questions

From the research (§9), to be answered before the
phase that spends them: **D-IPC-1** (blocking receive
as a sanctioned switch point — before IPC-2),
**D-IPC-2** (registry trust model — before IPC-4),
**D-IPC-3** (1024 B inline cap + OOL deferral —
before IPC-1 freezes its constants). Recommendations
are recorded with each; silence is not assent — the
coordinator surfaces them at phase boundaries.
