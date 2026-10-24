# DEVLOG — IPC-2: blocking receive as a scheduler switch point

Branch `dev/ipc2`, on top of the IPC-1 merge (`f095302`).
Plan: `docs/rencana-ipc/PLAN.md` §IPC-2, resolutions D-IPC-1..3.

## What was built

`SYS_RECV` (11) gains the blocking form. The timeout travels
in `r3` exactly as the plan's encoding: `0` = poll (IPC-1
semantics, unchanged), `> 0` = park up to that many ms,
`-1` = park with no deadline (allowed, but bootlogged).
`user/ulib/uipc.h` documents the forms; `uipc_recv()` is
now the default (it loops on the internal park code) and
`uipc_recv_poll()` stays for hot loops. One-shot callers
(no scheduler yet) always get poll semantics, whatever
the timeout argument says.

### The generalised WAITING state

State 2 was Q9's spawn-parent wait. It is now general: a
daemon is in state 2 either as a spawn parent (collected
by `spawn_done`) or as an IPC waiter (collected by its
wait record). The record lives in `struct daemon`:

- `ipc_wait_port` — the destination port OBJECT
  (`ipc_port_t` identity, opaque `void *` here);
- `ipc_wait_deadline` — an `arm_timer_ticks()` deadline,
  `0` = no deadline;
- `ipc_wait_active` — set while the wait is open (parked
  or woken-but-not-yet-re-dispatched).

`wd_supervise()` only ever judges state 1, so both kinds
of waiter are exempt from hang verdicts while parked —
the FND-3 spawn-parent rule, generalised, which is
exactly D-IPC-1's "exempt-but-observed". The "observed"
half is the bootlog: the no-deadline form logs event 17
(`ipc park-nodeadline <name>`) at park time, so a
parked-forever daemon is visible in post-mortem.

### The wait engine (`user.c`, IPC-2 block)

`sys_recv_block()` is a loop, and the poll always runs
first: a message already queued is delivered without
parking, and a message that raced a deadline beats it.
Only the empty-queue path opens the record and parks.
A re-dispatch after a wake keeps the ORIGINAL record
and deadline: one logical wait, however many parks it
took. The wait completes in exactly three ways:

1. **Send** — the glue calls `sched_ipc_wake_port()` with
   the destination port object after staging (see below);
   matching waiters become runnable.
2. **Deadline** — `ipc_wait_expire()` runs in the
   yield-switch prologue, the exit-switch prologue, the
   wait switch itself, and once per tick in the
   all-parked idle loop.
