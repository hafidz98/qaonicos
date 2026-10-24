# DEVLOG-IPC3 — port lifecycle on daemon death (IPC-3)

- Track: IPC (`docs/rencana-ipc/PLAN.md`, IPC-3 section + Resolutions D-IPC-1..3)
- Branch: `dev/ipc3` (worktree; coordinator merges, never pushed from here)
- Status: **DONE — verify-ipc3 PASS 30/30, battery green (§6), soak PASS (§7)**
- Predecessors: IPC-1 (poll IPC, `DEVLOG-IPC1.md`), IPC-2 (blocking receive,
  `DEVLOG-IPC2.md`); FND-2 (`daemon_reclaim`) and FND-3 (supervisor) are the
  funnel and the kill path this phase hooks into.

## 1. Problem

IPC-1/2 left every death leaking: a daemon's `ipc_space` was never torn down
(no `task_terminate` anywhere in `src/ext`), so its receive ports stayed ACTIVE
in a never-freed space. A sender holding a send right to a dead daemon's port
queued messages on a corpse port forever — no error, no wake, no bound. A
client parked in a blocking receive on a reply port whose server died had no
completion at all (deadline or nothing). IPC-3 closes both: death destroys
ports, senders get dead-name on the next send, and RPC waiters complete with a
peer-died verdict.

## 2. Design as built

### 2.1 Teardown inside `daemon_reclaim` (the funnel)

`daemon_reclaim(idx)` in `mach3-port/src/ext/user.c` — the single funnel both
death routes share (clean `SYS_EXIT` via `sched_exit_switch`, `user_fault`
isolation, and the watchdog kill path in `wd_supervise`) — gains an IPC part
that runs FIRST, before the display/console/TCP items:

1. The dying slot's own peer state is released and cleared
   (`ipc_peer_clear_slot`): its pending peer ref, its wait-record peer ref,
   and the wait record itself. This covers the woken-but-not-redispatched
   case (state 1 with the record still open) and guarantees the dying daemon
   cannot be woken by its own port deaths.
2. `ipc_glue_teardown(d->task, &ports, &msgs)` destroys every right in the
   dead task's space (see §2.2), then a bootlog death note is written
   (event **18**, `ipc dead <name> <p>p <m>m`, e.g. `ipc dead spawned 1p 1m`).

Ordering guarantees (verified in code, not assumed): reclaim runs BEFORE the
slot is marked dead in both `sched_exit_switch` and `wd_supervise`, and BEFORE
`wd_respawn` replaces `d->task` in the watchdog path — so teardown always
sees the dying task, and waiter completion lands before/while the respawn
lands. A respawned incarnation gets a fresh task (hence a fresh space) from
`setup_uprog_task`, so it starts with a clean port table by construction.
Spawn-slot setup also clears the new fields explicitly (defense in depth:
reclaim clearing covers the funnel, setup clearing covers any future path
that skips reclaim).

The rendezvous table is deliberately NOT touched: RV slots are owned by the
table, not the publisher — publish uses MOVE_RECEIVE, transferring ownership
at publish time, and IPC-1's publish-then-exit flow REQUIRES slot survival
across the publisher's death. The plan's loose phrase "rendezvous entries die
with their owner" is therefore NOT implemented literally; the
respawn-republish EBUSY case is IPC-4's registry problem (REGISTER replaces +
generation++), recorded here so IPC-4 does not have to rediscover it.

### 2.2 Teardown algorithm (`ipc_glue_teardown`, `ipc_glue.c`)

Two passes over the dead task's `itk_space->is_table` (names reconstructed as
`MACH_PORT_MAKEB(index, ie_bits)`):

- **Pass 1 — receive rights.** Destroying one kills its port: upstream
  `ipc_port_destroy` discards the queue (counted first, by a bounded walk of
  the circular kmsg list via `ikm_next`, bound 4096 iterations) and generates
  dead-name notifications for registered requesters (none in our flows).
  Receive-first ordering is what makes senders go dead immediately. After
  each destroy, `sched_ipc_wake_peer(object)` wakes waiters annotated with
  that port (see §2.4). The object is pinned with an extra reference across
  the destroy so the wake comparison cannot see a recycled pointer.
- **Pass 2 — everything else** (send, send-once, dead-name, port-set entries).
  Every entry still pointing at an object holds a reference on it, so
  destroying these cannot touch a freed port.

