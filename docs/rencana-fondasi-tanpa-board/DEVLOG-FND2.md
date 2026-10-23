# Dev Log: FND-2 — Resource reclamation on daemon death

Date: 2026-10-23. Plan: `docs/rencana-fondasi-tanpa-board/`
`PLAN.md` §FND-2; audit item #3 in
`docs/riset-fondasi-tanpa-board/RINGKASAN.md`.

## Problem

Fault isolation worked (a faulting daemon dies alone,
`user_fault`), but `sched_exit_switch()` only marked the
daemon slot dead. Whatever the dead daemon held stayed
held forever: a dead uiapp kept the display token, a dead
sh kept the console, and the device needed a power cycle
to recover either — on a product whose selling point is
recovering by itself.

## Per-daemon kernel-state inventory

Swept `kernel/kernel/arm/user.c` (the only file that
references `sched_cur`), `webui.c`, and `tcp.c`:

| State | Keyed by | Owner in practice | Reclaim on death |
|---|---|---|---|
| Display token `dpy_holder` | progid (0=face, 1=uiapp) | face / uiapp | uiapp death: back to PROG_FACE + `evq_clear()` (mirrors SYS_DISPLAY_RELEASE). Face death: token is already home by construction; clear `dpy_sleep_req`, evq, pending face request (see below) |
| Event queue `evq[]` | (for the token holder) | holder | cleared whenever the token is reclaimed — queued input was typed for the dead reader (GRANT/RELEASE already clear it on hand-over) |
| Sleep request `dpy_sleep_req` | — | set by holder, consumed by face | NOT cleared on uiapp death: the Sleep Now flow sets it immediately before release, so a holder dying in between must not lose the user's request (the RELEASE path keeps it for the same reason). Cleared on face death: its only consumer is gone |
| Console owner `console_owner` | daemon index | sh (SYS_CONSOLE_TAKEOVER at startup) | back to -1, the boot default (anyone may read) |
| Web-shell bridge `ws_state/ws_cmd/ws_out` (webui.c) | — | executor = the console owner (sh is the only SYS_WEBUI taker) | settled by `webui_shell_executor_died()` when the console owner dies: state 1 (queued, unclaimed) dropped; state 2 (taken, never posted) completed into state 3 with the error result "web shell: executor exited before completing the command" so the web side gets a definitive answer; states 0/3 untouched |
| Face request `face_req_*` | poster (new `face_req_owner`, daemon index) | qabotd in practice | dropped if the poster dies (`face_req_owner` recorded in the SYS_FACE_EXPR set path, cleared when the face consumes the request); also dropped on face death (consumer gone) |
| TCP client connection `tcc` (tcp.c) | opener (new `tcp_client_owner`, daemon index) | qabotd (provider) | `tcp_client_close()` (graceful FIN) if the dying daemon opened it. A connect still in SYN_SENT is left to the existing client timeout (close() only acts on ESTABLISHED — documented tcp.c behaviour) |
| Spawn state `spawn_parent/spawn_exitcode/spawn_done` | parent index | spawning daemon | already reclaimed before FND-2: the child's exit wakes the parent in `sched_exit_switch` (the pattern reference). A parent cannot die while WAITING (it never runs), so no dangling parent exists. `daemon_reclaim()` runs before that branch and does not touch spawn state except that a dying spawn child also gets its console/TCP/face-request holdings reclaimed |
| `gui_pending` | — | a request to the scheduler, not to a daemon | deliberately NOT reclaimed: a GUI launch requested before sh died still completes at the next switch; the launched daemons own their own slots |
| Daemon Mach task / pmap / udesc | slot | the daemon | NOT reclaimed: slots are fixed (NDAEMON) and daemons are never respawned in v1, so the bound is constant. Destroying VM objects belongs with the respawn design (FND-3) |
| Webui session + PIN throttle (webui.c) | — | the web server (kernel/netpump context, not a daemon) | not daemon-owned; correctly survives any daemon death |
| Wall clock `time_valid/time_synced` | — | set by the ntp one-shot | global clock state, not owned |
| littlefs mount, credstore, bootstate | — | kernel services | not daemon-owned |
| GPIO pin levels (gpio.c) | — | last writer | deliberately left: outputs must not glitch because a daemon died |
| Audio capture (audio.c) / PL330 | — | today only pre-scheduler one-shots (uaudiocap, uvoice) start capture | no daemon owner exists today; if a daemon ever starts capture, stopping it joins this list |