3. **Never** — the no-deadline form (bootlogged; peer
   death as a completion is IPC-3's lifecycle work).

Wakes only flip state and stamp `wd_last_progress`
(D-IPC-1: the wake restarts the progress clock, the Q9
spawn-wake precedent). The record stays open until the
woken daemon re-enters `SYS_RECV`, where the verdict is
decided in ITS OWN context: poll first, then the
deadline (`UIPC_ETIME`, −10), then park again. No
copyout is ever done in a waker's context.

### The resume channel (no pc rewind, no re-execution)

`trap.c` writes `user_syscall()`'s return over the
(possibly switched) frame's `r0`. The park therefore
saves `r0 = UIPC_EPARK` (−11) in the waiter's context
(`ctx_save_wait`, a `ctx_save` variant), and every
switch function sets `sched_resume_ret` to the resumed
daemon's saved `r0` right after its `ctx_load`; the
`SYS_YIELD` / `SYS_EXIT` cases return it. The waiter's
`SYS_RECV` thus returns `UIPC_EPARK`, the ulib wrapper
re-issues the call, and the re-dispatch resolves the
wait as above. In every pre-IPC-2 flow the parked `r0`
is 0 (forced by `ctx_save`), so all existing return
semantics are unchanged.

### The glue side (`ipc_glue.c`)

- `ipc_glue_recv_ex()` — the recv core plus an
  `obj_out` parameter reporting the destination port
  object identity on every outcome where the name
  resolved, including `UIPC_EEMPTY` (the path that
  parks). `ipc_glue_recv()` is now a wrapper over it.
  The identity is a raw pointer, safe for a wait's
  whole life: the receive right lives in the waiter's
  own space for exactly that long, so the port cannot
  be freed underneath the park, and only the
  receive-right owner can wait on a port.
- `ipc_glue_send()` captures the destination object
  from the kmsg header's remote-port field after
  copyin (upstream `ipc_mqueue.c`: copyin translates
  the destination name into the object there) and,
  after a successful stage, calls
  `sched_ipc_wake_port()`. The hook runs on every send;
  it is a scan that finds no waiter in the current
  harness's sequential flows (see Limitations).

### The all-parked idle

When the wait switch finds nobody runnable, it does not
return to a frame: it spins one timer tick at a time,
pumping the network per tick (a TCP arrival can be the
event a waiter indirectly needs, and HTTP stays alive)
and expiring deadlines. The first wake — a deadline
here, since no userspace runs to send — ends the idle.

## The bug that shaped the build: trap context masks IRQs

The first staged park hung the guest silently. The
investigation (temporary printfs, then disassembly of
the generated spin loop) established the spin itself
was correct — real `bl arm_timer_ticks` calls per
iteration — but `timer_ticks` never advanced: it is
incremented only in the timer IRQ handler, and SVC
trap context in this port runs with CPSR.I = 1
(measured: `cpsr=0x...93` in the trap handler vs
`spsr=0x...10` for the user frame). The fix: the idle
brackets its raw tick wait with `cpsie i` / `cpsid i`,
so IRQs are live for the wait itself and everything
else in the switch runs in the normal masked
environment. The nested timer IRQs are safe — they
take their own frame on the SVC stack — and
`wd_irq_tick()` only ever judges user-mode frames
("kernel/WFI stretches are not hangs"), so class (b)
cannot fire from the idle; `wd_last_activity` is
stamped per tick regardless. A consequential fact for
future phases: **any kernel code that needs time to
pass while it runs in trap context must unmask IRQs
explicitly** — `arm_timer_ticks()` is frozen otherwise.

A second, smaller find during the same debugging: this
kernel's `printf` has no `%p` (it prints a bare `p`
and shifts the remaining arguments), which made one
debug print look like corrupted values. Not fixed
here — it is a pre-existing printf gap, and no
production code path prints a pointer.

## Verification (test-first)

New script `scripts/verify-ipc2.py` (a separate script,
not an extension of verify-ipc1: the IPC-2 flows need
three boots and a GUI phase of their own, and
verify-ipc1 stays the untouched IPC-1 contract).
Pre-change FAIL evidence: **33/61** — every blocking
semantic failed (deadline probe, 100/100 round trip,
park behaviour, no-deadline kernel log, ev=17) while
the harness itself worked (boots, web, GUI screendump,
FND-3 adjacency all passed pre-change).
Post-change: **IPC2-VERIFY PASS (57/57)**.

- Boot 1 (typed serial): the IPC-2 pair round-trips
  **100/100 with byte-exact payloads** on the blocking
  receive; the client's setup run opens with a
  deadline probe (500 ms on a silent port → `ETIME`,
  guest elapsed 500 ms exactly); every server drain
  run ends with a park probe (400 ms on the emptied
  queue → `ETIME`). Then `uipkpark` parks 65 s on a
  silent port with sh spawn-WAITING — the all-parked
  state: still parked at 40 s, no reset, deadline wake
  (`rc=-10`), program PASS, and the bootlog gained NO
  watchdog events (ev=13/14 counts unchanged across
  the park). Then `uyield` adjacency: a genuinely
  wedged daemon in the same boot is still warned,
  declared hung and killed (`exit 53405`).
- Boot 2 (GUI up, web shell): `uipknd` parks with no
  deadline; the kernel prints its bootlogged form; the
  face home screen (786 eye pixels before the park,
  4460 during) keeps rendering while the child is
  parked and sh waits; no watchdog verdict; QEMU is
  killed with the park in place.
- Boot 3 (same disk): the bootlog ring carries `ev=17`
  from the no-deadline park; `setup reset` leaves the
  disk unprovisioned and `bootctl fail_count=0`.

Two harness lessons from the same run, both fixed in
the script: the mid-park face sample must wait for the
face home screen (the face shows its setup QR until
the provisioned flag flips — boot 2 now provisions
with `setup done` before `gui`; boot 3 resets it
again).

## Regression battery (standalone, this worktree)