Single-threaded trap context means no other task can mutate this space while
teardown runs, so the walk needs no extra locking (`mach_port_destroy`
handles space locking internally); destroying an entry never shrinks the
table or moves other entries, so iterating by index while destroying is safe.
Documented limitation: if `is_tree_total != 0`, a warning is printed — our
spaces never grow a tree (a handful of sequential allocations in the
128-entry initial table); if the warning ever fires, teardown is incomplete
by exactly the tree entries.

The task object, its `vm_map` and its `pmap` are NOT freed: that is the
pre-existing task leak, unchanged by IPC-3 (see the surgical decision, §3).

### 2.3 Peer annotation (the RPC shape)

A waiter parks on its OWN receive port, so "the port whose owner dies" can
only mean the RPC counterparty: a client parked on its reply port waiting for
a server that dies. Plain own-port waits need no completion (the waiter's own
death is impossible while it waits — it is not running). This matches the
plan's own wording ("waiting on a port whose counterparty died").

Mechanism (chosen over task-slot idx and over implicit last-sender
heuristics):

- At send time, `SYS_SEND` now goes through `ipc_glue_send_ex`, which
  reports the destination port OBJECT (from the kmsg header's remote-port
  field after copyin, as IPC-2 already captured it for wake) and the reply
  port NAME. On a successful reply-carrying COPY_SEND (not a send-once
  reply), the dispatch (re)sets the pending peer: the old ref is released
  first, a Mach reference is taken on the destination object
  (`ipc_object_reference`) and stored with the reply name. A failed send
  stages nothing and touches nothing; a no-reply send leaves a pending peer
  untouched for a later park on its reply port. The glue itself takes no
  reference — the dispatch takes it when it stores, in the same trap context,
  so nothing can free the object in between.
- At park-open, `sys_recv_block` adopts the pending peer into the wait record
  ONLY if the parked port name equals the pending reply port name (exact
  match, generation bits included — a stale name never matches). Parking on
  any other port leaves the pending untouched for a later park on the reply
  port. A second request replaces the pending (old ref released).
- Park-open liveness: if the adopted peer object is no longer
  `IO_BITS_ACTIVE` (cleared by `ipc_port_destroy`), the wait returns
  `UIPC_EPEER` immediately WITHOUT parking. This closes the no-deadline
  strand: a peer already dead at park time could otherwise strand a
  no-deadline waiter forever.
- Ref release rules (all must hold or refs leak): at wait resolution
  (message delivered, deadline ETIME, peer-died EPEER), at pending
  replacement, and at slot cleanup in reclaim. The wait-record ref and the
  pending ref are never both held for the same wait (transfer at park-open).

v1 limitation, stated plainly: only the MOST RECENT outstanding
reply-carrying request is tracked. Parking on an older reply port after a
newer request is unannotated (its deadline still bounds it). The normal flow
(request → park → reply → next request) is always annotated; generalising to
multiple outstanding RPCs is IPC-4's RPC work if it wants it.

### 2.4 Peer-death wake + verdict order

`ipc_glue_teardown` (pass 1) and the `IPCP_DESTROY` hook (below) call
`sched_ipc_wake_peer(object)`, which sets `ipc_wait_peer_died = 1` + state 1 +
progress stamp on every waiter whose `ipc_wait_peer_obj` matches. The verdict
is decided in the waiter's OWN context on re-dispatch, in this order
(`sys_recv_block`, after the poll-first that IPC-2 established):

1. queued message (poll) — beats everything, so a message racing a wake is
   never lost;
2. peer-died → `UIPC_EPEER` (if both peer-death and deadline happened, EPEER
   is the truthful verdict — the wait can never succeed);
3. deadline → `UIPC_ETIME`;
4. otherwise park again under the same record.

No copyout ever happens in a waker's context (the IPC-2 rule, preserved).

`IPCP_DESTROY` hook: a live daemon destroying its own receive port is a peer
death for waiters annotated with that port (the shutdown case). The hook
resolves the name first (`ipc_mqueue_copyin`, which takes a reference that
pins the object across the destroy), destroys, then wakes peers ONLY if the
name really was a receive right (checked by table lookup: index in range,
generation match, type `MACH_PORT_TYPE_RECEIVE`). Destroying a send name
kills nothing, and waking on it would lie to a waiter; unresolvable names
(send names to dead ports, dead names, port sets) keep the old behaviour.

### 2.5 Codes (user/ulib/uipc.h, mirrored by hand in ipc_glue.h)

