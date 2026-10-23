# DEVLOG FND-3 — Watchdog / health supervisor

Status: **DONE** (2026-10-23). `scripts/verify-fnd3.py` PASS 46/46
post-fix, FAIL 10/45 pre-fix (supervisor compiled out — see Run
history). Full standing battery green (below). Not committed by the
implementer; the coordinator commits after review.

Problem (audit item, `docs/riset-fondasi-tanpa-board/RINGKASAN.md`):
one hung daemon froze the GUI and the network with no detection and
no recovery — the only way out was a manual reset, and a hang during
boot repeated forever. FND-3 adds a kernel health supervisor that
detects a wedged daemon, escalates through a logged ladder, and —
when recovery is impossible — converts the hang into the existing
bootstate 3-strikes safe mode instead of a reset loop (acceptance
bar from `docs/riset-out-of-box/RINGKASAN.md`: 3 consecutive crashes
→ recovery mode, never a brick loop).

Justification for living in the kernel: `docs/ARSITEKTUR.md` §5, the
FND-3 row — the supervisor is the principled R6 exception. The
policy must be able to act precisely when userspace is what hung, so
it cannot itself be a userspace daemon. It is kept as small as the
ladder allows: timestamps, comparisons, and FND-2's reclaim funnel.

**No new syscall.** Progress is derived from existing signals, so
`SYSCALL-ABI.md` is unchanged.

## Design as built

### Liveness model

Every daemon slot (`struct daemon` in `user.c`) carries
`wd_last_progress`, stamped with the 100 Hz timer tick
(`arm_timer_ticks()`). Progress is defined as: **the daemon entered
the syscall gate (`user_syscall`) with any syscall except
`SYS_YIELD`**. When healthy, every daemon makes non-yield syscalls
on every loop iteration (event polls, console reads, display
flushes), so this is an honest "still working" signal, not a
cooperative heartbeat a wedged-but-looping daemon could fake cheaply
— a daemon stuck in a yield-only retry loop makes no progress by
definition here, which is exactly the wedge shape of audit concern.

Exemptions and edge cases:

- A spawn parent in state WAITING is exempt: while it waits, its
  progress is the child's business. Its stamp is refreshed when the
  child-exit path wakes it.
- Slots not yet registered (user mode registers daemons late) have
  zero deadlines and are skipped.
- One scan can only escalate a daemon by one rung per pass, and the
  warn rung re-arms only after progress resumes, so each hang
  episode logs a bounded number of events.

Deadlines (warn / hang, ticks converted from ms at 100 Hz):

| daemon | warn | hang | note |
|---|---|---|---|
| face | 10 s | 20 s | render loop is syscall-dense |
| uiapp | 10 s | 20 s | same |
| qabotd | 15 s | 30 s | network waits are longer |
| sh | 60 s | 120 s | input-aware backstop only — an interactive shell idle at the prompt polls input with non-yield syscalls, so it never accrues age; the deadline exists to catch a shell wedged *while running something* |
| spawn child | 15 s | 30 s | class (a) only, see below |

### Class (a) — yields but wedged

`wd_supervise()` runs at the two points the cooperative kernel
already funnels through: the tail of `sched_yield_switch()` (after
`ctx_save`; a respawn performed there makes the caller activate +
`ctx_load` the fresh task instead of the saved one) and the tail of
`sched_exit_switch()` (after exit bookkeeping). Escalation ladder,
per daemon:

1. **Warn** (age > warn deadline): bootlog **ev 13** + serial print.
   No action.
