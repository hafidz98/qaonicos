# DEVLOG-K1 — FPU/NEON save/restore (THR-1)

Branch: `dev/k1` (worktree `~/workspace/qaonic_os-k1`).
Base: `qabot/q2-tls-provider` @ `3d889c9`.
Status: implemented, verified test-first, full battery green
(see §5 for the complete history, including flakes and reruns).
Not merged, not pushed — coordinator review/merge only.

## 1. Problem

VFP/NEON was enabled (`fpu_init` sets CPACR CP10/CP11 + FPEXC),
but `fpu_save`/`fpu_restore` were empty stubs. Any thread using
floating-point/NEON registers risked silent corruption across
switches: the next thread to run inherited the previous thread's
`d0-d31` + FPSCR.

## 2. Design

### 2.1 Where the state lives (R14)

Per-thread FPU state lives in MD thread context — the justified
placement. No userspace workarounds, no new kernel resident.

- `struct arm_fpu_state` in `mach3-port/src/md/arm/thread.h`,
  inside `struct arm_machine_state` (`mms`, previously a 1-word
  placeholder with zero users):
  32 double regs as 64 words (`d0-d31`, aliased `s0-s31`/`q0-q15`,
  so NEON shares the same file) + FPSCR + pad = **264 bytes**.
- `mms` sits at PCB offset **120** (`iss` 80 + `kss` 40).
- `mach3-port/src/md/arm/pcb.c` carries compile-time guards
  (`offsetof(struct pcb, mms) == 120`,
  `sizeof(struct arm_fpu_state) == 264`): a layout drift fails
  the build, not the boot.

### 2.2 Single implementation in `context.s`

All switches funnel through `__Switch_context`, so the
save/restore lives there and nowhere else. `fpu.c`'s empty
`fpu_save`/`fpu_restore` stubs were deleted (replaced by a comment
pointing at `context.s`); there are deliberately no C wrappers, so
no second copy can drift. `fpu_init` is unchanged (FPEXC stays
globally enabled; it is not per-thread state).

- File-local `.fpu neon-vfpv4` directive (the kernel build has no
  `-mfpu` flag; C inline asm with VFP was verified to fail without
  `-mfpu=neon -mfloat-abi=softfp`, which is why the switch-path
  code is asm, not C).
- `.set PCB_FPU, 120`.
- Save in `__Switch_context`, under exactly the same guard as the
  `kss` save (`continuation == NULL && old_pcb != NULL`), keyed on
  **old_pcb identity (r0)** — never on the preempting frame's mode
  (Fase 10 lesson). A continuation-discarded thread never resumes,
  so its FPU state needs no saving, exactly like its general
  registers:
  `vstmia {d0-d15}; vstmia {d16-d31}; vmrs fpscr; str`.
- Restore in `__Switch_context` (always runs, including first
  switch-in): `vldmia {d0-d15}; vldmia {d16-d31}; ldr fpscr; vmsr`.
  Uses only r0/r3 (r1 = new_thread, r2 = old_thread return value,
  r12 = new kss.sp, lr = resumption PC all survive — re-verified
  by reading the final code).