| Code | Value | Meaning |
|---|---|---|
| `UIPC_EDEST` | −3 | send to a destroyed / dead port (dead name); also the resolve/destroy failure for a bad name |
| `UIPC_EPEER` | −12 (NEW) | RPC waiter completed because the annotated peer port was destroyed; distinct from `UIPC_ETIME` (−10) |
| `UIPC_ETIME` | −10 | blocking-receive deadline expired (IPC-2, unchanged) |
| `UIPC_EPARK` | −11 | internal park/wake resume code (IPC-2, unchanged; never returned to userspace — the ulib wrapper re-issues) |
| `UIPC_EEMPTY` / `UIPC_EFULL` / `UIPC_E2BIG` | −5 / −4 / −6 | unchanged IPC-1 poll semantics |

Send-after-death is `UIPC_EDEST` on the FIRST attempt: receive-right
destruction deallocates/inactivates the port, so the next send's copyin
detects a dead destination (`MACH_SEND_INVALID_DEST` → `map_send_err` →
`UIPC_EDEST`). No registered dead-name requesters exist in our flows, so
there are no notification side effects.

## 3. Surgical vs `task_terminate`: decision with evidence

**Decision: SURGICAL** (destroy rights in the dead task's space; do not adopt
`task_terminate` wholesale).

Evidence:

- Destroying all rights in the space via `mach_port_destroy` per entry
  achieves every IPC-visible effect of `task_terminate`: the space is empty,
  senders go dead on next send (proven: P1/P2/P3 send-after-death → EDEST on
  the first attempt), queued messages are discarded (proven: P4 drain EMPTY
  after deaths; teardown counts `1p 1m` per death in the bootlog), and
  peer-annotated waiters complete (proven: P5).
- What `task_terminate` would additionally do — free the task struct, its
  `vm_map` and its `pmap` — is a separate memory-hygiene project with its
  own risks: `thread_terminate` behaviour on never-scheduled multiplexed
  threads (every daemon's Mach thread was created but never runs under the
  extension scheduler) is unproven, and freeing a `vm_map`/`pmap` that the
  extension layer still references through `cur_utask`/`daemons[].task`
  would need its own audit. Nothing in IPC-3's scope requires it.
- The task-object leak is pre-existing and UNCHANGED by IPC-3 (FND-3's dev
  log already deferred VM destruction; persistent-daemon leak is bounded by
  `WD_MAX_RESPAWN = 2`/boot; spawn-child task leak per run pre-exists).
  IPC-3 strictly REDUCES the net leak per death by freeing ports + queued
  kmsgs, which previously leaked too.
- Cost of the surgical path is bounded and runs inside the existing reclaim
  funnel's context: O(ports + queued messages) per death, no new switch
  point, no new syscall, no ABI change for healthy daemons.

Consequence carried forward: a future phase that wants task-object reclamation
should do it as its own project (audit `thread_terminate` on multiplexed
threads + extension-layer task references first), not as a silent IPC-3
extension. The respawn's fresh-space cost is already paid today
(`setup_uprog_task` builds a fresh task/pmap/ctx per respawn) and is unchanged.

## 4. Harness: two findings that reshaped the test

### 4.1 A spawn child cannot spawn (test redesign)

The first driver design (`user/uipcd.c`: a spawn child parenting server
children) failed at once with `FAIL spawn rc=-1`: `SYS_SPAWN` rejects when
`spawn_parent != -1` — while a spawn child runs, its parent sh is
spawn-parked, so a second spawn is refused. A spawn child therefore cannot
hold a send name across another task's death either (it cannot parent the
dying task, and its own space dies with it). The test was redesigned around
the one persistent userspace task: sh holds send names across spawn runs via
new test-only builtins in `user/sh.c` (same precedent as the `key` injection
builtin):

- `ipc resolve <slot>` → resolve the RV slot, hold the send name;
- `ipc send <id>` → send a fixed 8-byte message (body `A5 5A 00..`);
- `ipc req <id>` → alloc a reply port, send a reply-carrying request;
- `ipc park <ms>` → blocking recv on the held reply port (unsigned ms only:
  a stranded park would hang the console owner).