## Design

One function, `daemon_reclaim(idx)` in `user.c`, called at
the top of `sched_exit_switch()` — the single funnel both
death routes already share (clean SYS_EXIT, and
`user_fault()` which forwards into it). No new syscall.

Supporting changes:

- `face_req_owner` (user.c): recorded on SYS_FACE_EXPR
  set, cleared on poll-consume.
- `tcp_client_owner` (user.c): recorded on successful
  SYS_TCP_CONNECT (daemon index, or -1 for one-shots),
  cleared on SYS_TCP_CLOSE.
- `webui_shell_executor_died()` (webui.c, declared in
  webui.h): bridge settlement described above. Keyed on
  console-owner death because the executor is by
  construction the console-owning shell; a random daemon
  dying mid-command (e.g. qabotd) must not abort sh's
  running command, so the call is inside the
  console-owner branch only.
- Face-death rule (documented in code): if the dying
  daemon is the face holding the token, the token stays
  PROG_FACE (it is home by construction) and only the
  state aimed at the face is reset (sleep request, event
  queue, pending expression request). Nothing can render
  without the face; making a future face instance start
  from clean state is the safe reset. (Respawn itself is
  FND-3.)

### Face death vs the finale/menu flows

Reclaim returns the system to exactly the kernel state a
normal SYS_DISPLAY_RELEASE produces, so the face's
existing poll/grant loop and uiapp's acquire/wait loop
(`while (sys_display_acquire() == 0) yield`, then
`ui_reset(&scr_menu)` on acquire) handle a reclaimed
token with no product-code changes.

## Fault injection (verification scaffolding)

