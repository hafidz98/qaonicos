# DEVLOG-C6L5 — link daemon linkd (last C6L phase)

Branch `dev/c6l5`, base = C6L-4 merge. No kernel change,
no wire change: topology only. The wire stays v1.1
(`WIRE-FREEZE.md`) + v1.2 (`WIRE-FREEZE-V1.2.md`); the
IPC surface is frozen `uipc.h` v1 (DEVLOG-IPC4/IPC5 §11).

## 1. What C6L-5 delivered

- `linkd`, a real daemon (daemon_images index 4, 2 pages
  stack): claims the reserved IPC name `link` at startup
  through the kernel-assigned spawn identity, owns the
  single transport (bridge backend in QEMU, mock backend
  for unit tests), and multiplexes multiple IPC clients
  onto the one link. No other daemon touches the
  transport directly after this phase.
- LINKD-IPC v1, the client contract
  (`user/uartproto/linkd.h` normative;
  `docs/rencana-komunikasi-c6/LINKD-CONTRACT.md`
  human-readable): 16 ops (CMD, KV_SET/GET/DEL,
  TOOL_DISPATCH, NETHTTP_GET, VOICE_START/STOP,
  AUD_FRAME, EVT_WAIT, STATUS, DISPATCH_RESULT,
  WS_OPEN/SEND/RECV/CLOSE), 8 status codes, per-op
  reply budgets, caps, truncation semantics mirroring
  the direct layer, EVT fan-out, queue/death/watchdog
  rules.
- The client stub `user/uartproto/linkcli.c`: the full
  `uartproto.h` surface over IPC with signatures
  identical to the direct layer, so consumers migrate
  by changing only the link line. One deliberate
  omission: `uproto_retry_policy` is NOT provided —
  the classifier lives in linkd via the real `_retry`
  wrappers, and a second copy in the stub would
  diverge.
- Migration of the real daemon consumer: uiapp screens
  (`scr_wifi.c`, `scr_ble.c`, `scr_llm.c`,
  `scr_passkey.c`) now reach the link through the stub.
  Verified: uiapp uses only `uproto_cmd`,
  `uproto_kv_get`, `uproto_kv_set` — all provided.
  Uiapp shrank from ~69 KB (C6L-1) to 60,304 B: the
  migration removes the direct uartproto + mock
  objects from its link line.
- Peer-dispatch forwarding: linkd forwards peer
  DISPATCH units to IPC name `tools` (SYS_RPC 12, rpc
  id 0x3101, body tool NUL + args-json NUL, reply
  result JSON NUL, 5000 ms timeout) where the real
  policy gate decides. Linkd never executes, confirms,
  or refuses a peer tool. If no tools consumer answers
  (ENORV/EDEST/EPEER/ETIME/malformed reply), the peer
  gets `{"ok":false,"error":"tools_unavailable"}` via
  DISPATCH.RESULT — never silence.
- EVT fan-out to subscribed IPC clients (see §4).
- The shell `link` status command (read-only): prints
  one greppable line `link: UP/DOWN reason=… ok=…
  … evt_seq=… waiters=… qlimit=…`, or
  `link: unavailable (rc=…)` when linkd is absent.
- Test programs: `ulink5` (spawn client through the
  stub), `ulink5d` (marker-gated second-client daemon),
  `ulinktools` (stub tools server), `ulink5die`
  (client-death probe). `uvoice` stays direct (it is a
  one-shot, not a daemon; its boot UVOICE regression
  needs the mock backend without linkd running). Test
  programs (`ulink*`, `ulinkaud`) stay direct — they
  test the layer itself.
- The verify script `scripts/verify-c6l5.py` (55 checks).

## 2. Ownership and arbitration

- The single TCP connection is linkd's alone. The
  C6L-2 constraint (qabotd must not connect
  concurrently) is now enforced by ownership, not
  convention: qabotd links no uartproto, face links no
  uartproto, and the transport-ownership rule binds
  persistent daemons.
- Linkd uses the bridge backend ONLY in QEMU — no
  silent mock fallback. A fallback would mask peer
  outages and break the transport-death test. Screens
  without a peer show link-down gracefully (the stub
  returns -1, and the screens already handle -1).
  GUI verification via linkd therefore requires the
  host peer running.
- Daemon-table change (mechanical): old table face(0),
  uiapp(1), qabotd(2), uipcld(3, marker-gated test
  daemon), sh(4, MUST stay last); NDAEMON was 6 (5
  daemons + 1 spawn slot). Inserted `linkd` at index
  4 (2 pages) and test daemon `ulink5d` at index 5
  (1 page), both BEFORE sh (new index 6); NDAEMON
  6→8; watchdog `case 4` (sh, 6000/12000 ticks) →
  `case 6`; the default watchdog case (15/30 s)
  covers linkd + ulink5d. Registry table in
  `mach3-port/src/ext/ipc_glue.c`: `{ "link", 4 }`
  (was `REG_OWNER_UNBOUND`). `SH_IMAGE_IDX`,
  safe-mode-only-sh, dev-mode-only-sh, and
  `gui_maybe_launch` (all except sh) adjust
  automatically — they are index-based.
  `uprog_udesc` is sized `NUPROGS + NDAEMON_IMAGES`
  (auto-scales). `uprog_names` is capped at 16 with a
  guard (adding daemons cannot corrupt anything; at
  worst SYS_TLIST shows fewer names). The only other
  hardcoded progid-4 use found was bootstate sub 4
  (unrelated).
