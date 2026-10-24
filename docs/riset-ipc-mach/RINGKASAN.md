# Research: Real Mach IPC for QaonicOS — what exists, what is missing

Date: 2026-10-24. Commission: the owner resolved
D-ARCH-4 *against* the architecture document's
recommendation — real Mach IPC will be built, as its own
track between FND and C6L, first consumers linkd and
TOOL.DISPATCH (`docs/ARSITEKTUR.md`, "Resolutions —
owner decisions, 2026-10-24"; `docs/ARSITEKTUR-SISTEM.md`
§9 update). This research establishes, from the code,
what switching IPC on actually requires. The phased
plan that consumes it is `docs/rencana-ipc/PLAN.md`.

Companions: `docs/ARSITEKTUR.md` (placement rules +
inventory), `docs/ARSITEKTUR-SISTEM.md` (master
architecture), `docs/SYSCALL-ABI.md` (the interface
contract), `docs/riset-arsitektur-os/RINGKASAN.md` §1.1
(the microkernel-pattern analysis this reverses in
part), `docs/riset-threading-compute/RINGKASAN.md` §1
(the cooperative-scheduling verdict IPC must not break),
`docs/rencana-komunikasi-c6/PLAN.md` (the consumer
track).

## 0. Headline findings

1. **The IPC machinery is already in the running
   kernel.** 17 of the 18 upstream `ipc/*.c` files are
   `standard` in `conf/files`, are compiled by
   `build-mi.sh`, are linked into `mach3.elf`, and are
   initialised at every boot (`ipc_bootstrap()` +
   `ipc_init()` run from `setup_main()`). The resident
   cost — ~85 KB — is already paid. What is missing is
   not machinery; it is a *path*: no user trap reaches
   it, and no daemon has ever sent a message.
2. **Every daemon already owns a real Mach IPC
   identity.** Daemons are created with the genuine
   `task_create` / `thread_create` (`mach3-port/src/ext/
   user.c:2054,2060`), and `task_create` calls
   `ipc_task_init`, which creates a per-task
   `ipc_space`, a task self-port, and inherits
   registered/bootstrap ports from the parent. The
   identity gap is therefore narrow and specific (§2),
   not the blank slate the older documents assumed.
3. **The one hard problem is blocking.** All daemons
   execute on a single Mach thread (the boot thread)
   via trap-frame rewriting in the extension scheduler;
   the Mach scheduler never runs them. The upstream
   receive path blocks via `thread_block()` on
   `current_thread()` — calling it from a daemon would
   hand the boot thread to a scheduler that does not
   know our daemons exist. The upstream code also
   provides the escape hatch: both mqueue directions
   have a native non-blocking mode (timeout 0 →
   `*_TIMED_OUT`). The design that fits the house is
   *poll at the Mach layer, block at the extension
   layer* — blocking receive re-expressed as a
   yield-with-wake-condition, generalising the proven
   Q9 spawn-WAITING state (§3).
4. **Port death is today's leak.** The extension layer
   never calls `task_terminate` / `task_deallocate`;
   daemon tasks (and their spaces, ports and queued
   messages) are abandoned on death and a respawn
   builds a fresh task with a fresh space. Upstream
   has the teardown (`ipc_task_terminate`, called from
   `task_terminate`), unused. Wiring death semantics —
   not message passing — is the phase where this track
   earns or loses its keep, because FND-2/FND-3 built
   the daemon reclaim/respawn ladder IPC must now
   plug into (§5).

## 1. What exists in `upstream/mach3/kernel/ipc/`, and what is compiled