Each prints one greppable `IPC: ...` line. `scripts/verify-ipc3.py` drives
the whole sequence from sh in one dev-mode boot (typed serial only, no `gui`).
The five server legs are embed-hook spawn children (`uipcdsetup`:
alloc + setqlimit 16 + publish `ipc3`; `uipcdclean`: claim + clean exit;
`uipcdfault`: claim + null deref → `0xDAB7`; `uipcdwedge`: claim + yield
forever → watchdog kill `0xD09D`; `uipcdrain`: claim + poll → `DRAIN EMPTY`
or `DRAIN MSG OK`). This "no nested spawn" constraint is a real harness
limit IPC-4 inherits: its registry test server cannot be a spawn child
parenting clients either — it must be resident, or sh-driven.

### 4.2 The send-once notification is a second peer-death channel

P5's first park did NOT return EPEER as first designed: it returned a 24-byte
message, id 71. That is upstream Mach's own mechanism, not a bug: destroying
a queued message that carries a send-once right queues a
`MACH_NOTIFY_SEND_ONCE` (id 71 = `MACH_NOTIFY_FIRST` 0100 octal + 007,
`mach/notify.h`) on the reply port (`ipc_kmsg.c` → `ipc_notify_send_once`).
So a peer's death produces TWO signals, in order: (1) the kernel's send-once
notice, delivered by poll before any wait record opens (the queued request
dies with the peer); (2) the IPC-3 EPEER annotation, which survives the
notice delivery (poll returned before park-open, so the pending was still
pending) and fires on the next park. The test now asserts all three stages
sharply: park 1 → `rc=24 id=71`; park 2 → `rc=-12` at once (a 500 ms park
returning −12 cannot be a deadline expiry — that yields −10); park 3 →
`rc=-10` (control: annotation consumed, plain deadline park).

## 5. Verification: test-first evidence

`scripts/verify-ipc3.py`, one dev-mode boot, five phases (clean death /
watchdog kill / fault death / fresh incarnation / peer death):

- **Pre-change FAIL** (IPC-2 kernel + full test scaffolding, previous script
  revision; the kernel had no teardown): **FAIL 23/29** — the harness legs
  all passed (boot, shell, setup/resolve/send-live, wedge killed with
  `0xD09D`, fault with `0xDAB7`), and exactly the feature assertions failed:
  all three send-after-death probes SUCCEEDED (`rc=0`, message queued on the
  corpse port) instead of −3; the peer park expired with ETIME instead of
  EPEER; no bootlog event 18. (One of the six fails was a harness artifact —
  the FAULT check matched the `UIPCDFAULT` program name — fixed since; see
  §5 note.)
- **Final-script pre-change, derived:** the final script adds one check (the
  send-once notice stage) and fixes the FAULT filter. The pre-change serial
  log proves each derived outcome: the 5000 ms peer park returned ETIME
  (`rc=-10`), not `24/71`; the EPEER stage failed; the control ETIME stage
  passed; no other FAULT lines exist besides the two filtered ones. Derived
  final-script baseline: **FAIL 24/30** (fails: 3× EDEST, notice, EPEER,
  event 18). The derivation is recorded here rather than a re-run because a
  stash-based re-run proved methodologically unsound: the test hooks live in
  `user.c`, so stashing the kernel also removed the hooks (`spawn gagal` on
  the first leg) — the true baseline is IPC-2 kernel + scaffolding, which is
  what the 23/29 run was.