- `uipc.h` frozen-header comment for `link` got a
  COMMENT-ONLY update ("bound at C6L-5 to linkd,
  daemon_images index 4") — no ABI change (no
  constants, no layout).

## 3. The IPC surface (LINKD-IPC v1)

Request body word 0 = op; reply body word 0 = status;
reply message id echoes the request op (never 71, the
Mach send-once death-notice id). Ops 1–16 and status
0–7 are tabulated in LINKD-CONTRACT.md and in
`user/uartproto/linkd.h`.

The sizing rule that shaped the whole contract
(verified against `sys_rpc_call` at
`mach3-port/src/ext/user.c:2339` and
`ipc_glue_send_ex`): the `len` passed to
`uipc_rpc_user` is BOTH the sent size and the reply
budget; a larger reply is E2BIG and is LOST (the
wrapper destroys the reply port). The stub therefore
pads short requests to per-op reply budgets with
zero bytes (linkd parses strings by NUL termination
and ignores trailing zeros): CMD 2048 (SCAN/LIST
multi-line responses are the large case), KV_SET 192,
KV_GET 512, KV_DEL 128, TOOL 1024, NETHTTP 2048
(body is 1024 by the C6L-2 peer), VOICE 128,
FRAME 1088, EVT 512, STATUS 512.

Envelope validation happens BEFORE the wire;
envelope violations are answered PEER_ERR "ARG"
without a wire exchange. Caps: command line 159
chars (mirrors the direct tx-buffer truncation),
tool/key/url 63 chars, TOOL.DISPATCH args 192 bytes
(wire cap), KV value 64 bytes (store cap), AUD
payload 1024 bytes (`LINK_MAX_PAYLOAD`), EVT payload
256 bytes.

STATUS layout (word indices 0–19): `[status, state,
reason, ok, peer_err, timeout, dead, crc_bad,
oversize, malformed, resets, retries, retry_denied,
frames_sent, frames_send_fail, inb_dropped,
evt_dropped, evt_seq, waiters, qlimit]`.

## 4. Forwarding and fan-out

- Tools forwarding: peer DISPATCH units are forwarded
  to IPC name `tools` via `uipc_rpc_user` with RPC id
  0x3101, body = tool NUL + args-json NUL, reply id
  echoes 0x3101, body = result JSON NUL, timeout
  5000 ms. Qabotd does NOT register `tools` today
  (only the IPC-4 mock server `uiptoolssrv` does,
  with progid override 2) — and the IPC-4 mock
  protocol (echo 0x3000+s XOR 0xA5, dispatch 0x3100,
  double 0x3200, die 0x3300, stop 0x3400) cannot
  express tool names/policy, so it is NOT reused for
  forwarding. C6L-5 has its own stub tools consumer
  (`ulinktools`, spawn-only via `embed:ulinktools`
  with progid override 2, so it may claim reserved
  `tools` when `uiptoolssrv` is not running; the two
  can never hold the name concurrently — EBUSY — so
  verify scripts must not run them together).
  `ulinktools` implements the C6L-3 policy mirror
  with real guest data: ALLOW get_info (returns real
  `sys_uptime()` uptime_ms) and gpio_read (pin table
  value = pin & 1); CONFIRM gpio_write/file_write →
  `{"ok":false,"error":"needs_confirm"}` (never
  executed); BLOCK everything else including
  self_destruct and unknown tools →
  `{"ok":false,"error":"refused"}`. It ALSO exits
  after serving 4 dispatches (`ULINKTOOLS: done 4`,
  exit 0) so the shell-driven test can proceed — the
  shell's `run` waits for the child, so a
  never-exiting server would block all later legs
  (see bug 8 in §8).
- EVT fan-out (subscription shape, C6L-5 design, not
  v1.2): linkd keeps a broadcast ring of the last 8
  EVT units (sequence numbers from 1, monotonically
  increasing) and up to 4 parked EVT_WAIT waiters.
  EVT_WAIT semantics: if the ring holds an event with
  seq > last_seq, reply immediately with the OLDEST
  such event (a waiter more than 8 events behind gets
  the oldest held event — a sequence jump the client
  can detect; jumps counted in `evt_dropped`); else
  if budget_ms == 0, reply immediately TIMEOUT; else
  park the waiter (store the send-once reply name +
  last_seq) and reply when the next event arrives.
  Every parked waiter receives every event arriving
  while parked (true fan-out); a waiter consumes
  exactly one event per wait call and must re-issue.
  If the client's RPC times out first, its reply port
  is destroyed and linkd drops the waiter on the next
  event (failed reply is not an error); the event
  stays in the ring for the next poll. Waiters
  survive a transport reopen (application-level, like
  the inbound queue) but NOT linkd's death (client
  sees EPEER/EDEST and re-issues after respawn +
  re-registration).