The upstream IPC directory holds 18 C sources: the full
Mach 3 message system — spaces and entries
(`ipc_space.c`, `ipc_entry.c`, `ipc_hash.c`,
`ipc_splay.c`, `ipc_table.c`), ports and rights
(`ipc_port.c`, `ipc_right.c`, `ipc_object.c`,
`ipc_pset.c`), queues and messages (`ipc_mqueue.c`,
`ipc_kmsg.c`), notifications (`ipc_notify.c`,
`ipc_marequest.c`), the message trap implementation
(`mach_msg.c`), the `mach_port_*` management routines
(`mach_port.c`), thread/task IPC state glue in
`kern/ipc_tt.c`, and `mach_debug.c`.

Build reality, verified 2026-10-24:

- `conf/files` marks every one of them `standard`
  except `ipc/mach_debug.c` (`optional mach_ipc_debug`,
  off in `build-tools/gen_config.py`). Running
  `build-tools/mi_sources.py` selects **17 ipc files**
  of 94 MI files total; `kern/ipc_host.c`,
  `kern/ipc_kobject.c`, `kern/ipc_mig.c`,
  `kern/ipc_sched.c`, `kern/ipc_tt.c` are likewise
  `standard` and selected.
- All 17 objects exist in `build/obj/ipc/` and their
  symbols are in the linked `build/mach3.elf`:
  `mach_msg_trap`, `ipc_init`, `ipc_bootstrap`,
  `ipc_space_create`, `ipc_port_alloc` all resolve as
  text symbols. The link takes every object
  (`build-md.sh` links `find build/obj -name '*.o'`),
  there is no `--gc-sections` surprise.
- Boot reaches the init: `src/md/arm/arm_init.c:62-70`
  `machine_startup()` → MI `setup_main()`
  (`kern/startup.c:245,247`) calls `ipc_bootstrap()`
  then `ipc_init()`. IPC zones, the kernel space, the
  reply space and the notify machinery are live on
  every boot today.
- Config: `gen_config.py` sets `MACH_IPC_COMPAT=0`,
  `MACH_IPC_DEBUG=0`, `MACH_IPC_TEST=0`,
  `NORMA_IPC=0`. NORMA being off matters: the
  network-IPC branches inside `ipc_mqueue.c` are
  compiled out, so the queue code that ships is the
  local-only path.

**What breaks if "enabled"?** The question inverts:
the machinery is enabled. Nothing in the MI build
needs to change. The unbuilt piece is (a) an MD/ext
trap path — `src/md/arm/trap.c:205-210` still has
`thread_syscall_return()` as a `panic("not implemented
for ARM")` stub, i.e. the Mach trap ABI was never
wired on this port — and (b) any caller. Enabling
`mach_ipc_debug` (the one excluded file) is neither
needed nor wanted: it adds debug calls, not function.

## 2. The identity gap: what a daemon is, in Mach terms

A product daemon's life today (`src/ext/user.c`):

- `setup_uprog_task()` creates a real Mach task and
  thread: `task_create(kernel_task, FALSE, &utask)`
  (user.c:2054), `thread_create(utask, &uthread)`
  (user.c:2060). One-shot programs use the same path
  (user.c:1928,1935).
- `task_create` (`upstream/.../kern/task.c:463`) calls
  `ipc_task_init(new_task, parent_task)`, defined in
  `kern/ipc_tt.c:125`. It creates the task's IPC space
  (`ipc_space_create(&ipc_table_entries[0], &space)`,
  initial table 128 entries, `ipc/ipc_table.c:88`),
  allocates the task's kernel self-port
  (`itk_self` / `itk_sself`), and — parent being
  non-null — copies the parent's registered ports,
  exception port and bootstrap port as send rights.
- `thread_create` calls `ipc_thread_init`
  (`kern/thread.c:890`; `kern/ipc_tt.c:274`), so each
  daemon thread also has its IPC thread state
  (including its reply-port slot).

So every daemon *has* an `ipc_space` and *can* hold
port rights today. The remaining gaps are four, and
they are precise:

1. **Nobody ever populated the inherited ports.** The
   parent is always `kernel_task`, whose registered /
   bootstrap / exception ports are `IP_NULL` in this
   port (nothing calls `task_set_bootstrap_port` or
   registers the clock/host ports the way a BSD
   personality would). Daemons inherit a row of null
   ports. There is no bootstrap rendezvous — §5's
   registry has to play that role.
2. **`current_thread()` is not the daemon.** All
   daemons share one executing Mach thread (the boot
   thread); the extension scheduler multiplexes them
   by saving/loading register state into per-daemon
   `struct user_ctx` slots and rewriting the single
   trap frame (`ctx_save`/`ctx_load`,
   user.c:~1607-1630; daemon table user.c:~434).
   Mach's `current_space()` / `current_map()` macros
   resolve through `current_thread()->task` — the
   *kernel* task. Any code path that derives identity
   from `current_thread()` silently operates on the
   wrong space when a daemon calls it. The trap
   implementation does exactly that
   (`ipc/mach_msg.c:207,291,396`: `space =
   current_space()`). This is the single fact that
   rules out wiring `mach_msg_trap` directly.
3. **The parameterised layer underneath is clean.**
   Everything the trap calls takes identity
   explicitly: `ipc_kmsg_copyin(kmsg, space, map,
   dest)`, `ipc_kmsg_copyout(...)` (`ipc/ipc_kmsg.h:
   297-330`); the `mach_port_*` management routines
   take the target space as first argument —
   `mach_port_allocate(space, right, namep)`
   (`ipc/mach_port.c:537`),
   `mach_port_allocate_name` (:474),
   `mach_port_insert_right` (:1332),
   `mach_port_extract_right` (:1368). Kernel glue in
   the extension layer can therefore operate *for*
   `daemons[cur].task` — with `task->itk_space` and
   `task->map` — without impersonating a thread. This
   is the seam the whole track is built on.
4. **Daemon tasks are never destroyed** — see §5.

## 3. The blocking problem, and the shape that solves it

### 3.1 What upstream receive does

`ipc_mqueue_receive` (`ipc/ipc_mqueue.c`) waits like
this: if the queue is empty and the option carries
`MACH_RCV_TIMEOUT` with `time_out == 0`, it returns
`MACH_RCV_TIMED_OUT` immediately. Otherwise it calls
`thread_will_wait[_with_timeout](self)` with
`self = current_thread()`, enqueues that thread on
`mqueue->imq_threads`, and `thread_block()`s. The send
side is symmetric: a full queue with
`MACH_SEND_TIMEOUT` + 0 returns `MACH_SEND_TIMED_OUT`;
otherwise it blocks too.

Under our execution model the blocking branch is
unusable, three times over: it would block the boot
thread (the only thread that runs *any* daemon);
`thread_block` hands control to the Mach scheduler,
whose run queues do not contain our daemon threads in
any meaningful order (they were created, given an
initial context by the extension layer, and are driven
by frame rewriting, not by `thread_start` /
`context_switch` discipline); and the mqueue wait
bookkeeping (`ith_state`, `ith_msize`) is per-*thread*
state that all daemons would share through the one
boot thread. A daemon that blocks in Mach receive
does not sleep — it corrupts the multiplex.

### 3.2 Design options

- **Option A — poll-only IPC.** Syscalls only ever
  call the mqueue layer with timeout 0. "Blocking" is
  a userspace loop: `recv_poll(); yield;` in ulib.
  Zero scheduler change; trivially safe with FND-3
  (a polling daemon keeps making progress stamps by
  construction only if it syscalls — a pure yield loop
  accrues age exactly like a wedged daemon, so ulib
  must stamp via its poll syscall, which does count:
  any non-YIELD syscall is progress under FND-3's
  model). Cost: wake latency is one round-robin cycle
  (acceptable — the same latency class as today's
  UDP/TCP poll loops in the daemons), and an idle
  system burns cycles polling. This is the *only*
  safe option for the first phase.