2. **Hang** (age > hang deadline): bootlog **ev 14** + serial print.
3. **Act**:
   - Persistent daemon (face/uiapp/qabotd/sh): `daemon_reclaim(idx)`
     (FND-2's funnel — display token, console, pending face request,
     TCP client all come home), then **respawn** (below). Bootlog
     **ev 15** + print. Respawn budget **2 per daemon per boot**;
     the third hang is fatal (rung 4).
   - Spawn child: reclaim, then wake the waiting parent with exit
     code **`USER_EXIT_WDOG` 0xD09D (53405)** — a value a parent can
     distinguish from any normal exit and from the fault path
     (FND-2's 0xFA017). **3 consecutive** watchdog child kills are
     fatal (rung 4); any clean child exit resets the counter.
4. **Fatal**: `wd_fatal_reset()` → `bootstate_note_watchdog()` +
   `psci_system_reset()`.

### Class (b) — never yields

A daemon that spins without syscalls never reaches a switch point,
so no switch-point supervisor can ever fire (the cooperative
kernel's hard limit — the audit and the threading research both
flagged this). The one mechanism that still runs is the timer IRQ
(100 Hz, PPI 27). `wd_irq_tick()`, called from `arm_trap_handler`
in `trap.c` after `timer_ticks++`, implements the strongest safe
version:

- It tracks `wd_last_activity`, stamped at both scheduler switch
  functions and at `sched_run()` (any successful switch counts).
- If the interrupted frame is **user mode**, a daemon is current,
  and no scheduler activity has occurred for **30 s**
  (`WD_STARVE_TICKS`), it concludes the current user task is
  starving the system and goes straight to the terminal rung:
  watchdog note in bootstate + PSCI reset. There are no soft rungs
  — in IRQ context the kernel cannot safely run task setup, reclaim
  policy, or respawn machinery, and the task never gave the
  supervisor a cooperative moment to use them.
- If PSCI refuses the reset, the window re-arms and the check
  retries (and re-logs) every 30 s rather than latching silent.

Attribution honesty: the check proves "the scheduler has not
switched for 30 s while this slot was current", with the timer
still ticking. It cannot distinguish a user-space spin loop from a
pathological kernel path spinning with a user task current; in this
kernel the latter would also stop the timer or fault. Either way a
reset with `BSR_WATCHDOG` is the right terminal action.

### Respawn machinery (new)

`wd_respawn(idx)` re-runs FND-1/FND-2's launch path
(`setup_uprog_task`) with the daemon's embedded image
(`daemon_images[d->image]`: 0=face, 1=uiapp, 2=qabotd, 3=sh) into
the **same fixed slot**, resets its watchdog fields, and lets the
scheduler activate it normally. The slot model from FND-2 is what
makes this trivial — daemons are identified by slot, not by task
pointer, anywhere it matters.

Per-daemon state-loss caveats (RAM state only; on-disk state is
untouched by construction):

- **face**: expression/timers reset; if it was mid-setup it
  re-enters setup mode (the QR payload is derived from durable
  state). The finale/hands-on state restarts.
- **uiapp**: menu position and settings-screen edits in progress
  are lost; it re-enters its acquire-wait loop and renders on the
  next grant.
- **qabotd**: session/LLM context in RAM is lost; its TCP client is
  reclaimed first (FND-2), so no half-open client lingers.
- **sh**: the shell session restarts (it re-runs its startup
  self-test and prints a fresh prompt); the console is reclaimed
  first, so the new shell owns it cleanly.
- **Memory**: the wedged task's Mach task/VM structures are **not**
  destroyed (the kernel has no task-destroy path — the same gap
  FND-2 documented). Each respawn leaks one task's worth, bounded
  by the budget: at most 2 leaks per daemon per boot, then the
  fatal rung resets the device. Acceptable for v1; a task-destroy
  primitive is the natural FND-4+ follow-up if respawn budgets ever
  need to grow.

### bootstate integration

New bootlog events in `bootstate.c`: **13** wd warn, **14** wd
hang, **15** wd respawn, **16** wd reset. `bootstate_log_event()`
appends a record without touching fail accounting.
`bootstate_note_watchdog()` sets `last_reason = BSR_WATCHDOG (3)`
(the value FND-1 reserved) and `fail_count = BS_FAIL_THRESHOLD - 1`
— deliberately the same arithmetic as `bootstate_crash_sim()`: the
next boot increments to the threshold and lands in safe mode, and a
safe boot whose one-shots pass healthy-marks and self-clears. So a
device that hangs every boot converges to safe mode in exactly one
extra boot and *stays recoverable* — it can never reset-loop,
because safe mode does not launch the daemons that hang.

### Hardware WDT seam

`wdt_kick()` in `user.c` is a no-op marked `TODO(HW)`, called from
exactly one place: a `wd_supervise()` scan that found nobody
overdue. On hardware, the dw-wdt driver (RV1103, 0xff5a0000,
two-stage — research `docs/riset-soc-rv1103/RINGKASAN.md` §2 item 3)
gets fed from that call and nowhere else: if the supervisor itself
stops running (kernel wedge, class (b) in kernel context,
interrupt failure), the kicks stop and the hardware resets the SoC
with its own reason path. Supervisor deadlines stay in ticks/ms
from the existing timebase; the hardware timeout value is
deliberately *not* expressed yet because the WDT clock rate on the
Mini B is unverified. Writing the dw-wdt driver is out of scope
(hardware phase).

### Test switch

`WD_ENABLED` (user.c, =1) gates both supervisor call sites and
`wd_irq_tick()`'s body. The pre-fix FAIL evidence below was produced
by flipping it to 0 — same tree, same test programs, no supervisor.

## Injection one-shots (embed hooks only, never in `uprogs[]`)

Following the FND-2 pattern — lookup hooks in the SYS_SPAWN embed
table, production spawn surface unchanged:

- `user/uyield.c` (`embed:uyield`) — yields forever, makes no
  progress: the class (a) child case (warn → hang → kill → parent
  woken with 53405), repeated 3× for the fatal rung.
- `user/uhangc.c` (`embed:uhangc`) — takes the **console**, then
  yields forever: deterministic reclaim-under-hang evidence (the
  console has no idle/return path, so only the supervisor +
  `daemon_reclaim` can bring it home).
- `user/uhangd.c` (`embed:uhangd`, PROG_UIAPP hook) — injects
  EV_MENU so the face grants it the **display token**, then yields
  forever: a wedged display holder that is not the daemon that
  would normally return it.
- `user/uspin.c` (`embed:uspin`) — pure spin, no syscalls at all:
  the class (b) case; only the IRQ-side check can fire.

Note the plan asked for "a daemon that hangs repeatedly across
respawns" to prove the ladder reaches safe mode instead of looping.
The child-kill ladder (phase D) proves the same property — repeated
hangs escalate to a watchdog reset + safe mode rather than looping
— through the spawn slot, which needs no daemon-slot surgery. The
persistent-daemon respawn rung itself is implemented and
budget-capped (2/boot) but is *not* exercised end-to-end by the
verify script: hanging a real face/uiapp/qabotd on demand would
need a wedge hook inside a production daemon, which this phase
deliberately does not ship. Recorded as a limitation, not hidden.

## Verification — `scripts/verify-fnd3.py`

One QEMU process; the guest resets in place between phases. Serial
sockets are drained by a reader thread throughout (house rule).
Commands to `sh` are issued through the **web shell** once the GUI
is up (see Harness findings for why typed serial is unreliable in
dev mode), and observed on the serial transcript like everything
else.

- **Phase A** (boot 1, pre-GUI): `uyield` → warn line + ev 13, hang
  line + ev 14 in `bootlog`, child killed, `exit 53405`, no reset
  (same boot), shell responsive afterwards.
- **Phase D** (boot 1): clean `run embed:uls` (exit 0) resets the
  consecutive-kill counter, then 3 consecutive `uyield` runs — the
  3rd is fatal: new boot, banner `reason=3 (watchdog)`, **SAFE
  MODE**, safe shell answers.
- **Boot 3**: after a plain `reboot` from safe mode the device is
  NOT in safe mode and `bootctl` reports `fail_count=0`
  (self-clearing works — no latch-up).
- **Phase B2** (boot 3, pre-GUI): `uhangc` takes the console → hang
  → kill → parent woken 53405 → kernel logs the console reclaim →
  `sh` answers `bootctl` on the recovered console.
- **Phase B** (boot 3, GUI): full ONB-2 flow to the finale, then
  `uhangd` becomes display holder via a real grant → killed by the
  watchdog (exit 53405, no fault) → display token comes home →
  face renders its eyes again (framebuffer count: 4460 eye pixels,
  the FND-2 baseline) → face grants again on `key menu` → uiapp
  re-acquires and the menu renders (6498 selected-tile pixels).
- **Phase C** (boot 3): `uspin` via the web shell → the IRQ
  starvation check resets the device (~30 s) → new boot with
  `reason=3`, SAFE MODE, and bootlog **ev 16** present.
- **Cleanup**: reboot until a normal boot with `fail_count=0`; the
  shared dev disk is left healthy and unprovisioned.

### Run history (honest)

- Runs 1–2: FAIL on script bugs of mine (a wrong expected exit
  decimal — 53373 for 53405 — and transcript segment slicing that
  cut the SAFE MODE banner) plus one unexplained D-window timeout;
  no kernel changes resulted from these.
- Run 3: FAIL 45/46 — the only miss was cleanup bookkeeping: one
  boot in the chain had a *genuine* one-shot failure (ufs/FAT under
  host load) and correctly skipped its healthy mark, so cleanup
  needed one more reboot than the script allowed. Cleanup is now a
  reboot-until-healthy loop.
- Run 4: interrupted by a VM replacement; before dying it exposed
  the typed-serial race described under Harness findings, which
  moved phases A/D onto the web-shell transport.
- Run 5: FAIL 44/46 — phase B's post-kill screendump caught a stale
  frame (uiapp's last menu) ~3 s after the token came home; eye
  pixels 0 that run, 4460 in runs 3/6/7 with identical code.
  Treated as a host-load flake (frame flush vs screendump race),
  not reproducible on demand.
- Run 6: green through phase B, then the VM was replaced mid-phase
  C, losing the run.
- **Run 7 (final tree): PASS 46/46.**
- **Pre-fix (WD_ENABLED=0, otherwise identical tree): FAIL
  10/45** — the child starts, but there is no warn, no hang, no
  kill; the shell stays wedged inside the first `run` and every
  downstream phase cascades. That contrast is the point of the
  test.

### Regression battery (each standalone, final tree)

| script | result |
|---|---|
| verify-shell.sh | PASS |
| verify-safemode.sh | PASS |
| verify-firstboot.sh | PASS — see note |
| verify-update.sh | PASS |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS — standalone rerun; the first attempt died with NO PROMPT because a QEMU/socket from an interrupted environment was still holding the serial path (killed by exact name, rerun clean). Also needed `zxing-cpp` reinstalled after a VM replacement. |
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS (21/21) |
| soak-test.py (100 boots + 30 power cuts) | see below |

firstboot note: the first battery attempt failed boot 1 with "setup
status should be NOT provisioned" because FND-3's own phase B leaves
the device provisioned (its `setup reset` runs at phase start, the
web provisioning completes after). After a manual `setup reset`,
verify-firstboot passes standalone and self-cleans as designed.
Test hygiene, not a regression — but worth knowing: any future
script that provisions must reset before handing the disk on.

soak note: `soak-test.py` is resumable via `logs/soak-results.log`;
the first invocation in this session found the 2026-10-20 run's
complete log and reported PASS **without booting anything**. That
result is void. The 2026-10-20 log was archived to
`logs/soak-results-2026-10-20-bphase.log` and a fresh full run was
executed against the FND-3 kernel: **SOAK: PASS (fails=0)** —
100 cold boots + 30 random power cuts + final FS/bootstate check,
results in `logs/soak-results.log` (this run's own log).

## Harness findings (for future verify scripts)

1. **Typed serial races the face in dev mode.** The face's progid
   (0) equals sh's console-owner slot index (0), so when the
   console is open the face's `SYS_DISPLAY_GET_EVENT` drains typed
   bytes as button events (`translate_key`: `m` → EV_MENU, …).
   A typed `run embed:…` sent while the face holds the display can
   be silently eaten. FND-2's script survived on timing; FND-3's
   phase D did not. Rule adopted in verify-fnd3: once the GUI is
   up, drive `sh` through the web shell; keep typed serial for
   pre-GUI phases and safe mode. (The byte race itself is the
   already-known FND-2 observation; a real fix — separating the
   display event queue from the console byte stream — belongs to a
   later phase.)
2. **Two 30 s timers start at the display grant** — the spawn-slot
   hang deadline and uiapp's `UI_IDLE_RETURN_S`. Which one returns
   the token for a wedged *child* holder is a genuine race; both
   outcomes are safe (the watchdog kill still lands, and reclaim
   tolerates the token already being home). Phase B therefore
   asserts the disjunction and prints which path won (in all
   completed runs: uiapp's idle release won, the kill landed ~2 s
   later). Phase B2 exists because the console case has no idle
   path and proves reclaim-under-hang deterministically.

## Limitations

- Persistent-daemon respawn is implemented and budget-capped but
  not exercised end-to-end (see Injection one-shots note).
- Respawn leaks the wedged task's Mach structures (no task-destroy
  primitive yet), bounded at 2 per daemon per boot.
- Class (b) coverage is terminal-only: reset with `BSR_WATCHDOG`,
  no soft rungs possible from IRQ context; attribution is "the
  current slot when the starvation window elapsed".
- A WAITING spawn parent is exempt from supervision; a parent that
  wedges in WAITING on a healthy child is out of scope (its state
  is kernel-mediated and its child still finishes or is killed).
- Deadlines are in timer ticks; QEMU time dilation under host load
  stretches wall time, not tick time, so thresholds are
  guest-time values.
- `wdt_kick()` is a seam, not a driver: no dw-wdt registers are
  touched until the hardware phase verifies the WDT clock rate.

## Files

- `kernel/kernel/arm/user.c` — watchdog state in `struct daemon`,
  progress hook in `user_syscall`, `wd_supervise()` + ladder,
  `wd_respawn()`, `wd_fatal_reset()`, `wd_irq_tick()`, `wdt_kick()`
  seam, `WD_ENABLED`, switch-path hooks, spawn-child watchdog
  init + embed hooks for the four test programs.
- `kernel/kernel/arm/trap.c` — one call: `wd_irq_tick()` from the
  timer IRQ.
- `kernel/kernel/arm/bootstate.c` — events 13–16,
  `bootstate_log_event()`, `bootstate_note_watchdog()`.
- `user/uyield.c`, `user/uhangc.c`, `user/uhangd.c`, `user/uspin.c`
  — hang-injection one-shots (embed hooks only).
- `kernel/build-md.sh` — the four programs added to both build
  loops.
- `scripts/verify-fnd3.py` — the verification above.
- `logs/fnd3-serial.log`, `logs/fnd3-after-kill.ppm`,
  `logs/fnd3-menu-again.ppm` — run-7 evidence artifacts.