- Restore in `_load_context` (first entry, loads the
  `pcb_init`-zeroed state so a new thread never inherits the
  previous thread's registers).
- Eager save/restore on every switch. No lazy switching, no fault
  path.

### 2.3 Switch paths covered

- Cooperative switches: `switch_context()` (MI scheduler path,
  incl. `thread_block`/`thread_dispatch`) and `stack_handoff()`.
- IPC blocking-receive switch point (D-IPC-1): the park/wake cycle
  funnels through `thread_block` → `switch_context`, so it takes
  the same hook. Proven by the `ufpu` leg (§3.2) and by IPC2 on QA
  (§5).
- First entry: `_load_context`.
- Timer-driven deferred preemption: **no forcible timer preemption
  exists in this kernel** (`thread_exception_return()` is a no-op
  stub; full preemption via `ast_taken()` needs per-thread trap
  frames, future work). "Under timer preemption" for K1 therefore
  means: the 100 Hz timer is live and firing throughout the test
  (35 ticks counted during the selftest), and any AST switch would
  take the same `__Switch_context` hook. This is a limitation of
  the kernel, not of K1 (§6).

## 3. Test-first evidence (`scripts/verify-k1.py`)

Proof is value-comparing, not boot-PASS. The script boots QEMU on
copies of the disk images (shared disk untouched), waits for the
shell prompt, checks the kernel markers, then runs `embed:ufpu`.

### 3.1 Kernel selftest (`mach3-port/src/md/arm/tests/fpu_test.c`)

Hooked in `clock.c` between `user_selftest()` and
`sched_selftest()`. Two kernel threads with distinct patterns
(A: words `0xa0000000 + i*0x00010001`, FPSCR `0x00000000`;
B: words `0xb0000000 + i*0x00010001`, FPSCR `0x20000000`).
Per thread, 20 rounds: burn 1,000,000 empty-asm loops (timer ticks
fire), `switch_context` to the other thread, check the pattern
bit-exact, re-set. Each thread re-sets only after its check, so
every check observes the other thread's writes.

39 checks total (A 20 + B 19; B's final check is cut off when A
prints the verdict and continues boot — B is left suspended in its
last switch, same as `sched_test`'s worker_b).

Boot-flow note (found by the test): `current_thread()` is NULL in
`startrtclock`, so there is no boot thread to switch back to. The
first version switched back to it and panicked with a data abort
(`dfar=0x24`). The selftest now switches with `THREAD_NULL` as old
thread and never returns on success; worker A prints the verdict
then calls `sched_selftest()` itself. On setup failure it prints
FAIL and returns so boot continues.

Build flag: per-file `-mfpu=neon -mfloat-abi=softfp` for
`fpu_test` in `build-md.sh` (verified by scratch compiles to be
the flag set under which C inline-asm VFP is actually emitted).

### 3.2 Userspace leg (`user/ufpu.c`)

Template: `user/uipkpark.c`. Allocates a private IPC port, sets
pattern C (words `0xc0000000 + i*0x00010001`, FPSCR `0x10000000`),
parks 2000 ms via `uipc_recv` expecting `UIPC_ETIME` (-10), then
checks the pattern bit-exact. Registered in production PROGS/IMGS
(with per-program `-mfpu=neon -mfloat-abi=softfp`) and the
`embed:ufpu` hook in `user.c` (embed-hook table only — the daemon
table is untouched; daemon order face=0..sh=6, spawn=7, NDAEMON 8
is unchanged).

Alone this leg is vacuous (no second VFP thread runs while it
parks); combined with the kernel two-thread proof it covers the
user path end to end (user-mode VFP + trap + park + return).

### 3.3 Results

Pre-change, run 1 (`logs-k1/k1-prechange.log`): boot panicked
(NULL boot thread bug above); MISMATCH A rounds 1–19 already
visible before the panic — corruption detected, boot died.

Pre-change, run 2, clean boot (`logs-k1/k1-prechange2.log`):
`K1-VERIFY: FAIL (4/7)` — PASS: prompt reached, no panic, UFPU
PASS, no UFPU FAIL. FAIL: kernel PASS marker missing, FAIL marker
present, MISMATCH lines present.
`fpu_selftest: FAIL fails=39 checks=39`; MISMATCH A ×20, B ×19.
Every check fails without the fix, as designed. (UFPU PASS alone
pre-fix is expected — single VFP thread — and does not weaken the
verdict.)

Post-change (`logs-k1/k1-postchange.log` + battery rerun):
`K1-VERIFY: PASS (7/7)` — `fpu_selftest: PASS rounds=20 checks=39
ticks=35`, `UFPU: PASS elapsed=2000`.

## 4. Files changed

- `mach3-port/src/md/arm/context.s` — `.fpu` directive, `PCB_FPU`,
  save/restore in `__Switch_context`, restore in `_load_context`.
- `mach3-port/src/md/arm/thread.h` — `struct arm_fpu_state` in
  `struct arm_machine_state`.
- `mach3-port/src/md/arm/pcb.c` — compile-time layout guards.
- `mach3-port/src/md/arm/fpu.c` — stubs deleted, pointer comment;
  `fpu_init` unchanged.
- `mach3-port/src/md/arm/clock.c` — `fpu_selftest()` hook.
- `mach3-port/src/md/arm/tests/fpu_test.c` — new, kernel selftest.
- `mach3-port/build-md.sh` — `fpu_test` flags; `ufpu` in PROGS/IMGS
  with per-program VFP flags.
- `mach3-port/src/ext/user.c` — `embed:ufpu` hook entry only.
- `user/ufpu.c` — new, userspace park/wake leg.
- `scripts/verify-k1.py` — new, test-first harness.
- `docs/rencana-lanjutan/DEVLOG-K1.md` — this file.

Upstream (`mach3-port/upstream/`) untouched. `linkd` untouched.
SYS_STAT, Memory guard, name editor, icon font, Tier B daemons
untouched. Production headroom: the K1 PROD elf is 2,375,964 B
(base 2,362,532 B; delta 13,432 B = `ufpu` + `fpu_test` +
switch-path code).

## 5. Regression battery (branch `dev/k1`)

Logs: `~/workspace/logs-k1/` (`battery-*.log` = PROD chain,
`battery-qa-*.log` = QA legs, `rerun*.log`/`ab-*.log`/`base-*.log`
= reruns and the base comparison).

### 5.1 PROD legs — final verdicts

| Leg | Result |
| --- | --- |
| K1 | PASS 7/7 |
| UI5 | PASS 73/73 |
| UI4 | PASS 73/73 (after the flake history in §5.3) |
| UI3 | PASS 78/78 (after the flake history in §5.3) |
| UI2 | PASS 38/38 |
| UI1 | PASS 25/25 |
| FND1 | PASS (after the flake history in §5.3) |
| FND2 | PASS 21/21 |
| FND3 | PASS 46/46 |
| FND4 | PASS 36/36 |
| FND5 | PASS 33/33 |
| ONB1 | PASS |
| ONB2 | PASS |
| SHELL | PASS |
| SAFEMODE | PASS |

### 5.2 QA legs (QA=1 image) — all PASS

C6L1 29/29, C6L2 36/36, C6L3 72/72, C6L4 60/60, C6L5 55/55,
IPC2 34/34, IPC4 58/58, IPC5 20/20.

**QA-image correction (important for future batteries):** the
task text said "QA image only if your change needs Tier A
programs". That understates it: the C6L/IPC LEGS themselves
require the QA image, independent of the change under test.
`ulinktest`, `ulinkbr`, `ulink3`, `ulinkaud`, `ulink5`,
`ulinktools`, `ulink5die`, `uipkpark`, `uipknd`, `uiptoolssrv`,
`uipcregexit`, `uipcregfull`, `uipcaud` are all QA-only
(`build-md.sh`: "UI-8: QA image only (QA=1); omitted from
production"). The PROD-chain runs of those legs failed exactly on
their QA-program checks and nothing else (C6L1 7/29, C6L2 7/36,
C6L3 6/72, C6L4 9/52, C6L5 16/55, IPC2 27/34, IPC4 36/58,
IPC5 7/20) — image mismatch, not regressions, proven by the QA
passes above. (C6L4 counts 52 on PROD vs 60 on QA because the
script counts only applicable checks.) This matches the UI-8
battery split (QA+font for C6L/IPC, PROD+font for the rest).

### 5.3 Flake history (honest, all cleared by clean reruns)

- UI3: chain 65/78 (PIN-picker stall cascade) → standalone rerun
  75 PASS / 0 FAIL but its exec session died in teardown before
  the verdict line → clean rerun PASS 78/78. Cleared.
- FND1: chain FAIL on the documented ev=12 battery-order flake
  (not reproduced standalone) → rerun FAIL on a Power-navigation
  stall pair (`power screen opened`, `SHUTDOWN button rendered` —
  one navigation root) → clean rerun PASS. Cleared.
- UI4 (longest history — read before judging): chain 72/73
  (single Monitor Device row) → rerun 1: 48/73 (menu-navigation
  stall cascade) → rerun 2: 72/73 (tz-set timeout, the documented
  UI4 known flake) → rerun 3: 69/73 (Shutdown Power-navigation
  stall, 4 checks, one root) → A/B run 1 on K1: PASS 73/73.
  Four runs, four different roots, never the same deterministic
  failure. To rule out a K1-attributable flake-rate increase, UI4
  was run on the unmodified base kernel (scratch detached worktree
  @ `3d889c9`, removed afterwards) interleaved with K1: base
  73/73 PASS, K1 73/73 PASS, K1 second run 65 PASS / 0 FAIL when
  its exec session died in teardown (no verdict — see §6).
  Conclusion: intermittent harness/timing flakes (the sh
  key-drain stall class proven downstream of `sh` in UI-6, plus
  the documented tz-set timeout), not a K1 regression. UI4 is
  cleared by the clean 73/73 on the K1 kernel, per the UI-8
  precedent (flake → rerun → clean → merge sah).

### 5.4 Battery incidents (not kernel results)

- A runtime restart killed the first PROD chain mid-IPC4. The
  chain was resumed (orphan mock peer killed by PID — note:
  `pkill -f mock-c6.py` self-matches the invoking shell; kill by
  PID or a bracketed pattern instead) and completed; no state was
  lost beyond the interrupted leg.
- Three exec sessions died in final teardown phases (UI3 rerun 1,
  base UI4 attempt 1, A/B K1 run 2). In all three cases the partial
  log showed 0 failures at death; all three verdicts were
  re-established by clean reruns. Treat as a runtime/session
  phenomenon, not a test result.
- One battery byproduct (`logs/ipc4-serial.log`, a tracked file
  overwritten by a battery script in the worktree) was reverted
  before commit; battery `.ppm`/`.png`/`.log` byproducts in the
  worktree were left untracked and uncommitted.

End state after the battery: shared disk healthy (`fail_count=0`),
PIN restored to 246810 (UI3 leaves 1234; restored via
`setup pin 246810` → `setup: PIN saved`, verified in the same
boot), no QEMU or mock peer left running, main tree untouched.

## 6. Honest limitations

- No forcible timer preemption exists yet (§2.3). K1 covers every
  switch point that exists; a future preemption implementation
  must route through `__Switch_context` to inherit the FPU
  save/restore (it already does by construction — there is no
  other switch path).
- Kernel IRQ handlers must not use VFP/NEON: the trap frame does
  not preserve VFP state, and no kernel C code uses it (soft ABI
  everywhere). If a future kernel path needs FP in interrupt
  context, it must save/restore explicitly.
- The `ufpu` leg alone is vacuous (§3.2); the value-comparing
  proof is the kernel two-thread test, with `ufpu` covering the
  user-mode + trap + park/wake path.
- B's final check is cut off (39 checks, not 40) by design (§3.1).
- Continuation-discard semantics: FPU state is not saved when
  `kss` is not saved. On wake at a continuation, the thread's
  pre-block FPU state is restored (park/wake preserves it, as
  `ufpu` proves). A continuation entry point must not read FPU
  registers before writing them — the same rule as for general
  registers, which the switch path does not preserve either.
- UI4's flake cluster (§5.3) is real: the suite is timing
  sensitive (navigation stalls, tz-set timeout, row refresh).
  The clean K1 pass + base comparison clear K1, but the suite's
  flakiness itself is worth a future hardening pass (out of scope
  for K1).