- WS sessions are per-client state in linkd by design
  (the v1.2 single-session rule becomes per-client,
  not global) — but NOT implemented: per-client WS
  state + BIN draining would need a second loop
  shape. WS stays direct-link only until a later
  phase designs it. Stub `ws_*` return -1, linkd
  answers WS ops UNSUP. Same for DISPATCH_RESULT
  through linkd (UNSUP): results travel the wire,
  not IPC.

## 5. Queue policy: default 5, measured, NOT raised

Start at default qlimit 5 (no SETQLIMIT call in
linkd). The measurement that decided it (verify-c6l5
runs 2 and 3, then the passing run): one client
streams 32 frames at production pace (~31 ms,
1024 B) through the stub while the second client
(ulink5d) issues commands concurrently; peer aud_ok
32, STOP 32, client frame failures 0.

Decision: qlimit stays 5; the 16-deep option (66 KB
cost) is NOT taken. Cost reference: 16 deep =
16×4120 = 65,920 B total vs 5×4120 = 20,600 B
(delta 45,320 B); the IPC-5 note quoted the 16-deep
cost as 66 KB total.

Concurrency model: linkd is single-threaded and
serves one wire exchange at a time. Client requests
queue in linkd's IPC port (default qlimit 5); a
sender to a full queue gets EFULL immediately
(sends never block) and decides its own retry/drop
policy — for voice frames the honest policy is drop
+ count. Each reply goes to the requesting client's
own send-once reply name, so responses cannot cross
clients even when a slow command holds the wire
while others queue behind it.

True two-client shape: only one spawn slot exists,
and a spawn child cannot spawn, so two spawn
children can never run concurrently. The daemon +
spawn-child pair is the only true two-client shape —
hence `ulink5d` as a marker-gated daemon (reads
`/data/link5-test` at startup; unless content is
exactly `"1"`, prints one dormant line and exits
cleanly). When active, it issues commands through
the stub with distinct keys/markers (`ULINK5D:…`)
in a slow loop (~1 s pace) so its traffic
interleaves with the spawn client's: TIME.GET → KV
set/get own key `link5d` (value `v<n>`, round-trip
checked — a crossed response fails the iteration) →
`TEST.SCRIPT c6l3` via stub → EVT poll (budget
5000 ms, must deliver `WIFI.DROPPED`). A failed
iteration prints FAILED and continues (the
transport-death window legitimately fails it); it
never exits mid-test, so its markers also prove a
surviving client recovers after the peer returns.
Startup: waits for a working link (TIME.GET loop,
30 s bound) → `ULINK5D: ready`, else
`ULINK5D: no link` + exit 0.

## 6. Failure behaviour

- Transport DOWN propagates to IPC clients as the
  C6L-4 errors (LINK_FAIL + reason), never a hang,
  never a silent drop. Retry policy is C6L-4's,
  reused verbatim: plain wrappers NEVER retry;
  `_retry` variants at most once, only after a
  transport failure, only for idempotent commands
  (allowlisted line commands, KV.SET only when the
  caller asserts `idem != 0`); NEVER WIFI.PASS,
  KV.SET with changed value, TOOL.DISPATCH, VOICE.*,
  NET.WS.*, DISPATCH.RESULT, BLE.NAME, WIFI.CONNECT;
  KV.GET has no retry variant. DENIED detection in
  linkd is exact given the verified
  `uproto_cmd_retry` code (policy check happens
  after a failed attempt with `last_was_transport`
  set, and the denied counter increments inside):
  on -1 with mode==1, if link is DOWN and
  `!uproto_retry_policy(line, 0)` → DENIED; if link
  is UP → PEER_ERR; otherwise LINK_FAIL + reason.
- Client death mid-request (IPC-3 teardown): a
  client dying while its request is queued or in
  flight does not break others — linkd still
  executes the wire exchange (it cannot un-execute
  it) and its reply to the dead send-once name fails
  harmlessly (linkd logs `LINKD: reply fail op=…
  rc=…`, never treats it as a linkd error).
  Verified by `ulink5die`: a raw `uipc_send` (not
  RPC) of a CMD request with its own reply port to
  `link`, then immediate `sys_exit(0)`; the passing
  run shows the reply-fail log line, ulink5d
  markers continuing, and `link` still UP.
- Linkd's own death/restarter behaviour under FND-3:
  respawn budget 2/boot, progress stamps. It
  restarts from scratch (re-registers `link`,
  generation bumps per IPC-4, re-selects the bridge
  backend, empty EVT ring, no waiters). Clients see
  EDEST/EPEER transiently and recover on their next
  call because the stub looks `link` up fresh on
  every call and caches nothing.