- **Option B — receive as a scheduler switch point
  (recommended end state).** Add a dedicated block
  operation to the extension scheduler, a sibling of
  `sched_yield_switch`: the caller daemon is marked
  `state = 2` (WAITING) with a recorded wake
  condition (a port's mqueue, plus an optional
  deadline); its context is saved; `pick_next()`
  switches to the next runnable daemon. The send path
  (running in whichever daemon sends) checks the
  wait table after a successful enqueue and flips
  the waiter back to `state = 1`. This is not a new
  invention — it is the Q9 spawn mechanism generalised:
  today a spawn parent sits in `state = 2`
  (user.c:1258) and the child's exit flips it back to
  runnable and restarts its watchdog progress clock
  (user.c:1773-1785). IPC wait adds a second producer
  of the same transition. The ulib blocking-receive
  wrapper is then: one `SYS_RECV` call with a wait
  flag; the kernel either returns a message, or
  performs the wait-switch and resumes the daemon
  inside the syscall's return path with the message
  (or a timeout / peer-died result) — the same
  "syscall appears to return later" illusion
  `ctx_save` already creates for `SYS_YIELD`
  (user.c comment at `ctx_save`: r[0] pre-set, pc =
  return address).
- **Option C — real Mach threads per daemon.** Give
  each daemon its own genuinely scheduled Mach thread
  and let `thread_block` work as designed. This is
  the threading research's rejected direction
  (cooperative model, C3) wearing an IPC costume:
  it re-opens preemption, FPU context (THR-1's
  landmine: `fpu_save`/`fpu_restore` are still empty
  stubs) and the whole FND-3 verification basis.
  Rejected.

### 3.3 Consequences Option B must honour

- **net_pump coverage.** The network is pumped only
  from the two switch points — `sched_yield_switch`
  (user.c:1657) and `sched_exit_switch` (user.c:1769).
  A wait-switch is a third switch point and must pump
  too, or a system parked on IPC starves HTTP/TCP.
  The wait path should literally reuse the yield
  path's prologue (pump, `gui_maybe_launch`,
  `wd_last_activity` stamp).
- **FND-3 class (a) accounting.** `wd_supervise()`
  already skips any daemon whose `state != 1`
  (user.c:~2423, "dead, or WAITING on spawn"). An
  IPC-waiting daemon is therefore exempt from warn/
  hang by the existing rule — but the exemption must
  be *earned the Q9 way*: on wake, restart
  `wd_last_progress` (else a daemon woken after a long
  legitimate wait is instantly overdue), and waits
  need their own observability (bootlog note class or
  counter) so a daemon parked forever on a dead peer
  is visible, not silently exempt. A wait with no
  deadline on a port whose owner died is exactly the
  FND-3 failure shape in a new costume — hence waits
  carry deadlines, and peer death completes waits
  with an error (§5).
- **FND-3 class (b).** `wd_irq_tick()` (user.c:2505,
  called from the timer IRQ, `src/md/arm/trap.c:118`)
  fires when there is no scheduler activity for 30 s
  *while a user frame is current*. If every daemon is
  IPC-parked, no daemon's frame is current and the
  boot thread sits in kernel context — the detector's
  precondition is false and it must not fire. The
  wait-switch design has to guarantee that state is
  reachable and quiescent: when `pick_next()` finds
  nothing runnable and the current daemon wants to
  wait, the syscall completes the wait by idling in
  kernel context (interrupts on, periodic re-check)
  rather than returning to a user frame. This
  all-parked idle is a new kernel behaviour and gets
  its own verify scenario in the plan (IPC-2).
- **Send never blocks.** In this design the send
  syscall always uses the timeout-0 mqueue mode:
  full queue → `MACH_SEND_TIMED_OUT`, surfaced to
  userspace as a distinct code. Backpressure policy
  belongs to callers (linkd drops/counts, RPC clients
  retry), not to a blocked boot thread.

## 4. Message transfer semantics