`scripts/verify-fnd2.py` injects deaths through product
paths only: SYS_SPAWN children (`run <path>` in sh) that
die on a deliberate data abort at 0x200000 (outside the
child's pmap; routes to `user_fault`, exit code 0xDAB7 =
55991, printed by sh's `run`).

- `user/ufault.c` ("embed:ufault"): takes console
  ownership, takes a pending web-shell command if one
  arrives within ~8 s (bridge -> running), faults without
  posting.
- `user/ufaultd.c` ("embed:ufaultd"): injects EV_MENU so
  the face grants it the display, waits until it is the
  holder, faults.
- The `embed:` hooks in SYS_SPAWN were generalised from
  the single hardcoded "embed:uls" (Q9) into a small
  table. The ufaultd entry carries a progid override
  (PROG_UIAPP): the display token is progid-keyed, so a
  test child that must die holding it has to carry the
  holder progid. The override exists ONLY in the hook;
  product (FAT) spawns always get progid -1. The kernel
  cannot distinguish the child from the real uiapp —
  which is precisely why the test is faithful for the
  token logic — and the real uiapp stays alive throughout,
  masking nothing the checks rely on: the checks key on
  the face rendering again and on a NEW grant/acquire
  cycle counted after the death (see the pre-fix note on
  the menu pixel check below).

## Verification results

Build: `cd kernel && source ~/workspace/toolchain/env.sh
&& ./build-md.sh` → MD 42 ok / 0 failed, LINK OK.

### Post-fix (final tree): FND2-VERIFY PASS 21/21

Run twice on the final build (before and after the
regression battery), PASS 21/21 both times. Measured
evidence: eye-band pixels after the holder's death =
4460–4500 (face rendering again); selected-tile pixels
in the re-granted menu = 6498; the bridge settled with
"web shell: executor exited before completing the
command"; sh answered `bootctl` on the reclaimed console
("last_reason=").

### Pre-fix (only change: the `daemon_reclaim()` call in
### `sched_exit_switch` commented out, rebuilt)

- Full script: **FAIL 8/20.** Injection and isolation
  checks pass (the fault machinery pre-exists FND-2);
  then: no console reclaim log, bridge never settles,
  console dead (the owner index points at the dead child
  forever, so SYS_READ_CONSOLE refuses sh — total
  silence), and phase B cannot even start (the wedged
  console cannot type `setup reset`/`gui`).
- `--only-b` (display wedge isolated from the console
  wedge): **FAIL 8/11.** The holder child is granted,
  faults, and is isolated — then nothing: no reclaim log,
  eye pixels after death = **0** (the face never renders
  again; the screen keeps the dead holder's last frame),
  no re-grant. One honest nuance: the "menu renders"
  pixel check passes pre-fix — vacuously, because the
  menu was never cleared from the screen (the token
  never left the dead progid). It only discriminates in
  sequence, after the eyes check and the counted
  re-grant, which is how the post-fix run passes it.

### Regression battery (each standalone)

- verify-shell.sh: SHELL-VERIFY PASS
- verify-safemode.sh: SAFEMODE-VERIFY PASS
- verify-firstboot.sh: first attempt FAIL — the shared
  dev disk was left provisioned by verify-fnd2's own
  `setup done` (disk state, not code); after a cleanup
  boot (`setup reset`), standalone rerun PASS.
- verify-update.sh: first attempt FAIL on boot2/boot5
  slot+version expectations (mid-cycle A/B state from
  back-to-back runs — the documented shared-disk
  flake); standalone rerun PASS (boot2 version=258
  slot=A confirmed; boot5 rolled back to 258 slot=A).
- verify-onb1.py: ONB1-VERIFY PASS (20 PASS lines)
- verify-onb2.py: ONB2-VERIFY PASS (the nanobind leak
  warnings on stderr are zxing-cpp exit noise, printed
  after the verdict)
- verify-fnd1.py: FND1-VERIFY PASS — FND-1 stays green.

The shared dev disk was left unprovisioned after the
battery (the state verify-firstboot expects).

### Script transport note

`verify-fnd2.py` routes the phase-B steps that must run
while qabotd is alive (`setup done`, `run embed:ufaultd`,
`key menu`) through the web shell instead of typed serial
lines: on the open console those lines race qabotd (and,
for the holder, the face's own console drain) and lose
persistently in some boots. The web bridge executes the
same builtins inside sh, deterministically — the product
paths under test (spawn, grant, fault, reclaim) are
unchanged.

## Observations (not FND-2 defects)

- With the console open (the reclaim's restored default),
  typed line bytes can split between sh and qabotd (both
  poll SYS_READ_CONSOLE when there is no owner), leaving
  a mangled partial line in sh's buffer; qabotd can also
  swallow whole commands. This is pre-existing Q9
  semantics — it is how the console behaves whenever no
  daemon owns it (e.g. before sh's startup takeover) —
  exposed, not caused, by the open-console default.
  verify-fnd2.py sidesteps it by driving the contended
  steps through the web shell (see the transport note in
  the results). A structural fix (sh re-taking ownership,
  or a successor rule) belongs with FND-3's respawn,
  where a restarted sh takes the console at startup as
  usual.
- After a reclaim, the real uiapp prints "uiapp: release
  FAIL" once: its main loop calls sys_display_release()
  unconditionally after the render loop notices the lost
  token, and the release legitimately fails because the
  token is already home. Cosmetic; uiapp then waits for
  the next grant as designed. A uiapp-side guard (release
  only while still holding) would silence it — noted,
  out of FND-2 scope.