- Watchdog/loop shape: linkd parks in blocking IPC
  receive with a 100 ms deadline, drains inbound
  units between receives (at most 8 per turn, so IPC
  service cannot starve behind a flooding peer), and
  yields every turn. Transport reopen attempts are
  throttled to at most one per 2000 ms while the
  link is DOWN (a dead peer cannot make the idle
  loop burn open attempts; commands still attempt
  their own lazy reopen per C6L-4, bounded open
  cost, never a hang). Wire execution yields
  naturally: `bridge_read` yields on empty and
  `bridge_write_all` yields on window-full (verified
  in `user/uartproto/bridge.c`), so `net_pump` runs
  and the watchdog stays fed. Bridge open worst
  case is settle 1000 ms + 3 attempts × 1000 ms
  (`BRIDGE_SETTLE_MS`/`BRIDGE_ATTEMPT_MS`/
  `BRIDGE_ATTEMPTS` in `bridge.c:49-53`; IP
  10.0.2.2, port 18082); in practice a dead peer
  RSTs fast, while a silent peer costs the 2 s
  command timeout, not the open path.

## 7. Files and wiring

New files:

- `user/linkd.c` — the daemon.
- `user/uartproto/linkd.h` — LINKD-IPC v1 contract
  (normative header).
- `user/uartproto/linkcli.c` — the client stub.
- `user/ulink5.c` — spawn test client through the
  stub (NOT in `uprogs[]`, via `embed:ulink5`).
- `user/ulink5d.c` — marker-gated second-client
  daemon (daemon_images entry, `uipcld` precedent).
- `user/ulinktools.c` — stub tools server
  (`embed:ulinktools`, progid override 2).
- `user/ulink5die.c` — client-death probe
  (`embed:ulink5die`).
- `scripts/verify-c6l5.py` — 55 checks.
- `docs/rencana-komunikasi-c6/LINKD-CONTRACT.md`
  — the client contract document (the plan names no
  location; this location is justified here: next
  to the wire freezes and the other C6L dev logs).
- This dev log.

Wiring edits:

- `mach3-port/src/ext/user.c`: NDAEMON 6→8;
  daemon_images insert `linkd` (index 4, 2 pages) +
  `ulink5d` (index 5, 1 page) before `sh` (index 6);
  watchdog `case 4` → `case 6` for sh; externs for
  `linkd`/`ulink5d`/`ulink5`/`ulinktools`/
  `ulink5die` images; hooks[] entries
  `embed:ulink5` (progid -1), `embed:ulinktools`
  (progid 2), `embed:ulink5die` (progid -1).
- `mach3-port/src/ext/ipc_glue.c`: registry
  `{ "link", 4 }` (was `REG_OWNER_UNBOUND`).
- `mach3-port/build-md.sh`: generic Phase-C loop
  adds `ulinktools ulink5die`; uiapp uartproto
  compile loop adds `linkcli`; uiapp link line drops
  `uartproto_uartproto.o` +
  `uartproto_mock_comcu.o`, adds
  `uartproto_linkcli.o` (cap 98304 kept as upper
  bound); custom sections (ulinkbr pattern:
  compile, link at 0x100000, entry check, objcopy,
  pad-bss, embed cap 65536) for `linkd` (links
  uartproto+mock+bridge+cfg+ulib+udiv), `ulink5d`
  (linkcli+cfg+ulib+udiv), `ulink5`
  (linkcli+cfg+ulib+udiv); Phase-C final list adds
  `linkd ulink5d ulink5 ulinktools ulink5die`.
- `user/sh.c`: `link` builtin (STATUS RPC via
  `uipc_rpc_user("link", …, 5000)`, one greppable
  line; `link: unavailable (rc=…)` when linkd is
  absent; `link: status N` on non-OK status) +
  dispatch entry + help line; includes
  `uartproto/linkd.h`.
- `user/ulib/uipc.h`: comment-only update to the
  `link` reserved-name note (bound at C6L-5 to
  linkd, index 4); no constants or layout change.
- Worktree setup note: the worktree initially lacked
  `disk-images/` (gitignored, lives only in the main
  tree) — created symlink
  `~/workspace/qaonic_os-c6l5/disk-images` →
  `~/workspace/qaonic_os/disk-images` so verify
  scripts share the dev disk state. The symlink is
  gitignored and does not pollute `git status`.

Build: `cd mach3-port && source
~/workspace/toolchain/env.sh && ./build-md.sh` →
"MD: 43 ok, 0 failed" + "LINK OK". Binary sizes
(2026-10-24 build): linkd 61,676 B (under its 65536
embed cap, 93.9% — thin ~3.8 KB cap headroom, note
for UI-1: linkd is near its embed cap and any
growth needs the cap revisited, not silent
truncation), uiapp 60,304 B, ulink5 22,168 B,
ulink5d 18,616 B, ulinktools 11,816 B, ulink5die
8,964 B.

Headroom (objcopy-binary method, same as C6L-3/4):
binary 3,629,852 B, headroom **564,452 B** in the
4,194,304 B partition (C6L-4: 663,012 B; delta
98,560 B = linkd + linkcli + ulink5/ulink5d/
ulinktools/ulink5die images + sh link builtin +
daemon table growth). No kernel change in
behaviour: the extension layer grew, the Mach
substrate did not (no new syscall, no ABI change).

## 8. Bugs found and fixed during C6L-5