**Inline only, for this track.** A message is a Mach
header plus a flat byte body, copied user → kernel
(kmsg) → user. The copy path the glue uses is the
house pattern, not Mach's `vm_map_copyin` user flow:
today's syscalls validate the caller's buffer with
`user_range_ok(va, len)` (user.c:653-663 — a window
check against `[USER_VA_BASE, va_top)`, no per-page
walk; the SYS_WRITE handler's comment at user.c:~735
says so plainly) and then dereference it directly,
which is sound because the caller's pmap is the
active one in SVC context. IPC inherits both the
pattern and its known limitation; IPC does not make
the validation gap worse (the copy is bounded,
single-buffer, and the window is the caller's own),
but the plan keeps the audit's copyin/copyout item
explicitly open rather than pretending IPC closes it.
Header translation (local/remote port names →
rights in the caller's space) is done by
`ipc_kmsg_copyin` with the explicit space — that part
is real Mach validation: a daemon cannot forge a
right it does not hold, because names resolve only
inside its own space.

**Out-of-line memory: machinery present, deliberately
not wired.** Upstream supports OOL descriptors:
`vm_map_copyin` / `vm_map_copyout` in `vm/` (compiled,
standard) move page lists between maps, and
`ipc_kmsg_copyin` processes OOL descriptors. Wiring
it means descriptor parsing of untrusted input in
kernel context, page-pinning lifetime rules against
our pmap port, and a validation story we do not have
(the §4 window check is not one). No named consumer
needs it: the C6L data plane's largest unit is a
1024 B audio frame; TOOL.DISPATCH payloads are small
JSON. OOL is a named non-goal with a named trigger
(a consumer with a measured need above the inline
cap), not a silent omission.

**Inline cap.** The plan freezes a v1 payload cap of
**1024 B body + header** (one AUD frame fits exactly;
everything else is far below) with the kmsg layer
able to carry more if a later phase raises it
deliberately. Queue depth is Mach's default port
queue limit (5) unless a phase raises it per port —
the C6L hand-off (§7) prices this for linkd's audio
path.

**Port rights inside messages.** The restricted,
sufficient subset: a sender may move/copy **one send
or send-once right in the message header** (the
`msgh_local_port` reply pattern — pure header, no
body descriptors). That is what makes RPC possible
without opening complex-body parsing. Body-carried
rights (arrays of ports in descriptors) are out of
scope for the track.

## 5. Rights subset, naming, and death — the critical interaction

### 5.1 The subset

- **Receive right**: exactly one holder (the owning
  task's space). Allocated by the owner through glue
  (`mach_port_allocate(itk_space, MACH_PORT_RIGHT_
  RECEIVE, &name)`). Only the owner can receive.
- **Send right**: copyable, obtained from the
  registry (§5.2) or by message. Sending is
  validated at copyin against the sender's own space.
- **Send-once right**: for RPC replies; consumed by
  use; implemented via the header reply pattern (§4).
- **Port sets**: upstream compiled (`ipc_pset.c`),
  **not exposed in v1**. A daemon with two input
  ports polls both (Option A) or waits on one
  designated port (Option B). The C6L event fan-in
  question is re-examined at the hand-off with real
  traffic shapes in hand; if a set is needed it is an
  additive phase, not a redesign.
- **Dead names**: sending to a port whose receive
  right has been destroyed fails at translation with
  `MACH_SEND_INVALID_DEST` — the upstream behaviour,
  once destruction actually happens (§5.3).

### 5.2 Naming: the registry is the bootstrap port we never had

Mach names are per-space integers; there is no global
namespace in the kernel. Stock Mach solves rendezvous
with the bootstrap port — ours is `IP_NULL` everywhere
(§2 gap 1). The track therefore builds the rendezvous
in the extension layer, where the house keeps its
other arbiters (display token, console owner, TCP
owner — ARSITEKTUR.md's KEEP verdicts): a small
kernel-side **registry**, name (short string) →
`ipc_port_t` + generation counter, owned by the glue.
A daemon registers its receive port under a name
(the glue holds a send right to it, obtained by
insert in the owner's space / the owner's own
registration call); a client looks the name up and
the glue inserts a send right into *the client's*
space (`mach_port_insert_right(client_space, …)`).
Generation increments on every (re)registration, so
a client can detect that the "linkd" it resolved
before a crash is not the "linkd" after respawn.
This is a bootstrap-port analogue in shape and in
trust: the registry is kernel-resident, tiny, and
its table is fixed-size.

### 5.3 Death and respawn (FND-2 / FND-3 interaction)

Today (`sched_exit_switch`, user.c:1766): the dying
daemon goes through `daemon_reclaim()` (FND-2's
single funnel for both death routes) and its slot is
marked dead — but its Mach task is **never**
terminated. Nothing in `src/ext/` or `src/md/arm/`
calls `task_terminate` / `task_deallocate` /
`thread_terminate` (grep-verified). The task, its
`vm_map`, its ipc_space, every port it held and every
message queued to those ports simply leak; a respawn
(FND-3's `wd_respawn`) builds a brand-new task with a
brand-new space (user.c:2054 again). Bounded today
only by the respawn budget (2 per daemon per boot).

With IPC live, that leak becomes a correctness bug,
not a memory footnote:

- Senders keep send rights to ports in a dead space.
  The ports are not destroyed (nobody destroys them),
  so sends *succeed* into queues nobody will ever
  drain — silent black-holing, the worst failure
  mode for a link layer.
- A client WAITING (Option B) on a reply from the
  dead daemon waits until its deadline, every time.
- The registry would hand out the corpse's port.

Upstream has the tool: `task_terminate`
(`kern/task.c:729`) calls `ipc_task_terminate(task)`
(`kern/ipc_tt.c`), which destroys the task's ports;
port destruction discards queued kmsgs and turns
outstanding send rights into dead names. But calling
full `task_terminate` from `daemon_reclaim` also
tears down the task's VM map and thread set — state
the extension layer bookkeeps separately
(`struct user_task`, pmap table) and has never let
Mach own. The plan therefore phases it (IPC-3):
first a **surgical IPC teardown** inside
`daemon_reclaim` — unregister registry entries
(generation++), destroy the dead daemon's receive
ports (queued messages discarded, senders get dead
names), complete any waits *on* the dead daemon
with a peer-died result, release the space's entries
— while the task/VM leak stays as-is (pre-existing,
budget-bounded, separately tracked); full
`task_terminate` adoption is evaluated only after
the surgical path is proven, as its own decision.
The respawn side is symmetric: a respawned daemon
re-registers its names (new generation); stale
clients discover the change on their next send
(dead name) or lookup (generation mismatch) and
re-resolve. No message queued before the death is
ever delivered after it — that sentence is the
phase's acceptance test.

## 6. The syscall surface

The reservation is real and still free.
`docs/SYSCALL-ABI.md:134-135` reserves 10–12 for
`SYS_SEND` / `SYS_RECV` / `SYS_RPC` (IPC) and 23 for
`SYS_RPC_USER`. In the dispatch (`src/ext/user.c`
defines, ~line 256+), 10, 11, 12 and 23 are
unassigned — bracketed by `SYS_EXIT 22` and
`SYS_SBRK 24` — and `user_syscall` has no case for
them (a caller today gets the default error return).
The ABI stays in house shape: number in `r7`, args
in `r0`–`r5`, `svc #0` (the ulib convention,
`user/ulib/ulib.c:5`), result in `r0`, errors as
negative codes (house precedent: the bootstate
sub-operations' −2/−3 style). Mach return codes are
mapped onto a small documented negative set rather
than passed through raw — userspace must not grow a
dependency on upstream's numeric values.

Proposed concrete surface (frozen by the plan's
first phase; management operations ride sub-ops on
one additional number from the free 25–29 block,
since the reserved four are message operations):

| Nr | Name | Shape |
|---|---|---|
| 10 | `SYS_SEND` | (port_name, buf, len, timeout_ms=0-only) → 0 / − codes: invalid dest (dead), timed out (queue full), too large, invalid right |
| 11 | `SYS_RECV` | (port_name, buf, max, flags/timeout) → len / − codes: empty (poll), timed out, too large (message kept? no — Mach semantics: too-large leaves it queued; document), peer died, interrupted by death of self n/a |
| 12 | `SYS_RPC` | (port_name, req buf/len, resp buf/max, timeout) → resp len / − codes; glue-composed: send + wait on the caller's own reply port (header send-once pattern) |
| 23 | `SYS_RPC_USER` | (name string, req, resp, timeout) → as SYS_RPC but resolves the registered name first — the one-shot-friendly form |
| 25* | `SYS_IPC_PORT` | sub-ops: ALLOC_RECV → name; REGISTER(name string, port) ; LOOKUP(name string) → send name + generation; DESTROY(port); SETQLIMIT(port, n) (*number proposed, to be ratified in IPC-1 and backfilled into SYSCALL-ABI.md) |

**Not** a `mach_msg_trap` analogue: the trap ABI
assumes `current_thread()` identity (§2 gap 2) and a
`thread_syscall_return` this MD port never implemented
(trap.c:205). The split send/recv shape also matches
the frozen reservation names and the ABI's existing
poll-shaped style (cf. `SYS_TCP_RECV` returning 0 for
"nothing yet"). One-shot programs use the same
syscalls: outside the daemon scheduler there is no
wait-switch, so for them `SYS_RECV` is poll-only and
`SYS_RPC`'s wait degrades to a bounded yield/poll —
documented semantics, identical results for the
sequential test flows.

## 7. Size and RAM cost

Measured on the current build (2026-10-24,
`build/mach3.elf` + `llvm-size-18`):

- The 17 `ipc/*.o` + 5 `kern/ipc_*.o` objects total
  **87,024 B (text 86,992 + data 32)** — already inside
  today's image. This track does not pay it again.
- Whole-image reference: ELF `.text` 3,133,280 B +
  `.data` 4,160 B; `objcopy -O binary` kernel image
  3,148,148 B against the 4,194,304 B boot partition
  (`scripts/update-img/parameter.txt`).
- **Measurement discrepancy, flagged honestly:** the
  UI research recorded ~287–291 KB of headroom
  (text+data 3,907,484 B), which the current ELF does
  not reproduce by any of the measures above (they
  imply ~1.0 MB). One of the two accounings is wrong
  (different section set, an older fatter build, or
  the update-image slot accounting). The IPC plan
  spends from the *pessimistic* figure until its
  first phase re-measures partition occupancy with
  the update-image tooling and reconciles the two
  numbers; nothing in this track's design depends on
  the optimistic one.
- Incremental cost of the track itself is glue, not
  machinery: the user.c dispatch cases + registry +
  wait table (est. 250–400 lines ≈ 4–8 KB text),
  ulib wrappers (~1 KB into each program that links
  them — only consumers), two test programs
  (~4 KB each embedded, test builds only via the
  embed-hook pattern). Even under the pessimistic
  headroom figure this is two orders of magnitude
  inside budget. RAM: ipc spaces grow their entry
  tables on demand from zones that `ipc_init` already
  sizes at boot; per-port cost is one `ipc_port` +
  queue nodes per queued message (kmsg zone,
  bounded by the 1024 B cap × queue limit 5 per
  port). The 64 MB RAM budget does not notice this
  track; the NAND image budget is the only one
  watching, and it is not close.

## 8. Risks, and what IPC must not become

- **Scope creep into a server framework.** The
  owner's decision makes IPC a *second, sanctioned*
  mechanism; the resolutions text is explicit that
  per-service syscall surfaces remain the pattern for
  existing services. This track must not migrate
  display, storage, credstore or any existing service
  onto IPC, must not grow a general "server" runtime,
  and must not decompose the netstack (D-ARCH-3's
  trigger is the httpd move and C6L arbitration, not
  IPC enthusiasm). If a future phase wants a service
  on IPC, it argues its own case then.
- **The blocking design is the risk concentration.**
  Every catastrophic version of this track is a
  version where a Mach thread blocks (§3). The
  plan's hard rule: no code path from a user syscall
  may reach `thread_block`, `thread_will_wait`, or
  the mqueue blocking branch; enforced by
  construction (timeout-0 at the mqueue calls) and
  by a verify scenario that would hang the boot if
  violated (and is therefore run behind the battery's
  timeout discipline).
- **Death semantics arriving late.** If message
  passing lands before lifecycle work, the system
  ships a black-hole window. The plan sequences
  lifecycle (IPC-3) before the registry/RPC polish
  and *before* any C6L consumer depends on it.
- **Two rendezvous systems.** The registry must not
  grow into a second policy layer beside the existing
  arbiters; it hands out send rights and generations,
  nothing else. Access policy stays where the house
  keeps it: the policy gate in qabotd for tools, the
  kernel arbiters for devices.
- **ABI freeze pressure.** C6L will consume `uipc.h`;
  changing it after C6L-1 freezes the client stub is
  a cross-track break. The plan freezes the ulib API
  at IPC-4 and treats later change as a breaking
  change requiring both tracks to move together.

## 9. Open questions for the owner (D-IPC-*)

Kept few, and each is a real fork:

- **D-IPC-1 — Blocking receive as a sanctioned third
  switch point.** The design (§3, Option B) makes an
  IPC wait a scheduler switch point beside yield and
  exit, with the FND-3 watchdog treating waiters as
  exempt-but-observed (deadline required, wake
  restarts the progress clock, peer death completes
  the wait). This changes an invariant FND-3 was
  verified against. Alternative is Option A forever
  (poll-only; simpler; wake latency = one scheduler
  cycle; idle burn). *Recommendation: adopt B as
  designed; A is phase 1 either way.*
- **D-IPC-2 — Registry trust model.** Flat appliance
  model (any daemon may register or look up any name;
  generations detect staleness, nothing prevents
  squatting) vs reserved system names (the glue
  refuses registration of e.g. `linkd`, `tools`
  except by their owning progid). *Recommendation:
  flat in v1 with generation checks; name-squatting
  on a single-owner appliance is a bug, not an
  attacker — revisit if Mode B ever exposes IPC
  names off-device (it must not).*
- **D-IPC-3 — Inline cap and OOL deferral.** Freeze
  the v1 inline body cap at 1024 B and defer
  out-of-line memory indefinitely (trigger: a named
  consumer with a measured need). This caps
  TOOL.DISPATCH payloads and any future bulk
  transfer over IPC; bulk data keeps using the
  existing surfaces (files, TCP) meanwhile.
  *Recommendation: freeze as stated.*

## 10. Bottom line for the plan

Build order follows the risk: (1) prove the
parameterised Mach layer works from the glue with
poll-only send/receive between test programs;
(2) add the wait-switch as a generalised Q9 WAITING;
(3) wire death before anyone depends on it;
(4) registry + RPC shape, freeze the ulib API;
(5) harden the C6L hand-off (queue sizing, fan-in
verdict, ABI/doc backfill). No MI changes are
expected anywhere in the track; every change lives in
`src/ext/` (glue), `user/ulib` + consumers (userspace),
`scripts/` (verify) and `docs/`. If a phase discovers
it needs an MI or MD change beyond the trap-adjacent
glue, that is a stop-and-surface event, not an
improvisation.
