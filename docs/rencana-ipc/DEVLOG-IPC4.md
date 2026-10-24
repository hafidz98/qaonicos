# DEVLOG — IPC-4: kernel registry + RPC + uipc.h v1 freeze

Branch: `dev/ipc4` (worktree `~/workspace/qaonic_os-ipc4`), base: the
IPC-3 merge (`daf612c`). Status: **implemented, verified, battery
green** (see §9). This phase closes the IPC track's API surface:
C6L builds against exactly what is frozen in §7.

Predecessors: `DEVLOG-IPC1.md` (poll send/receive, the mscount
epoch fix), `DEVLOG-IPC2.md` (blocking receive as the third
switch point), `DEVLOG-IPC3.md` (surgical teardown, peer-died
EPEER). Plan: `docs/rencana-ipc/PLAN.md` (IPC-4 section +
Resolutions D-IPC-2, D-IPC-3).

## 1. Design as built

### 1.1 Registry (`mach3-port/src/ext/ipc_glue.c`)

A fixed table of **8 entries** living in the kernel, not in any
daemon, so it survives the registered daemon's death and answers
lookups during its respawn window. Each entry: name (≤ 15 chars
+ NUL), `ipc_port_t` port (`IP_NULL` = slot free), owner
(kernel progid, −1 = one-shot), generation (unsigned), alive
flag.

- **REGISTER** (IPCP 3): binds a name to the caller's receive
  port. The right is resolved via `ipc_mqueue_copyin` (which
  returns the queue **locked** — code that copyins without
  receiving must `imq_unlock` at once; the reference alone
  pins the port — this unlock was missing in the first
  implementation and was added to REGISTER and to the
  DESTROY hook). A send name, dead name, or port set is
  refused (non-receive → EINVAL, unresolvable → EDEST).
  Existing entry: live + different owner → EBUSY; same
  owner, or a dead entry taken over → replace the port
  (a same-object re-register keeps its reference; a
  different object releases the old reference first) and
  bump the generation. New name: first free slot, else
  evict the **oldest dead** entry (release its reference,
  clear the name); a live-full table → EBUSY.
- **LOOKUP** (IPCP 4): no entry → ENORV; dead entry →
  EDEST — it never mints a right to a dead port (upstream
  `ipc_port_make_send` asserts `ip_active(port)`, so
  minting one would halt the CPU). The "answer during the
  respawn window" is therefore a truthful EDEST, not a live
  right; clients retry (the test client does, wall-clock
  bounded). Live entry → `ipc_port_make_send` +
  `ipc_port_copyout_send` (NULL → EDEST defensive, copyout
  failure → ENOMEM).
- **REG_GEN** (IPCP 5): no entry → ENORV, else the
  generation.
- **Death hook** `ipc_glue_registry_dead(obj)`: marks every
  entry holding that port object dead and bumps its
  generation. Called from teardown pass 1 (after each
  receive destroy, object pinned) and from the IPCP_DESTROY
  hook (after a live owner's own receive-port destroy, only
  when the name really was a receive right — see §5.3 for
  the mask fix that made this hook actually fire).

**Generation semantics.** First registration → generation 1.
Death (teardown or live destroy) → generation++. Takeover
re-registration → generation++. One documented reset: an
entry evicted as oldest-dead is *gone*; if its name is
registered again later it starts at generation 1, not
bumped — eviction is not death, it is forgetting. A stale
send right (old generation) fails on use with dead-name:
the port it names is destroyed, so the next send's copyin
detects a dead destination → EDEST on the first attempt
(IPC-3, proven again here by the regen leg).

### 1.2 Reserved names, enforced by spawn identity (D-IPC-2)