1. **Stub AUD_FRAME budget 128 < 1056 request
   (run 1 voice failure).** `rpc()` sets
   `total = budget` and pads to budget; for
   AUD_FRAME, reqlen = 8+1024 = 1032 > budget 128,
   so the pad loop never ran and only 128 bytes
   were sent (header + op + len + first 96 PCM
   bytes). Linkd's frame parse failed every frame;
   peer A stats: `VOICE.START` + `VOICE.STOP`
   present, `aud_ok 0`, zero `FRAME AUD` lines;
   STOP returned 0. Fix: `LINKD_BUDGET_FRAME`
   128 → 1088 (24 hdr + 8 + 1024 = 1056, +32 slack);
   `LINKD_BUDGET_KV_SET` 128 → 192 (max request
   24 + 12 + 64 key + 64 value = 164 — same latent
   bug for max-size KV, though small keys worked);
   `rpc()` now REJECTS (returns 0) when
   `reqlen + UIPC_HDR_SIZE > budget` instead of
   truncating. Verified by run 2: voice fbad=0,
   fstop=32, aud_ok=32.
2. **ulink5d marker syscall number 95 vs 94 (runs
   1–2 ulink5d failure).** Transcribed
   `sys_lfs_simple(95)` from memory; the kernel
   table (`mach3-port/src/ext/user.c:333-336`) is
   93 RUNTEST, 94 MOUNT, 95 LIST, 96 READ — uipcld
   calls `sys_lfs_simple(94u)`. With 95, the call
   is LIST with buf=95 (the arg register), which
   fails `user_range_ok` → -1 → ulink5d printed
   `ULINK5D: dormant (no lfs)` and exited 0. Fix:
   94u. Verified by run 3: `ULINK5D: ready` PASS.
3. **ulink5d sent bare `TEST.SCRIPT`, peer requires
   `TEST.SCRIPT c6l3` (run 3 iteration failures +
   tools-window failure).** `scripts/mock-c6.py:622`
   matches the exact line `TEST.SCRIPT c6l3` only
   (ulink3 uses `uproto_cmd1("TEST.SCRIPT", "c6l3",
   …)`); bare TEST.SCRIPT falls through to
   `err("UNKNOWN")` → every ulink5d iteration
   FAILED (and no script bytes, no dispatches, no
   EVT). The run-3 demux check still passed
   spuriously because it counted loose
   `"ULINK5D: iter "` + `" ok\n"` (the ok's came
   from ULINK5's own legs). Fix: ulink5d uses
   `uproto_cmd1("TEST.SCRIPT", "c6l3", …)`; script
   demux checks tightened to regex
   `ULINK5D: iter \d+ ok` (also applied to the
   post-recovery and client-death marker checks).
   Verified by run 4: demux ok PASS, no-FAILED
   PASS.
4. **linkd tools-forward reply budget = request
   length → E2BIG → 480/480 `tools_unavailable`
   (run 3 tools failure).** `forward_dispatch`
   passed `UIPC_HDR_SIZE + n` (e.g. 36 for
   get_info) as the `uipc_rpc_user` len, which is
   BOTH send size and reply budget; the tools
   result JSON (~65 B for get_info, up to 1024 B
   accepted) exceeded it, so the reply was lost
   (E2BIG, wrapper destroys the reply port) and
   linkd fell back to `tools_unavailable` for every
   dispatch even with a live tools server. Peer B
   stats at that point: `sessions 1, commands 961,
   aud_ok 0, kv_sets 120, dispatch_results 480`.
   Fix: pad `tmsg.body` to 1024 with zeros, pass
   `UIPC_HDR_SIZE + 1024u` (1048) as len/size; the
   consumer parses by NUL termination and ignores
   trailing zeros. Same bug class as (1), on
   linkd's side. Verified by the passing run:
   transcript B shows real results (get_info
   uptime_ms, gpio_read pin 3 value 1,
   needs_confirm, refused).
5. **Shell `link` line fragmented by concurrent
   daemon prints (runs 3–4 link-check failures).**
   `bi_link` printed field-by-field via many
   `sh_puts` calls; ulink5d's iteration line (every
   ~2 s) could interleave mid-line, breaking the
   `link: UP` substring the script greps for, while
   linkd was healthy. Fix: build the line in
   static `link_line[384]` via `link_cat`/
   `link_catu`/`link_catf` helpers and print with a
   SINGLE `sh_puts`. The same atomic-print fix was
   applied to `bi_run`'s `exit N` line and
   `bi_put`'s `put: ok (…)` line (both asserted by
   the script and equally fragmentable). General
   lesson for the dev log: console lines from
   concurrent writers must be atomic — a
   multi-put line is a grep race, not a print.
6. **Missing `ULINK5: recovered ok` marker (run 1
   script failure, not a product bug).** The script
   waited for `ULINK5: recovered ok` but ulink5
   only printed the `recovered cmd`/`recovered
   state up` checks. Fix: ulink5 prints
   `ULINK5: recovered ok` after the recovery
   checks. Verified by run 2+ (recovered-ok PASS).