| Script | Result |
|---|---|
| verify-ipc2.py | PASS (57/57) |
| verify-ipc1.py | PASS (29/29) |
| verify-fnd3.py | PASS (46/46) on re-run; first run 45/46 (see below) |
| verify-shell.sh | PASS |
| verify-safemode.sh | PASS |
| verify-firstboot.sh | PASS (after a provisioning reset; see below) |
| verify-update.sh | PASS |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS |
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS (21/21) |
| verify-fnd4.py | PASS (36/36) |
| verify-fnd5.py | PASS (33/33) |

Two non-IPC-2 findings, both honestly recorded:

1. **FND-3 first run 45/46**: `boot 3 bootctl
   fail_count=0` failed because boot 1041's one-shots
   reported 4 FAILs (missing `/sd.out`, `/fat.out`,
   `/.usd_done`, `/.ufs_done`) and boot 1043's 2 FAILs
   (missing `/fat.out`, `/.ufs_done`), so the healthy
   mark was skipped and fail_count stayed 1. The
   pattern is intermittent (boots 1039/1040/1042 clean
   in the same run; the re-run's log has no FAIL
   summaries at all and passes 46/46), the failing
   one-shots are SD/fat outputs that run
   pre-scheduler with no IPC code path, and the
   re-run passes. Verdict: a pre-existing SD/fat
   one-shot timing flake, not an IPC-2 regression —
   but it is real, and the parent should know the
   battery's FND-3 leg is flake-sensitive on this
   point.
2. **firstboot first attempt failed on premise**:
   the battery was run out of order (FND-3 before
   firstboot), so the disk was provisioned when
   firstboot expected it unprovisioned. After a
   `setup reset` boot it passes. Ordering note for
   future batteries, not a code issue.

## Soak (scheduler touched → mandatory)

Fresh log (the previous `logs/soak-results.log` was
archived to `logs/soak-results-pre-ipc2.log` first, per
the known resume trap): **SOAK PASS, fails=0** — 100
cold boots (each reaching the shell prompt with the
core PASS markers, no panic/FAULT) + 30 random power
cuts (SIGKILL mid-boot) + a final clean boot with
littlefs + bootstate intact. The dev disk was left
healthy and unprovisioned (`setup reset` verified) at
the end of the phase.

## Limitations (honest, by design of the plan)

- **Send-wake is not staged end-to-end.** The harness
  has one spawn slot and its parent parks while a
  child runs, and the resident daemons (face, uiapp,
  qabotd, sh) do not speak IPC — so no run can park a
  daemon while a second concurrent userspace actor
  sends to its port. What IS proven end-to-end: the
  full park → wake → re-dispatch → verdict path via
  deadline expiry, and the send hook fires on every
  staged send (it shares `ipc_wait_wake_idx` with the
  proven expiry path; the two differ only in what
  sets state 1). The plan's "first send wakes the
  system" wording is therefore met by deadline wake
  in the staged probes, not by a staged send. This is
  the same class of limitation as FND-3's unexercised
  respawn rung, and it lifts the moment a second IPC
  speaker exists (IPC-4's registry test server, or
  linkd in C6L-5).
- **Death completion is IPC-3.** A wait on a port
  whose owner exits is completed by deadline only; a
  parked daemon cannot die in IPC-2 (it is not
  running, so it cannot fault, and the watchdog only
  kills state 1).
- **Registry/RPC is IPC-4.** The test rendezvous
  (RV_*) is unchanged and still deleted at IPC-4.
- **Queue sizing** stays IPC-1's: qlimit max 16, so a
  full queue at the 4 KB cap is ~66 KB of kmsg zone
  per port; IPC-5's sizing work budgets on that.

## Commits (dev/ipc2, on top of f095302)

- `8fd1f13` user: uipc_recv() blocking form + SYS_RECV
  timeout arg (IPC-2)
- `526f1f5` user: IPC-2 test programs — blocking pair +
  park probes
- `85f8f73` scripts: verify-ipc2.py — blocking-receive
  verification (test-first)
- `867d0aa` kernel: IPC-2 blocking receive — third
  scheduler switch point
- `4608948` kernel: IPC-2 idle — unmask IRQs for the
  all-parked tick wait
- `52d2989` scripts: verify-ipc2 — wait for the face
  home screen before the park
- `772818e` scripts: verify-ipc2 — provision before
  gui in boot 2
- (this commit) docs: DEVLOG-IPC2 + soak evidence