- **Post-change PASS: 30/30**, including `bootlog carries IPC death note
  (ev 18)` (six `ipc dead spawned …` records in the run's bootlog) and
  `fail_count=0` at the end. Re-run after the stash/pop cycle to confirm the
  final tree state: **PASS 30/30 again**.
- Pre-change FAIL shape, for the record: send-after-death `rc=0` (×6: live
  sends + dead sends interleaved — the dead ones are the ones that must be
  −3); peer park `rc=-10` (ETIME, 5000 ms); no `ipc dead` in bootlog.

## 6. Battery (standalone, this worktree)

| Script | Result |
|---|---|
| verify-shell | PASS |
| verify-safemode | PASS |
| verify-firstboot | PASS on standalone re-run (see note) |
| verify-update | PASS |
| verify-fnd1 | PASS |
| verify-fnd2 | PASS (21/21) |
| verify-fnd3 | PASS (46/46) |
| verify-fnd4 | PASS (36/36) |
| verify-fnd5 | PASS (33/33) |
| verify-ipc1 | PASS (29/29) |
| verify-ipc2 | PASS (57/57) |
| verify-ipc3 | PASS (30/30) |
| verify-onb1 | PASS |
| verify-onb2 | PASS (battery tail obscured its result line by a nanobind warning from the QR-decode tooling; clean standalone re-run: ONB2-VERIFY: PASS) |

Notes, not hidden: (1) The FND-3 leg's known flake-sensitivity on `boot 3
bootctl fail_count=0` (intermittent SD/fat one-shot timing flake, pre-existing,
unrelated to IPC) did NOT trigger in this battery — FND-3 passed first
attempt. (2) Firstboot FAILED in battery order and PASSED standalone, for a
precondition reason, not a regression: the script's boot 1 requires an
unprovisioned disk, but the ONB1 leg provisions it, and a failed run never
reaches its own boot-2 `setup reset` — so the provisioned state sticks across
re-runs. A manual `setup reset` boot (cleared, `fail_count=0`) followed by a
standalone run gives FIRSTBOOT-VERIFY: PASS. The IPC-2 battery note ("standalone
re-run passes") assumed a run that had reached boot 2; this battery shows the
stickier case. The script's precondition (caller leaves the disk
unprovisioned) is worth a scrub step in a future battery harness, but no code
change was needed for IPC-3.

## 7. Soak

Scheduler-adjacent code changed in this phase (`sys_recv_block` verdict
order, `SYS_SEND` annotation, the reclaim funnel, the new wake path), so per
the plan's rule the soak is run even though the switch functions themselves
(`sched_ipc_wait_switch`, `pick_next`, `wd_supervise`) are untouched:
`logs/soak-results.log` archived to `logs/soak-results-pre-ipc3.log` first
(the FND-3 resume trap), then `soak-test.py` (100 cold boots + 30 random
power cuts + final clean boot). **Result: SOAK PASS, fails=0** — 100/100
boots reaching the shell prompt with the core PASS markers and no
panic/FAULT, 30/30 power cuts (SIGKILL mid-boot), final clean boot with
littlefs + bootstate intact. The dev disk was left healthy and
unprovisioned.

## 8. Image size

Raw kernel binary (`llvm-objcopy -O binary`): **3,272,364 B**; headroom
against the 4 MiB boot partition: **921,940 B**. That is +86,264 B since the
IPC-1 measurement (3,186,100 B → 1,008,204 B headroom) — but the delta spans
IPC-2 + IPC-3 + test scaffolding (five new test programs, the sh builtin),
not IPC-3 alone. UI-1 re-measures at its start per the standing rule; the
reconciled headroom figure, not the retracted ~287 KB one, remains the
budget.

## 9. Honest limitations (by design, not bugs)

- **Concurrent wake-from-park is still unproven end-to-end.** With one spawn
  slot, a parked IPC waiter + a concurrent userspace actor can never coexist
  (same wall as IPC-2's send-wake limitation, same class of proof): P5 stages
  the peer-death verdict at park time (peer already destroyed by reclaim)
  plus the reclaim scan that wakes already-parked waiters; the mid-park wake
  differs only in timing, not in code path (same flag, same re-dispatch
  verdict, same release rules). The residual (a truly concurrent waiter +
  wedged server, woken from its park by the server's death) lifts the moment
  a second IPC speaker exists — IPC-4's resident registry test server, or
  linkd in C6L-5. The design structurally guarantees no stranded park: every
  park ends by message, deadline, or peer-death verdict; the reclaim scan
  covers all parked waiters on every death route.
- **v1 tracks only the most recent outstanding request** (§2.3).
- **Task objects / vm_maps / pmaps still leak on death** (§3) — bounded,
  pre-existing, and now the ONLY leak class left on this path (ports +
  queued kmsgs are freed).
- **Ramfs fd tables still leak per task** (`RAMFS_MAX_TASKS` 16, found in
  IPC-1, FND-2-family follow-up) — untouched by IPC-3; the test routes around
  it with the RV-slot protocol, as IPC-1 did.
- **Registry is IPC-4 and was NOT built here.** The IPC-1 test-rendezvous
  scaffolding stays until IPC-4 deletes it (its slot-ownership semantics are
  documented in §2.1 so IPC-4's REGISTER can replace them deliberately).
- **Tree entries are warned, not torn down** (§2.2) — unreachable with
  today's allocation patterns; if the warning ever fires, that is a bug to
  fix, not a case to ignore.
- **No policy on names** (IPC-4, D-IPC-2 spawn-identity enforcement) and no
  queue-limit re-sizing (IPC-5 budgets on IPC-1's qlimit 16 / 4 KB cap).