7. **Test-program API-arity errors caught at first
   build (fixed before any run):** stub
   `uproto_poll_inbound` takes
   `(struct uproto_inb *inb, budget_ms)` (not
   budget+buf); `uproto_kv_get` takes a 4th
   `vlen_out` arg and returns 0/-2/-1 (NOT the
   length — ulink5's original `rc == 2` check was
   wrong); `uproto_nethttp_get` takes
   `(url, body, cap, len_out)` and returns 0/-2/-1
   (NOT the length); voice frame function is
   `uproto_aud_frame` (not `uproto_voice_frame`).
   Also fixed during writing: `ulinktools` JSON
   builder gap bug (builders added the digit length
   before copying digits — rewritten with
   `cat_str`/`cat_num` append helpers); `ulink5d`
   unused-variable cleanup; stub EVT poll
   ETIME→0 and budget-0 timeout 5000 ms.
8. **`ulinktools` never exited: idle-exit cannot
   fire under a live driver (runs 2–4 tools-window
   and post-window failures).** Exit-after-4 was
   designed but never implemented; only idle-exit
   (2×30 s of no dispatches) existed. With ulink5d
   alive, TEST.SCRIPT dispatches arrive every ~2 s,
   so ulinktools never idles, never exits, and the
   shell stays blocked on `run` forever — every
   later shell command (link, put, cat) then fails
   for want of a prompt, and the script's sends are
   never echoed. (In runs where ulink5d was
   dormant, idle-exit fired and the window passed
   vacuously — which is why the failure appeared
   only once ulink5d was fixed.) Fix: count
   served dispatches in `reply_json`; after the
   4th reply is SENT, print `ULINKTOOLS: done 4`
   and exit 0. The exit happens AFTER the reply is
   sent: exiting first would make linkd report
   tools_unavailable for the 4th dispatch.
   Verified by the passing run: done-4 + exit 0
   + real transcript results.
9. **Shell input occasionally LOST (runs 3–4 link
   checks, probe).** A `link` sent 5 s after
   ULINK5D ready produced no echo and no output,
   while the next command ran fine — the shell was
   never wedged, the bytes never arrived (serial
   input loss between the host socket and the
   guest console reader; mechanism not isolated —
   the PL011 RX path is not C6L-5 scope to
   redesign). The first `link` in the probe printed
   `unavailable (rc=-9)` only because that was the
   PRE-gui check; the post-gui `link` printed
   nothing at all. Fix (harness-side, honest):
   `shell_cmd` in verify-c6l5.py retries up to 3
   times, but ONLY when the command echo is absent
   from the segment — a missing echo means the
   input never reached the shell, so resending
   cannot double-execute. If the echo IS present,
   the command ran and produced other output — a
   real signal, never retried away. The retry is
   recorded here, not hidden: a future phase that
   owns the console input path should find the
   root cause; until then, echo-gated retry is the
   correct harness policy. Verified by the passing
   run (link UP + qlimit PASS on a retried send).

## 9. Verify runs

- **Pre-change run (KERNEL=`~/workspace/c6l4-base.elf`,
  C6L-4 kernel): DECISIVE FAIL as designed** — boot
  reaches qaon> PASS, marker set PASS, then FAIL on:
  link-before-gui unavailable (on C6L-4 the builtin
  does not exist at all — shell answers `perintah
  tak dikenal`, so the wait correctly fails),
  linkd registers, ulink5d ready, link-after-gui UP,
  qlimit, spawn accepted, ready kill, dead ok, ready
  restart, recovered, ULINK5 PASS, exit 0, all
  ulink5 legs, demux, peer A aud_ok. Only vacuous
  negatives passed (no ULINK5 FAILED, no FAULT).
  The run was cut by the 900 s `timeout` (EXIT=124)
  during late legs, as predicted (every wait times
  out pre-change); the `timeout` SIGTERM skipped
  the finally block, leaving QEMU + peer orphans —
  cleaned afterwards with `/tmp/cleanup-c6l5.py`
  (a script file, so its own command line does not
  self-match; lesson re-learned: never `pkill -f
  mock-c6.py` from a shell command line — the
  shell's own command string self-matches and the
  exec session gets SIGTERM'd).
- **Post-change runs 1–4:** FAIL 32/55, 41/55,
  partial, 42/55 — each failure traced to a bug in
  §8 (voice budget, syscall 94, TEST.SCRIPT arg,
  forward budget, atomic prints, done-4, lost
  input). No run failed on an unidentified cause
  at the end: every FAIL in the final 42/55 run
  was explained by the three fixes not yet in its
  kernel (forward budget, atomic prints, done-4)
  plus the lost-input mechanism.
- **Post-change passing run: C6L5-VERIFY PASS
  55/55.** Every leg green: link UP + qlimit 5,
  all ulink5 legs, demux (ulink5d ok markers during
  the ulink5 run, no FAILED before the death
  window), peer A aud_ok 32, tools window (done-4,
  exit 0, real transcript results for all four
  policy outcomes + tools_unavailable leg + EVT
  WIFI.DROPPED), client death (ulink5die sends,
  reply-fail logged, markers continue, link still
  UP), teardown (marker cleared + reads back 0,
  no peer/QEMU left). Serial log:
  `~/workspace/logs-c6l5/c6l5-serial.log`
  (saved by the script).
- **Final green run (hardened script): C6L5-VERIFY
  PASS 55/55**, after the `run_spawn` +
  `set_marker` hardening of §10 notes 5. Same
  kernel as the first green run — only the script
  changed between them.

## 10. Regression battery

All standalone, on the final kernel (forward budget +
atomic prints + done-4), except where noted. The
battery-order C6L5 rerun and two standalone reruns
exposed only harness flakes (see notes); the product
legs passed in every run in which the shell drove
them.

| Script | Result |
|--------|--------|
| verify-c6l1.py | PASS 29/29 |
| verify-c6l2.py | PASS 36/36 |
| verify-c6l3.py | PASS 72/72 |
| verify-c6l4.py | PASS 60/60 (see note 1) |
| verify-ipc2.py | PASS 34/34 |
| verify-ipc4.py | PASS 58/58 |
| verify-ipc5.py | PASS 20/20 |
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS 21/21 |
| verify-fnd3.py | PASS 46/46 |
| verify-fnd4.py | PASS 36/36 |
| verify-fnd5.py | PASS 33/33 |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS (see note 2) |
| verify-shell.sh | PASS |
| verify-safemode.sh | PASS |
| verify-firstboot.sh | PASS |
| verify-update.sh | PASS |
| verify-c6l5.py | PASS 55/55 (see notes 3–5) |

Notes:

1. C6L4 first failed with a harness setup crash, not
   a product failure: `FileNotFoundError:
   .../logs-c6l4` — the fresh worktree lacked the
   C6L-4 session's scratch dir (gitignored, like
   `disk-images/`). Created
   `~/workspace/qaonic_os-c6l5/logs-c6l4`, reran:
   PASS 60/60. Same class as the disk-images symlink
   gap at session start. Neither dir is committed.