Reserved table: `tools` → owner 2 (qabotd's
`daemon_images` index); `link` → sentinel unbound (−2,
always refused until C6L-5 binds linkd's index). The check
runs **before any table mutation** (a refused claim has no
side effect — proven by the lookup-still-ENORV probe) and
returns the distinct error **UIPC_EPERM (−13)**.

The enforcement key is `cur_progid`: the kernel-assigned
spawn identity, set by `sched_activate()` from
`d->progid`, passed by the SYS_IPC_PORT dispatch into the
glue. It is never a caller-supplied id: a daemon's progid
is the `daemon_images` index the kernel itself spawned;
one-shot programs spawned by sh get their spawn identity.
The mock `tools` server proves both sides of this: the
same binary spawned with the kernel-assigned progid
override 2 (the qabotd index, same precedent as
`ufaultd`'s PROG_UIAPP override) claims `tools`; spawned
without the override, and sh itself (progid 4), get EPERM.
Non-reserved names stay open to any caller.

### 1.3 RPC shape (SYS_RPC 12 / SYS_RPC_USER 23)

`sys_rpc_call()` in `user.c`, shared by both syscalls
(RPC_USER resolves the name via LOOKUP first and passes
lookup errors through):

1. Validate: len ≥ 24 and `hdr.reply != 0` — an RPC
   without a reply port is a caller bug, not a wait
   (EINVAL). The `uipc_rpc` wrapper always sets one.
2. Send via `ipc_glue_send_ex` (COPY_SEND, never
   SEND_ONCE for a request).
3. On success, the shared peer-note helper
   (`rpc_peer_note`, factored out of the SYS_SEND case):
   on a successful reply-carrying COPY_SEND, release the
   old pending reference first, take a reference on the
   destination object, store it with the reply name.
   Failed sends, send-once replies, and no-reply sends
   stage nothing.
4. Wait: one-shot callers (no scheduler) get a single
   poll (`ipc_glue_recv`, EEMPTY if no reply yet);
   daemon callers wait with `sys_recv_block()` on the
   reply port, same buffer, same timeout encoding
   (0 = poll, > 0 = deadline ms, −1 = no deadline).
   If the park switches away, the syscall returns
   UIPC_EPARK and the ulib wrapper continues the wait
   with a plain `uipc_recv()` on the reply port — it
   **never re-issues SYS_RPC** (which would duplicate
   the request). The wait record's original deadline
   governs re-dispatch, so continuing with the original
   timeout is safe.
5. Failure paths: a dead server mid-RPC completes via
   IPC-3's peer annotation → **EPEER** (never a hang);
   a deadline expiry → ETIME. A reply larger than the
   request buffer returns **E2BIG and the message stays
   queued** (Mach semantics) — the wrapper destroys its
   reply port on E2BIG, discarding that queued reply.
   A delivered notify id 71 is mapped to EPEER; reply
   ids must never be 71.

The wrapper contract (`uipc_rpc` / `uipc_rpc_user`):
allocate a fresh reply port, set `hdr.reply`, destroy the
reply port on **every** return path (including
EPEER/ETIME/E2BIG) so callers may loop RPCs without
leaking ports; the reply is written over the request in
the same buffer; len is the request length *and* the
reply budget — callers with a larger reply than request
must pad the request to the reply size (the test client's
dispatch does: 20-byte body both ways).

### 1.4 The mock `tools` server (C6L-3's consumer surface)

`user/uiptoolssrv.c`, an embed-hook spawn child with
progid override 2, registered as `tools`. Loop: blocking
recv with a 30,000 ms deadline, counting message-wakes vs
deadline-wakes; 2 consecutive ETIMEs → print the wakes
summary, `idle-exit`, exit 0. Methods:

- echo id `0x3000+s`: reply = request body XOR `0xA5`,
  same id;
- dispatch id `0x3100`: request body `[0xD15, arg]` →
  reply `[0, 3, arg+1, arg+2, arg+3]`, 20-byte body — the
  C6L-3 structured-result shape (a dispatch returning a
  structured result the caller validates);
- double id `0x3200`: reply OK once, then reply again on
  the consumed send-once name (the once name is saved
  *before* the first reply because `reply_now()` zeroes
  `msg.reply`) — the second attempt must fail;
- die id `0x3300`: exit *without* replying — the client
  must get EPEER (plus the kernel's send-once notice,
  id 71, on the first park, per IPC-3 §P5);
- stop id `0x3400`: reply OK, print the wakes summary,
  exit.

The client (`user/uipcld.c`) is a **daemon_images entry**,
not a spawn child — it must run alongside the embed-hook
server, the first pair of live IPC speakers in this track
(see §6 for the send-wake proof). It is marker-gated: at
startup it mounts /data and reads `iptoolsc-test`; unless
the content is exactly `"1"` it prints one
`UIPCLD: dormant` line and exits 0, so every other boot
is unaffected. Its lookup retry is **wall-clock based**
(60,000 ms via `sys_uptime()`, yielding between
attempts): the original fixed 600-yield loop provably
burned out in seconds, before the server existed (the
server only spawns after `gui` + web-shell latency).

Daemon table change (load-bearing): `SH_IMAGE_IDX =
NDAEMON_IMAGES − 1` — sh **must** be the last entry. New
order: face=0, uiapp=1, qabotd=2, **uipcld=3, sh=4**
(uipcld inserted before sh). The watchdog switch in
`launch_daemon` moved sh's case 3→4 (uipcld at 3 takes
the default 15/30 s class). `gui_maybe_launch` is generic
(loops all images except sh), so uipcld launches on
every `gui` — verified by code read.

## 2. Rendezvous deletion + migration

Deleted (via `git rm`): the RV_PUBLISH/RESOLVE/CLAIM
sub-ops (IPCP 3/4/5 — **numbers reused** by
REGISTER/LOOKUP/REG_GEN), the RV slots, and the test
programs `uipcsrv`, `uipccli`, `uipksrv`, `uipkcli`,
`uipcdsetup`, `uipcdclean`, `uipcdfault`, `uipcdwedge`,
`uipcdrain`. Deleted scripts: `verify-ipc1.py`,
`verify-ipc3.py`. `verify-ipc2.py`'s pair section
(uipksrv/uipkcli round-trip) is replaced by a NOTE block:
the same 100-message byte-exact round-trip now runs
concurrently between uipcld and uiptoolssrv in
`verify-ipc4.py`; the park probes (`uipkpark`/`uipknd`,
no rendezvous use) and the FND-3 adjacency and boot-3
ev=17 checks stay. Grep audit after retirement: zero
`uipc_test_*` / `RV_PUBLISH` / `RV_RESOLVE` / `RV_CLAIM`
/ deleted-program references remain in `user/`,
`mach3-port/src/ext/`, `scripts/` (docs history
excluded — historical dev logs and the plan still mention
them, which is correct: historical docs are never
edited).

Consequence of the number reuse, stated plainly: "the
deleted surface returns not-implemented" **cannot** be
tested by sub-op number (3/4/5 now do registry work).
It is proven behaviorally instead: a REGISTERed port
does **not** survive its owner's exit — lookup after
owner death returns EDEST, not a live port (the
`uipcregexit` leg) — which is the exact anti-rendezvous
property (the deleted RV's parked right *could* be
claimed by anyone after the publisher's death).

Assertion mapping (so the coordinator can audit that
nothing was dropped silently):

| Retired assertion | Where it lives now |
|---|---|
| IPC1 poll send/recv, qlimit, empty/full/dead/oversize (29 checks) | verify-ipc4.py boot 1: qlimit 2, self-send ×2 + EFULL, pollrecv drain + EEMPTY, sendbig E2BIG, destroy → EDEST; plus the concurrent echo 100/100 byte-exact (below) |
| IPC2 pair round-trip 100/100 (part of 57) | verify-ipc4.py boot 2: uipcld ↔ uiptoolssrv echo 100/100 byte-exact, concurrent |
| IPC2 park probes, FND-3 adjacency, ev=17 (rest of 57) | verify-ipc2.py, unchanged |
| IPC3 death legs: send-after-death EDEST, drain EMPTY, peer EPEER, ev 18 (30 checks) | verify-ipc4.py boot 1 (regexit death → EDEST + gen 2, ev 18 via teardown) and boot 2 (DIE → EPEER; second-reply failure; regen EDEST + gen 2) |
| IPC3 harness constraint (a spawn child cannot spawn) | Inherited: IPC-4's legs are sh-driven builtins + embed-hook servers, same pattern |

## 3. The three bugs found by testing (and one harness fix)

### 3.1 The ulib stub r0 bug — a latent defect since IPC-1

Symptom: the tools server registered `tools` fine, a
poll-recv right after answered EEMPTY (the port name
was a live receive right), but the first *blocking*
recv returned EDEST. A temporary kernel debug print
showed the failing call's arguments: same task as the
register, but **port = 4294967285 = −11 = UIPC_EPARK**.

Disassembly of `uipc_recv` in the built `ulib.o` proved
it:

```
mov r12, r0        ; ret = r0
cmn r12, #11       ; compare with -11 (EPARK)
beq loop           ; loop back to: mov r7, #11; svc
                   ; WITHOUT reloading r0-r3
```

The IPC stubs' inline asm used pure input constraints
(`"r"`) for r0–r3, but the SVC trap overwrites r0 with
the return value (trap.c writes `user_syscall()`'s
return over the frame's r0; r1–r3 are restored). The
compiler was entitled to assume the inputs survived the
asm, so the EPARK re-issue ran with r0 = −11 (the
previous return value) — and the glue rightly answered
EDEST for a nonexistent port. IPC-2's pair passed only
because its build happened to allocate the argument
reload; IPC-4's ulib.c changes shifted allocation and
exposed it.

Fix: all five IPC stubs (`sys_ipc_send`,
`sys_ipc_recv`, `sys_ipc_rpc`, `sys_ipc_rpc_user`,
`sys_ipc_port`) now model r0 as read-write (`"+r"`),
with a comment stating why; r1–r3 stay pure inputs
(the trap genuinely preserves them). After the fix the
loop reloads the port from its saved register each
iteration (verified in disassembly), and the server
parks cleanly (2 deadline wakes → idle-exit, exit 0).
The same pattern in the non-IPC stubs is a noted latent
issue for a future sweep (their callers do not loop on
a return value today, so it does not bite there).

### 3.2 The coalesced RECEIVE|SEND entry — one cause, two bugs

Symptom 1: after sh registered `myname`, looked it up,
then destroyed its receive port, `gen myname` still
read 1 — the DESTROY hook's `registry_dead` had not
fired. Symptom 2: the table-full leg failed all three
checks on a table with prior dead entries, while
passing on a fresh table.

The link: sh's lookup of its own registered name
returned send name **768 — identical to its receive
name**. Upstream copyout coalesces a send right for a
port the task already holds receive for into the
existing entry, so the entry's bits read
RECEIVE|SEND. Three sites tested type with exact
equality (`== MACH_PORT_TYPE_RECEIVE`): the DESTROY
hook's `was_recv`, REGISTER's validation, and
teardown pass 1's receive detection. All three missed
coalesced entries. For symptom 1, the destroy skipped
`registry_dead`, leaving `myname` alive. For symptom
2, that live `myname` plus the dead `gone` left only
six free slots: the leg's r6 evicted `gone`, then r7
hit a live-full table → EBUSY → the leg failed before
printing `ok 8/8` — exactly the observed all-three
failure.

Fix: all three sites now use a mask test
(`(type & MACH_PORT_TYPE_RECEIVE) != 0`): an entry
holding receive rights *is* a receive right. After
the fix, gen-after-destroy reads 2 and the table-full
leg passes in the full sequence. The teardown
consequence is worth stating: a coalesced entry
skipped in pass 1 would previously have been destroyed
in pass 2 *without* `registry_dead` — leaving a live
registry entry pointing at a corpse port, whose next
lookup would mint a send right to it and trip the
`ip_active` assert. That path is now closed.

### 3.3 The async web-shell teardown — harness only

The verify script's teardown (`put` the marker back to
0 via POST /shell) failed twice in two different ways:
run 1 the put never executed; run 2 it executed but
the check failed. Root cause, read from `http.c`:
POST /shell only **queues** the command (the response
is a "running" page; the output arrives on a later GET
refresh), and a busy executor answers "Shell sibuk"
and drops the command. The old single-POST body check
(`"put: ok" in body`) could therefore never pass.
Fix (script only, no product code): submit until
accepted (retry past "Shell sibuk"), then poll GET
/shell until the output contains "put: ok". The
marker itself is hygiene, not an IPC assertion; boot 3
re-checks it independently. No IPC-4 code was touched
for this.

### 3.4 Build-verification discipline (re-learned the hard way)

A rebuild failed on a temporary debug print, but the
command piped through `grep -E "FAIL|MD:|LINK OK"`,
grep exited 0 on matching "FAIL", and the `&&` chain
ran probes against a stale kernel anyway (a failed
kernel-object compile can leave a stale object that a
later link reuses, mixing new user binaries with old
glue code). Rule, now in force for this track: a build
counts only on its own exit code **plus** the MD line
**plus** LINK OK **plus** `strings` proof that the
intended code is in the ELF. The temporary `ipc4dbg`
prints are removed; the server poll-after-register
diagnostic added during debugging is also removed
(the final script does not assert it).

## 4. Test evidence

### 4.1 Pre-change (test-first)

User-side built, kernel-side not yet landed (commit
`462b802` + build fix `3490095`): the run reached boot
1 + into boot 2 before being killed deliberately (the
client flow was dead and every remaining check was a
timeout-bound consequence, and a rebuilt kernel had
replaced the ELF on disk, which would have corrupted
the "pre-change" label). Counts at kill time: **18
PASS / 18 FAIL**, and the FAILs are exactly the
discriminating ones: lookup returned a port name, not
generation 1 (REG_GEN aliased to old RV_CLAIM);
register moved the receive right out of sh's space
(RV_PUBLISH semantics), breaking the downstream
qlimit/EFULL/pollrecv/destroy checks; lookup of a
dead-owner entry returned a **live** port (RV_RESOLVE)
instead of EDEST; all three table-full checks failed
(RV table: 4 slots, not 8); the client flow died at
lookup gen 1. A partial log from this run was committed
in `38a5745` as `logs/ipc4-prechange.log` — it is
**partial** (killed mid-boot-2), not a completed run;
the evidence above comes from the kill-time counts
and the discriminating FAILs observed live. No
derived final-script baseline is claimed: the run
was killed before the script's later checks could
execute, and a stash-based re-run was
methodologically unsound (the test hooks live in
`user.c`, so stashing the kernel also removed them).

### 4.2 Post-change

`scripts/verify-ipc4.py`, three boots (dev typed
serial; GUI + web shell; same disk):

- Boot 1: lookup-before-register ENORV; reserved
  `tools` from sh EPERM; refused claim has no side
  effect (lookup still ENORV); unbound `link` EPERM;
  register ok; lookup send + gen 1; gen 1; qlimit 2;
  self-send ×2 + EFULL; pollrecv drain ×2 + EEMPTY;
  sendbig E2BIG; destroy ok; send-after-destroy
  EDEST; regexit registers + exits 0; lookup `gone`
  EDEST (not a live port); death bumped gen to 2;
  regfull ok 8/8 + 9th EBUSY; no FAULT;
  fail_count=0.
- Boot 2: web login (PIN 246810, first-login-creates
  semantics) → settings shell_on → marker 1 →
  `gui` → server#1 registered `tools` → client
  lookup gen 1 → echo 100/100 byte-exact → dispatch
  OK → server second-reply failure → client double
  first reply → DIE → EPEER (−12) → re-lookup EDEST
  + gen 2 → server incarnation 2 → lookup2 gen 3 →
  echo2 10/10 → DONE → teardown marker 0.
- Boot 3: prompt, fail_count=0, marker reads back 0
  (daemon dormant again).

Final: **IPC4-VERIFY: PASS (58/58 checks)**. Two
non-IPC flakes were observed and handled openly on the
way there: the async teardown (§3.3, fixed in the
script) and one run's boot-1 `fail_count=1` from the
known pre-existing intermittent SD/fat one-shot timing
flake (2 one-shot FAILs skipped the healthy mark;
re-run passed — same signature as the FND-3/IPC-2
battery notes, no IPC code path in the one-shot flow).

### 4.3 Concurrent send-wake proof (IPC-2's gap, closed)

The concurrent flow's wall time was **0.2 s** for
100+ RPCs (deadline-driven would take 100×30 s); the
server's wake totals across the flow were **msg=114,
etime=0** (100 echo + dispatch + double + die + 10
echo2 + stop; zero deadline wakes). A parked server
woken by a concurrent client's send, end to end, is
therefore proven — the first phase with two live IPC
speakers. (The server's idle-exit path, 2 deadline
wakes with no traffic, is proven separately by the
server-only probe.)

## 5. Headroom

Raw kernel binary (`llvm-objcopy -O binary`) post-IPC-4:
**3,313,416 B**; headroom against the 4 MiB boot
partition: **880,888 B** (down from 921,940 B at IPC-3;
IPC-4's all-in delta is +41,052 B: registry + RPC +
test programs + sh builtins + ulib growth). The
reconciled headroom figure, not the retracted ~287 KB
one, remains the budget; UI-1 re-measures at its start
per the standing rule.

## 6. Honest limitations (by design, not bugs)

- Queue sizing and port-set fan-in are **IPC-5**, not
  here: qlimit max stays 16, body cap 4096 B (a 16-deep
  queue is up to ~66 KB kmsg zone per port at the cap).
- v1 tracks only the most recent outstanding RPC per
  waiter (IPC-3's limitation, carried over); parking on
  an older reply port after a newer request is
  unannotated (its deadline still bounds it).
- Registry eviction forgets: an evicted name
  re-registered later starts at generation 1 (§1.1).
- Lookup of a dead entry answers EDEST; clients must
  retry across the respawn window (the test client
  does). There is no queued-wake for a future
  registration.
- Task objects, `vm_map`s, and `pmap`s still leak on
  death (pre-existing, bounded by the FND-3 respawn
  budget for daemons; IPC-3/4 freed ports + queued
  kmsgs, which previously leaked too). Ramfs fd tables
  still leak per task (`RAMFS_MAX_TASKS` 16, found in
  IPC-1, FND-2-family follow-up) — untouched here.
- The non-IPC svc stubs share the §3.1 r0-constraint
  pattern; only the IPC stubs are fixed here.
- The web-shell teardown flakiness (§3.3) is harness
  evidence about a pre-existing subsystem, not an
  IPC-4 behavior.

## 7. uipc.h v1 freeze (C6L builds against this)

`user/ulib/uipc.h` carries the freeze comment block
(version marker, cap, tables, reserved names, and the
explicit note that queue sizing and port-set fan-in are
IPC-5). Frozen:

- Message geometry: 24-byte header, body cap **4096 B**
  (D-IPC-3), total max 4120 B, wire rounded up to a
  multiple of 4 with zero padding. COMPLEX bit never
  set.
- Syscalls: 10 SEND, 11 RECV (r3 timeout: 0 = poll,
  > 0 = deadline ms, −1 = no deadline), **12 RPC**,
  **23 RPC_USER**, 25 IPC_PORT.
- IPC_PORT sub-ops: 0 ALLOC_RECV, 1 DESTROY,
  2 SETQLIMIT (1..16), **3 REGISTER, 4 LOOKUP,
  5 REG_GEN** (reused from the deleted RV_*).
- Errors: −1 EINVAL, −2 dispatch ENOSYS (never reused),
  −3 EDEST, −4 EFULL, −5 EEMPTY, −6 E2BIG, −7 EBUSY
  (registry: live entry owned by a different caller,
  or table full of live entries), −8 ENOMEM, −9 ENORV
  (registry: name never registered), −10 ETIME,
  −11 EPARK (internal, never to userspace), −12 EPEER,
  **−13 EPERM** (reserved name claimed by a non-owner,
  or an unbound reserved name claimed by anyone; no
  side effect).
- Reserved names: `tools` (owner: qabotd, daemon_images
  index 2), `link` (unbound until C6L-5 binds linkd).
  C6L-1 may not depend on anything beyond this block.

## 8. Commits on dev/ipc4

- `462b802` — user-side (uipc.h v1, ulib RPC, sh ipc
  builtins, tools server + client daemon, rendezvous
  retirement) + verify script + retirements. Kernel
  side not in the commit; the script is expected to
  FAIL against that build (test-first).
- `3490095` — user-side build fixes (uipc.h include
  in ulib.c, uipcld_img externs). Pre-change build
  green: MD 43 ok, LINK OK.
- `38a5745` — kernel registry + RPC engine,
  rendezvous deleted. (Also contains the partial
  `logs/ipc4-prechange.log` — see §4.1.)
- This commit: stub r0 fix, mask fix, dispatch padding,
  client wall-clock retry, teardown fix, this dev log,
  battery + soak results.

## 9. Battery + soak results

Battery, standalone, in order (worktree disk, post-IPC-4 kernel):

| Leg | Result |
|---|---|
| SHELL (`verify-shell.sh`) | PASS |
| SAFEMODE (`verify-safemode.sh`) | PASS |
| FIRSTBOOT (`verify-firstboot.sh`) | PASS standalone after a `setup reset` boot. In battery order it failed first (boot 1 requires an unprovisioned disk; the IPC-4 verify runs' web /settings POST had provisioned it) — the known precondition sensitivity from IPC-3, not a regression. The reset boot reported "provisioning cleared", "NOT provisioned", fail_count=0; the standalone re-run passed, and its own boot 2 leaves the disk unprovisioned again. |
| UPDATE (`verify-update.sh`) | PASS |
| ONB1 (`verify-onb1.py`) | PASS |
| ONB2 (`verify-onb2.py`) | PASS |
| FND1 (`verify-fnd1.py`) | PASS |
| FND2 (`verify-fnd2.py`) | PASS (21/21 checks) |
| FND3 (`verify-fnd3.py`) | PASS (46/46 checks), first attempt, disk left healthy |
| FND4 (`verify-fnd4.py`) | PASS (36/36) |
| FND5 (`verify-fnd5.py`) | PASS (33/33) |
| IPC2 (`verify-ipc2.py`) | PASS (34/34 checks). Count is down from 57 because the pair round-trip section migrated to verify-ipc4.py (see §2); the remaining 34 are the park probes, the FND-3 adjacency check, the boot-3 ev=17 check, and cleanup. |
| IPC4 (`verify-ipc4.py`) | PASS (58/58 checks), twice: standalone and as the battery tail |

IPC1 (29/29) and IPC3 (30/30) no longer exist as
scripts — they were retired with the rendezvous (§2);
their assertions run inside verify-ipc4.py, which
passes 58/58.

Soak (`scripts/soak-test.py`, fresh log — the previous
`logs/soak-results.log` was archived to
`logs/soak-results-pre-ipc4.log` first, per the resume
trap): **SOAK: PASS, fails=0** — 100/100 cold boots
reaching the shell prompt with the core PASS markers
and no panic/FAULT, 30/30 random power cuts (SIGKILL
mid-boot), final clean boot with littlefs + bootstate
intact. The worktree disk was left healthy and
unprovisioned (final `setup reset` boot: "NOT
provisioned", fail_count=0).
