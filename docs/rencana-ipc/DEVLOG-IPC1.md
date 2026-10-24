# DEVLOG — IPC-1: poll-only inline Mach IPC send/receive

Status: **DONE** on branch `dev/ipc1` (worktree
`~/workspace/qaonic_os-ipc1`), not merged, not pushed —
lead review + merge is the coordinator's step.
Spec: `docs/rencana-ipc/PLAN.md` §IPC-1 + Resolutions
(D-IPC-1..3); research: `docs/riset-ipc-mach/RINGKASAN.md`.

## What was built

Real Mach IPC, called from userspace for the first
time in this port. The machinery was already compiled
in and initialised every boot (17 of 18 upstream
`ipc/*.c`); IPC-1 is the caller the research predicted:
an extension-layer glue plus a ulib surface.

| Piece | File |
|---|---|
| Glue (all Mach calls) | `mach3-port/src/ext/ipc_glue.c` |
| Glue interface + mirrored constants | `mach3-port/src/ext/ipc_glue.h` |
| Dispatch cases + number defines | `mach3-port/src/ext/user.c` |
| User API (layout, sub-ops, error codes) | `user/ulib/uipc.h` |
| Raw stubs + wrappers | `user/ulib/ulib.c`, decls in `user/ulib/ulib.h` |
| Test programs | `user/uipcsrv.c`, `user/uipccli.c` (embed hooks, both build loops) |
| Verification | `scripts/verify-ipc1.py` |

### Identity — the point of the phase

The dispatch hands the glue the **caller's task**
(`cur_utask`: the task of the daemon/spawn slot
actually running, set by `sched_activate()` /
`launch_uprog()`). Every Mach operation is performed
FOR that task: names resolve in its `itk_space`,
copyin/copyout use its `vm_map`. `mach_msg_trap` is
never called — it derives identity from
`current_thread()`, which in this port is always the
boot thread (all daemons are multiplexed onto it), so
the trap would silently operate on the kernel task's
space. The glue uses the layer beneath the trap, which
takes identity explicitly (`ipc_kmsg_copyin(space,
map, …)`, `mach_port_allocate(space, …)`, …).

### No blocking — timeout-0 at the Mach layer

`ipc_mqueue_send` / `ipc_mqueue_receive` block via
`thread_block()` on `current_thread()` — unusable
here — but both return `*_TIMED_OUT` without reaching
the blocking branch when passed timeout 0 (verified
line-by-line in upstream `ipc/ipc_mqueue.c`, both
directions). The glue passes timeout 0 **always**;
polling lives in userspace. The blocking receive
(D-IPC-1) is IPC-2's scheduler switch point and will
be built on this file.

### Message shape

Userspace sees the 24-byte `struct uipc_msg` header
(reply / dest / id / size + padding), translated
field-by-field by the glue into a `mach_msg_header_t`
in a static kernel buffer (syscalls are serial under
the cooperative scheduler — the SYS_SPAWN imgbuf /
BOOTSTATE pkgbuf pattern). The kmsg is built with
`ipc_kmsg_get_from_kernel` and the real copyin
machinery translates the rights: dest is `COPY_SEND`
(or `MOVE_SEND_ONCE` on the reply path), the header
reply field is `MAKE_SEND_ONCE`. The COMPLEX bit is
never set, so body bytes are never parsed as
descriptors anywhere. Wire size is rounded up to a
multiple of 4 with zero padding (a kmsg requirement);
body cap is the owner-frozen **4096 B** (D-IPC-3),
total max 4120 B. Cleanup follows the trap's own
contracts: copyin failure → `ipc_kmsg_free`, send
failure → `ipc_kmsg_destroy`, receive copyout
failure → `ipc_kmsg_destroy`, success → `ipc_kmsg_free`
after translation.

### Syscall surface as implemented

| Nr | Name | Args (r0..r3) | Returns |
|---|---|---|---|
| 10 | `SYS_SEND` | dest name, buf, total len, flags (bit0 = move-send-once, the reply path) | 0 / UIPC_E* |
| 11 | `SYS_RECV` | port name, buf, max | total len / UIPC_E* |
| 25 | `SYS_IPC_PORT` | sub-op, a1, a2 | name / 0 / UIPC_E* |