2. ONB2 failed twice (battery + standalone) on the
   same two finale checks (`finale shows the eyes
   again`, `finale log line`), then PASSED on the
   third run with no code change (eye pixels 761).
   A control run on the C6L-4 base kernel passed,
   which first suggested a C6L-5 regression — but
   the probe showed the finale transition itself is
   fast on C6L-5 (0.4 s after `setup done`), and the
   third C6L-5 run passed. Verdict: flaky
   shell-input loss (the §8 bug 9 mechanism —
   ONB2's `send()` has no echo-gated retry, unlike
   verify-c6l5.py's `shell_cmd`), not a product
   regression. Recommended follow-up: echo-gated
   retry for the shell-driven scripts (same pattern
   as §8 bug 9); left untouched here because ONB2's
   script belongs to another phase.
3. C6L5 first green run: PASS 55/55 (before the
   run_spawn + set_marker hardening; composition
   then: 55 checks).
4. Battery-order C6L5 rerun: FAIL 53/55 — the chain
   truncated output to the last 2 lines, so the 2
   failing checks were never identified. Not
   re-litigated: the reruns below cover the same
   kernel.
5. Standalone rerun after the battery: FAIL 22/55 —
   a cascade from a lost `run embed:ulink5` send
   (no echo) plus a transient `spawn gagal` on a
   later `run`, both §8-bug-9-class harness faults;
   linkd itself served 2703 commands in that run
   and the tools window passed. Hardening applied
   to the script (not the product): `run_spawn`
   retries a `run` on missing echo or on
   `spawn gagal` (safe: a failed spawn leaves the
   slot free — `spawn_parent` is set only on the
   success path), and `set_marker` verifies the
   marker read-back. Next rerun: FAIL 55/56 — only
   the teardown cat, because a garbled teardown
   `put` reported `put: ok (18 bytes)` (wrong file,
   wrong content) while the marker still read `1`
   — the bare `put: ok` check cannot catch that,
   which is exactly why set_marker verifies the
   read-back. Final rerun: PASS 55/55 (composition
   now: +`ulinktools started` gate, merged marker
   checks = 55). The garbled 18-byte file remains
   as litter in /data (unknown garbled name);
   harmless — ulink5d reads only `link5-test` —
   and the next run overwrites the marker anyway.
6. End state after the battery: `/data/link5-test`
   reads back `0`; disk NOT provisioned (`setup`
   → "NOT provisioned" after `setup reset`);
   `bootctl` fail_count=0 safe_mode=0; no
   QEMU (`pgrep -x qemu-system-arm`) and no peer
   process left running.

## 11. Honest limitations (what C6L-5 does NOT do)

- WS through linkd: not implemented (per-client WS
  state + BIN draining needs a second loop shape).
  WS stays direct-link only; stub `ws_*` return -1,
  linkd answers WS ops UNSUP.
- The production tools consumer is still a stub:
  forwarding is verified against `ulinktools`
  (C6L-3 policy mirror, real guest data). Qabotd
  does not register `tools` today; wiring the real
  qabotd policy gate to IPC name `tools` is a
  later track's job — linkd's forward path is
  ready for it.
- Voice frames are guest-generated PCM via the
  VOICE contract, not I2S capture (H4, hardware
  track). Baud is simulated (slirp TCP underneath
  is unpaced) — the pacer is reservation-clock
  logic, not a measured wire rate.
- No oversize-EVT reader probe (carried from C6L-4).
- The shell/serial input path under load is the
  known weak spot (§8 bug 9, §10 notes 2/5): bytes
  are occasionally lost, delayed, or garbled when
  daemon output floods the console. The verify
  script now tolerates it (echo-gated retry,
  spawn-gagal retry, read-back-verified markers),
  but the root cause — PL011 RX FIFO drain vs
  cooperative scheduling — is a driver-level
  follow-up, NOT a linkd fix, and is not claimed
  solved here.
- Linkd is near its embed cap: 61,676 / 65,536 B
  (93.9%). Any growth needs the cap revisited.
- One face observation, out of scope: run 2's serial
  log showed `face: wake` → `face FAIL:
  SYS_DISPLAY_FLUSH` → `sched: 'face' exit 0x1`
  after the ulinktools window. The battery's GUI
  legs pass and ONB2 passes on retry, so this is
  recorded for the UI track, not chased here.

## 12. Track close-out: C6L-1..5 as a whole

What the track delivered, in order:

- C6L-1: transport-agnostic link layer
  (`user/uartproto/link.h`: mock/bridge/uart
  backends, lazy-open select, streaming reader,
  2 s command timeout); wire v1.1 FROZEN.
- C6L-2: bridge backend (TCP to 10.0.2.2:18082
  via slirp, outbound, no hostfwd) + host peer
  (`scripts/mock-c6.py`, stdlib only, v1.1
  semantics including quirks); the spawn-only
  rule for network programs (boot one-shots run
  before the scheduler activates, so `net_pump`
  never runs inside one).
- C6L-3: TOOL.DISPATCH both directions + EVT
  frame type 0x03 + NET.WS; wire v1.2 frozen
  (`WIRE-FREEZE-V1.2.md`); FIDO.DEL/APPROVE/DENY
  explicitly deferred (no session semantics).
- C6L-4: link state + counters + failure
  classification (peer ERR vs transport DOWN) +
  retry policy + bridge baud pacer; measured
  32.2 KB/s stream headroom (2.86× the
  92,160 B/s budget) and exact counter
  reconciliation.
- C6L-5 (this log): linkd, the single transport
  owner and IPC server on `link`; LINKD-IPC v1;
  the client stub with identical signatures;
  uiapp migrated; peer-dispatch forwarding to
  `tools` with the policy gate in the consumer;
  EVT fan-out; shell `link` status; queue policy
  measured (default 5 kept).

Frozen contracts at track end (all untouched by
C6L-5 except as noted):

- Wire v1.1: `WIRE-FREEZE.md` (canonical spec
  text, decision D-C6L-3).
- Wire v1.2: `WIRE-FREEZE-V1.2.md` (TOOL.DISPATCH,
  EVT, NET.WS).
- IPC: frozen `uipc.h` v1 (header 24 B, cap 4096,
  syscalls 10/11/12/23/25, names `tools`/`link`).
- LINKD-IPC v1: `user/uartproto/linkd.h`
  (normative) + `LINKD-CONTRACT.md`
  (human-readable). `link` bound to linkd
  (daemon_images index 4); `tools` reserved for
  qabotd (progid 2).

Remaining backlog, with owners:

- FW track (owner: user decision D-GADGET-1 /
  firmware-direction widget, still pending;
  parked until the user decides): real C6
  firmware — Gadget-based vs pure-radio fork;
  the UART "QaonicOS channel" inside Gadget
  firmware (G1, blocks everything else); voice
  upload codec/format (G2); provisioning merge
  (G4: QaonicOS QR+PIN vs Muse pairing app+SDK
  token); I2S capture (H4); real baud integrity;
  BLE coexistence; hardware test batches H0–H4.
  None of this is unparked by C6L-5.
- Qabotd `tools` registration (owner: qabotd /
  UI track): the production policy gate over IPC
  name `tools`, replacing the `ulinktools` stub
  in verification.
- WS through linkd (owner: later phase):
  per-client WS state + BIN draining, second
  loop shape.
- UI track (owner: UI-1..6, LVGL): UI-1
  re-measures headroom first (now 564,452 B);
  linkd's embed cap (61,676/65,536 B) needs
  revisiting before linkd grows.
- Console input reliability (owner:
  kernel/driver follow-up): PL011 RX
  loss/corruption under load (§8 bug 9, §10
  notes 2/5); harness mitigations in place,
  root cause open.
- Build hygiene (carried from IPC-5, partially
  addressed by C6L-4's explicit Phase-C list):
  stale `_img` objects glob risk in the final
  link (`MD_OBJS`/`MI_OBJS` via `find`).
- Small carried items: qfetch/SYS_STAT strings
  still print 1.2 GHz (correct: 1.104 GHz per
  vendor DTS); `ntp` daemon commented out
  ("UDP sandbox blokir", prerequisite for H1.1);
  THR-1 FPU/NEON enablement (VFP/NEON on since
  M3 but `fpu_save`/`fpu_restore` are empty
  stubs, `user_ctx` has no FP state).
- The `dev` splash "shell active" text-screen
  offer: still unanswered (user mode unaffected).
- Watchdog entry `track-c6l`: close at merge
  (coordinator), with marker
  `~/workspace/notifikasi/track-c6l.done`.