Number 25 is hereby ratified for `SYS_IPC_PORT`
(the research table proposed it as "to be ratified in
IPC-1"); 12 (`SYS_RPC`) and 23 (`SYS_RPC_USER`) stay
reserved for IPC-4. `SYS_IPC_PORT` sub-ops:

| Sub | Name | Args | Effect |
|---|---|---|---|
| 0 | ALLOC_RECV | — | `mach_port_allocate(RECEIVE)` → name |
| 1 | DESTROY | a1 = name | `mach_port_destroy` (a receive name destroys the port + queue) |
| 2 | SETQLIMIT | a1 = name, a2 = limit 1..16 | `mach_port_set_qlimit` (needs the receive right) |
| 3 | RV_PUBLISH | a1 = slot string, a2 = port name | rendezvous: extract receive right (MOVE_RECEIVE) into a glue slot |
| 4 | RV_RESOLVE | a1 = slot string | rendezvous: mint a send name (`ipc_port_make_send` + `ipc_port_copyout_send`) |
| 5 | RV_CLAIM | a1 = slot string | rendezvous: insert the parked receive right into the caller's space (`ipc_object_copyout` PORT_RECEIVE) |

Error codes (negative returns, `uipc.h`): −1 EINVAL,
−3 EDEST (dead port / no such name or right), −4
EFULL (queue full at poll), −5 EEMPTY (queue empty at
poll), −6 E2BIG (oversize send; receive buffer too
small — the message **stays queued**, Mach semantics),
−7 EBUSY (rendezvous slot conflict), −8 ENOMEM,
−9 ENORV (rendezvous slot empty). −2 remains the
dispatch's ENOSYS and is deliberately not reused.

## Three findings that shaped the build

### 1. A port queue holds at most 16 messages — the test pair is batched

`MACH_PORT_QLIMIT_MAX` is **16** (`mach/port.h:191`;
`mach_port_set_qlimit` returns `KERN_INVALID_VALUE`
above it). 100 messages can therefore never be in
flight at once, and — because the pair runs as
*sequential* spawn children (one spawn slot; SYS_SPAWN
parks the parent) — the round trip is store-and-forward
in batches, as the plan's "run sequentially via spawn"
requires. The pair is self-organising:

- Each program first CLAIMs its rendezvous slot.
  `ENORV` ⇒ it is the setup instance (alloc + qlimit
  16 + publish). Otherwise it is a work instance.
- Coordination state rides **in the queue** as a
  STATE message (id `0x5EED`, body = counters), queued
  while the queue is otherwise empty so it is always
  first out. One queue slot is spent on state, so the
  batch is 15; 100 messages = 7 batches (15×6 + 10).
  (The ramfs counter files other pairs use are not an
  option — finding 3.)
- Payload: seq s has id `0x1000+s`, body length 4
  (s=0), 4096 (s=50, the cap), else
  `8 + ((s*37) % 62) * 4`; byte j = `(s*31 + j) & 0xFF`.
  Replies: id `0x2000+s`, body byte ^ `0xA5`, sent on
  the header's send-once right. Both sides verify
  order and bytes exactly.

Input for IPC-5 / C6L: queue depth 16 is a hard
upstream ceiling — link profiles must be sized
against it, and with the 4096 B cap a full queue is
up to ~66 KB of kmsg-zone memory per port.

### 2. CLAIM must reset the make-send count (receiver epoch)

First end-to-end run halted the CPU:
`Assertion failed: ipc/ipc_right.c, line 2169` —
`assert(port->ip_mscount == 0)` inside
`ipc_right_copyout` (PORT_RECEIVE). Cause:
`ipc_port_make_send` (used by RV_RESOLVE) **increments
`ip_mscount`**, and nothing ever decrements it; the
count is only zeroed by `ipc_port_set_mscount(port, 0)`
inside `ipc_port_clear_receiver`, i.e. when a receive
right changes hands. A parked port that was ever
resolved was therefore unclaimable — permanently, not
just in the test flow (any future IPC-4 design that
mints send rights against a port and later moves its
receive right hits the same wall). Fix, in
`rv_claim`: under `ip_lock`, `ipc_port_set_mscount
(port, 0)` before the copyout — the same epoch reset
upstream performs at extract time. Outstanding send
rights stay valid (counted in `ip_srights`,
untouched); no no-senders notification is ever
registered on a parked port, so no consumer of the
count is affected. Assertions are compiled IN in this
build (`MACH_ASSERT`), which turned a latent Mach
invariant violation into an immediate, diagnosable
halt — recorded here as a behaviour to preserve, not
paper over.

### 3. Pre-existing gap found: ramfs fd tables leak per task (NOT fixed here)

The pair's first design kept counters in ramfs files
(the house coordination pattern). It failed mid-run:
`ramfs` fd-table slots (`RAMFS_MAX_TASKS` = 16,
`mach3-port/src/ext/ramfs.c`) are keyed by task
pointer in `taskfds_nolock()` and **never released** —
there is no ramfs hook in `daemon_reclaim` or anywhere
else. Boot one-shots consume most slots; each spawn
run is a fresh task, so a few runs exhaust the table
and `ramfs_open` starts failing system-wide. This is
an FND-2-family resource-reclamation gap (the funnel
reclaims display, console, web-shell, face-request
and TCP state, but not ramfs tables) and is reported
here for a future foundations follow-up; IPC-1 does
not touch it (scope), it routes around it with the
state-message protocol above.

## Verification (test-first)

`scripts/verify-ipc1.py` — one dev-mode boot, typed
serial only (sh owns the console pre-`gui`), 16 spawn
runs, assertions from guest behaviour only.

- **Pre-change** (test programs + ulib in place,
  syscalls still the dispatch's ENOSYS): both programs
  print `FAIL claim rc=-2`, exit 1 →
  `IPC1-VERIFY: FAIL (4/35 checks)`; the 4 passes are
  boot/shell/no-fault sanity. (An earlier harness bug
  — matching `"exit 0"` as a prefix of the kernel's
  `exit 0x1` log line — was found and fixed during
  this phase; the match is now the exact `"exit 0\n"`
  shell line.)
- **Post-change**: `IPC1-VERIFY: PASS (29/29 checks)`
  (the count differs because the rounds loop stops
  once both PASS markers land). Run twice: before and
  after the battery, same result.
- Evidence in the serial log (`logs/ipc1-serial.log`):
  all four negative probes print their expected
  codes (empty −5, full −4 after exactly the qlimit,
  dead −3, oversize −6); progress markers climb
  15/30/…/90 on both sides; both PASS markers print;
  no `FAULT`, `fail_count=0` at the end.

### Regression battery (standalone, this worktree)

| Script | Result |
|---|---|
| SHELL-VERIFY | PASS |
| SAFEMODE-VERIFY | PASS |
| FIRSTBOOT-VERIFY | PASS (see note) |
| UPDATE-VERIFY | PASS |
| ONB1-VERIFY | PASS |
| ONB2-VERIFY | PASS |
| FND1-VERIFY | PASS |
| FND2-VERIFY | PASS (21/21) |
| FND3-VERIFY | PASS (46/46) |
| FND4-VERIFY | PASS (36/36) |
| FND5-VERIFY | PASS (33/33) |
| IPC1-VERIFY | PASS (29/29), twice |

Note: FIRSTBOOT failed on its first invocation in
this battery order because the dev disk was
provisioned at entry (the script assumes an
unprovisioned entry state and does not scrub); its
own boot-2 `setup reset` restored the assumption and
the standalone re-run passed in full — the
documented battery state-sensitivity, not a
regression. The worktree disk is left healthy
(`fail_count=0`, provisioning reset). The main tree's
disk images were never touched; all runs used the
worktree copies.

## Headroom reconciliation (IPC-1 exit criterion)

**Method.** The boot partition holds exactly one
thing: `qaonic_kernel.bin`, produced by
`llvm-objcopy-18 -O binary mach3.elf` and checked by
`scripts/build-update-img.sh` (`KSIZE < BOOT_PART_MAX`
= 4,194,304 B; partition size from
`scripts/update-img/parameter.txt`: boot =
0x2000 sectors × 512 B). Partition occupancy is
therefore measured on the **raw binary**, never on
ELF section sums.

**Measured today** (post-IPC-1 build, this worktree):

| Measure | Bytes |
|---|---|
| Raw binary (`objcopy -O binary`) | **3,186,100** |
| `llvm-size` text + data | 3,181,940 + 4,160 = 3,186,100 |
| `llvm-size` bss (RAM only, not in the partition) | 4,229,016 |
| Boot partition | 4,194,304 |
| **Headroom (reconciled)** | **1,008,204 B (≈ 0.96 MiB)** |

**Reconciling the two research figures.**
The IPC research (§7) measured the raw binary at
3,148,148 B (headroom 1,046,156 B) on its day's build
— the same method as above, and reproducible in
kind: today's binary is +37,952 B over it, accounted
for by IPC-1 itself (glue + dispatch + ulib growth in
every embedded program + the two embedded test
programs, 10,472 + 11,620 B). The UI research's
"text+data 3,903,036 B" (→ ~291 KB headroom; the IPC
doc quotes a 3,907,484 variant) **does not reproduce**
under either accounting on any build available now:
it is ~755–770 KB larger than the raw binary of its
own era, and no combination of loadable sections
reaches it (text+data was ~3.14 MB then, ~3.19 MB
now). The only accounting that lands in that range
adds some or all of `.bss` — which occupies RAM,
never the flash partition. **Verdict: the ~287–291 KB
figure is retracted. All later phases — including
UI-1's LVGL go/no-go — spend against the reconciled
headroom, 1,008,204 B as of this build, re-measured
at each phase start.** (IPC-1's own spend, +37,952 B
all-in, is two orders below even the retracted
figure; no design decision in this track depended on
the optimistic number.)

## Limitations (honest, by design of the plan)

- **Poll-only.** No blocking anywhere; a waiter spins
  in userspace with yields. Blocking receive is IPC-2.
- **Death leaks remain.** A task that dies holding
  rights keeps its space, ports and queued messages
  (one round of this very test leaks a claimed port
  if a run fails mid-drain). IPC-3 owns teardown in
  `daemon_reclaim`.
- **The rendezvous is test scaffolding.** No
  ownership check on CLAIM; fixed 4 slots; slot names
  ≤ 15 chars. IPC-4 replaces it with the registry and
  D-IPC-2's spawn-identity policy — it is deleted,
  not extended. Product code must not build on it.
- **Send-once sends bypass the queue limit**
  (upstream Mach semantics: a send-once message is
  always enqueued). Replies can therefore overfill a
  reply port past its qlimit; flows that care must
  bound outstanding replies themselves (the test pair
  does: ≤ 15 outstanding by construction).
- **Name/error aliasing wart** (inherited from the
  house ABI, documented in `uipc.h`): returns are
  signed; a port name with the top bit set would
  alias an error code. Unreachable at current table
  sizes (128-entry initial tables, bounded test
  allocations), flagged for IPC-4's ABI hardening.
- The state-message protocol is test scaffolding for
  the sequential-spawn constraint, not a product
  pattern; linkd (C6L-5) will be a resident daemon
  and needs none of it.

## Commits (dev/ipc1, on top of efd5f76)

1. `2bba6c0` user: uipc.h v0 + ulib stubs for
   SYS_SEND/SYS_RECV/SYS_IPC_PORT (IPC-1)
2. `66e4b1d` user: IPC-1 test programs uipcsrv/uipccli
   + build wiring + embed hooks
3. `817de20` scripts: verify-ipc1.py — IPC-1
   round-trip + negative probes (test-first)
4. `c3191f9` scripts: verify-ipc1 — exact exit-line
   match + serial log dump
5. `eb30e35` ext: IPC-1 glue — SYS_SEND/SYS_RECV/
   SYS_IPC_PORT over real Mach IPC (poll-only,
   timeout-0)
6. `69cd9fb` ext: IPC-1 rendezvous claim — reset
   make-send count (receiver epoch, mirrors
   ipc_port_clear_receiver)
7. `4aff509` user: IPC-1 test pair — coordination
   state as queued STATE message (ramfs fd tables
   leak per task; batch 15 = qlimit 16 − 1 state slot)
8. this dev log.
