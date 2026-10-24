# DEVLOG-C6L3 — v2 completion: TOOL.DISPATCH, EVT, NET.WS framing

Phase: C6L-3 (third phase of the C6 link track).
Branch: `dev/c6l3` (worktree `~/workspace/qaonic_os-c6l3`).
Base: C6L-2 merge (`f7f82cf`).
Status: DONE, verified. No merge, no push (coordinator's job).

## 1. What was built

The peer-initiated half of v2 exists and is tested.
Three wire additions (frozen as wire v1.2, see
`WIRE-FREEZE-V1.2.md` -- v1.1 itself untouched):

1. **TOOL.DISPATCH** (guest-originated): the guest
   calls a tool on the peer over the link and gets a
   structured JSON result back.
2. **Peer-initiated DISPATCH + DISPATCH.RESULT**:
   the peer calls a tool on the guest; the guest
   answers through its policy classes.
3. **EVT** (binary frames, type 0x03) and **NET.WS**
   framing (BIN frames, type 0x02, echo) -- the
   async and data-plane halves.

No kernel change. No new syscall, no ABI change.
The whole phase is userspace link-layer code
(`user/uartproto/`), both peers, and one guest test
program.

## 2. The direction question, resolved explicitly

The brief and the plan point the dispatch wire in
opposite directions, and both are right about a
different half:

- The brief: guest-originated dispatch -- "a guest
  client drives a tool dispatch through the C6".
  That is what C6L-5's linkd needs for guest IPC
  clients asking C6-side tools.
- PLAN.md C6L-3: peer-initiated dispatch -- "peer
  sends a dispatch (JSON tool call); the link hands
  it to qabotd's tool path through the policy
  gate". That is what the companion needs to ask
  guest-side tools.

Resolution: the wire is symmetric, one shape both
ways. Guest-originated dispatch is a text command
(`TOOL.DISPATCH <tool> <len>\n<args-json>` ->
`OK <len>\n<result-json>`); peer-initiated dispatch
is a text line (`DISPATCH <id> <tool> <len>\n
<args-json>`) answered by a text command
(`DISPATCH.RESULT <id> <len>\n<result-json>` ->
`OK`). Both directions are implemented, both
peers speak both, and the guest test proves both.
Picking only one direction would have left half
the plan's verification criteria unmet; picking
two different wire shapes would have doubled the
surface for no reason.

## 3. The link-layer reader (the core of the phase)

`user/uartproto/uartproto.c` grew a unit reader.
The stream mixes text lines and binary frames, so
the reader parses one unit at a time:

- "QA" at a unit boundary starts a frame (type,
  len BE, payload, CRC BE, CRC-checked over header
  + payload). Anything else starts a text line (a
  line starting with 'Q' but not "QA" is text, via
  a 2-byte pushback).
- Queueable units (EVT frames, DISPATCH lines) met
  during a command response are queued and the
  response read continues. A BIN frame during a
  text wait is a link-reset error (WS data must be
  consumed promptly by uproto_ws_recv()). A stray
  text line during a poll is a link-reset error.
- The inbound queue holds 4 units; overflow drops
  + counts (uproto_inb_dropped()), never fails the
  in-flight command. Queued units survive a backend
  reopen (application-level, not transport state);
  the byte stream itself is cleared on reopen
  (staged bytes + pushback dropped).
- The C6L-1 staged-byte drops in do_cmd/kv/nethttp
  are REMOVED: trailing bytes in staging may be
  queued inbound units, and the unit reader
  consumes them in order. A failed exchange
  recovers by backend reopen, which is the only
  real resync (the old drops never cleared backend
  buffers anyway).

New API (uartproto.h): uproto_poll_inbound()
(1 unit / 0 silence-in-budget / -1 error),
uproto_inb_flush(), uproto_inb_dropped(),
uproto_tool_dispatch() (0 / -1 wire / -2 peer
ERR with the code copied out), uproto_dispatch_
result(), uproto_ws_open/send/recv/close().

Caps, all documented in v1.2: EVT payload <= 256,
DISPATCH args <= 512 inbound, outbound dispatch
bodies <= 192 (so one line+body write always fits
the in-process mock's 256-byte accumulator),
frame payload <= 1024 (frozen), queue 4 units.

### Known edge, accepted

A scan SSID line shaped EXACTLY like a dispatch
header ("DISPATCH <digits> <token> <digits>") in a
multi-line response would be swallowed as a
dispatch. The grammar check is exact (full word +
spaces + numeric fields), the mock's canned SSIDs
do not match it, and a real-world SSID matching
it is contrived. Same class of ambiguity as the
"QA"-magic check, guarded the same way (exact
grammar + CRC for frames). Noted, not fixed:
fixing it would need a response-context flag
plumbed through every reader, for a case that
cannot occur with the frozen command set's
response shapes (only SCAN/LIST have free-text
body lines, and their lines are comma-shaped).

## 4. TOOL.DISPATCH (guest -> peer)

Wire: `TOOL.DISPATCH <tool> <len>\n<args-json>`
-> `OK <len>\n<result-json>` or `ERR <CODE>`.
Result JSON: `{"ok":true,"result":...}` or
`{"ok":false,"error":"..."}`. Unknown tool is
`ERR NOTOOL` (new wire code), not a JSON error.
Over-long results are consumed and reported as
TRUNC, never delivered partial (KV/NET.HTTP
house style).

Stub tools, identical in both peers: get_info
(peer identity), gpio_read (pin table value =
pin & 1; missing pin is ERR ARG), fail_test
(always a JSON tool failure). The stubs compute,
they do not echo: gpio_read(3) = 1 and
gpio_read(4) = 0 are asserted, not reflected.

## 5. Peer-initiated DISPATCH + policy mirror

Wire: `DISPATCH <id> <tool> <len>\n<args-json>`
(peer -> guest), `DISPATCH.RESULT <id> <len>\n
<result-json>` -> `OK` (guest -> peer). The id
is echoed verbatim so the peer matches answers.

Policy mirror (user/qabot/tools.c + policy.c,
read, not copied): ALLOW = get_info, gpio_read
(executed by the test consumer: get_info returns
real guest uptime_ms, gpio_read returns the pin
table); CONFIRM = gpio_write, file_write (answer
needs_confirm, never executed -- the confirm UX
is ONB scope, per the plan); BLOCK = everything
else, including self_destruct (not in the
registry; policy.c blocks unknown tools).

The test consumer is a stand-in, stated plainly:
C6L-5's linkd will forward peer dispatches to
qabotd over IPC name `tools` (SYS_RPC 12, frozen
uipc.h v1), qabotd will execute through its real
gate, and linkd will send the result back. C6L-3
proves the wire, the classification, and the
reply shape; it does not wire qabotd (qabotd does
not link uartproto, and giving it a link would be
a new surface, not a test).

## 6. EVT

Wire: v2 frame, type 0x03, payload = one ASCII
event line (e.g. `WIFI.DROPPED`), cap 256 bytes.
Events are never answered. The mechanism does not
enumerate events: FIDO.EVENT and future events
ride the same frames with no wire change. First
consumer per the plan: link status + test
assertions; the Wi-Fi screen update on
WIFI.DROPPED is UI scope (the event arriving
intact with the stream in sync is what C6L-3
proves).

## 7. NET.WS (framing only)

Wire: `NET.WS.OPEN <url>` -> `OK <sid>`;
`NET.WS.CLOSE <sid>` -> `OK`; data rides BIN
frames (type 0x02), payload = raw message, peer
echoes verbatim while the session is open. One
session at a time (sid always 1); second OPEN is
ERR BUSY; CLOSE with no session/wrong sid is
ERR NOCONN; BIN with no session is dropped
(frames get no response) and the guest send
reports -1; bad-CRC BIN is rejected, no echo.
WS helpers return 0 / -2 truncated (full length
reported, frame consumed) / -1, the KV/NET.HTTP
house style. No product consumer yet, per the
plan; multiplexing and cloud endpoints are
future work.

## 8. Peers

Both peers implement the full v1.2 surface with
identical script bytes for the test commands:

- In-process mock (user/uartproto/mock_comcu.c):
  body-phase generalised from KV.SET-only to
  KV.SET / TOOL.DISPATCH / DISPATCH.RESULT
  (kind flag + tool/id tracking, 192-byte staging,
  same grammar rules); WS session + BIN echo in
  mock_send_frame (CRC-validated; echo fits resp_q
  only for small messages, which the test keeps
  small); TEST.SCRIPT queues 4 dispatches + 1 EVT
  frame (158 bytes total, inside the 256-byte
  resp_q); TEST.BADFRAME queues one corrupt-CRC
  EVT frame; DISPATCH.RESULT receipts captured
  (up to 4) and exposed via the test-only
  mock_comcu_script_result() API.
- Host peer (scripts/mock-c6.py): same surface
  over TCP; body-phase generalised the same way;
  frame handler routes AUD (counted), BIN (echo
  while open, else drop + count), EVT-from-guest
  (logged, ignored); TEST.SCRIPT/TEST.BADFRAME
  send OK first, then script bytes (bypassing
  fault injection, logged as SEND lines);
  stats extended (tool_dispatch, dispatch_results,
  ws_echo, ws_drop); initial stats file written
  at startup so verify scripts fail cleanly (not
  crash) when no session ever connects.

## 9. The uvoice cap (build lesson)

uvoice links the full uartproto + mock, so the
v1.2 growth pushed its padded image to 44,052
bytes, over its 32,768-byte embed cap. Two fixes:
the link layer's frame buffers were merged
(unit_payload + send frame -> the single
frame_tmp, 1031 bytes; consumers copy parsed
frames out before any send, so sharing is safe),
and uvoice's cap was raised to 65536 (same as
the other link test programs). The shared-layer
growth is inherent to the track, not bloat: every
program linking uartproto pays it, and UI-1's
size audit should account for it (already a
planned audit). Build-hygiene backlog (stale
_img objects glob-linked, IPC-5) is untouched by
this phase -- the Fase C loop gained exactly one
entry (ulink3), no stale objects were introduced.

## 10. The __aeabi_memcpy4 lesson

The first build failed linking uiapp: the inbound
queue's struct assignment lowered to
__aeabi_memcpy4, which exists only in the
kernel's aeabi.c, not in user programs (udiv
provides memcpy/memcpy8). Fix, house style:
field-by-field copies with runtime-bounded loops
(NUL terminator / len), which never lower to a
memcpy call. No struct copies of queue entries
anywhere in the link layer now. (A future
alternative -- adding __aeabi_memcpy4 to udiv.c
-- was considered and rejected as scope growth:
the codebase's user programs avoid the pattern,
and this phase should follow, not change, it.)

## 11. Verification (test-first)

scripts/verify-c6l3.py (new, modelled on
verify-c6l2.py): boots QEMU, checks ULINK: PASS
+ UVOICE: PASS regressions, sends
`run embed:ulink3`, asserts every ULINK3 marker
plus peer transcript/stats evidence.

- Pre-change (C6L-2 kernel from the main tree,
  no embed:ulink3 hook): C6L3-VERIFY: FAIL
  (7/71). The 7 passes are only the boot
  regressions (qaon> prompt, ULINK: PASS,
  UVOICE: PASS) and the absence checks (no
  ULINK3: FAIL, no ULINK3: SKIP, no FAULT, no
  peer left running). Every ULINK3 marker, the
  exit-0 check, and every peer stat/transcript
  check failed: the shell answered `spawn gagal`
  and no ULINK3 line ever appeared. The run took
  313 s (60 s banner wait + 240 s PASS wait),
  consistent with a program that never started.
  (Pre-change total is 71, not 72: it ran before
  the bridge WS markers were split into
  sent/dropped, see §7.)
- Post-change (worktree kernel): C6L3-VERIFY:
  PASS (72/72), first run, 13 s. Zero FAIL
  lines. All 42 leg markers + no-drops + PASS,
  all 4 peer stats (5 dispatches, 4 results,
  1 WS echo, 2 WS drops), all 15 transcript
  checks (scripted DISPATCH 1-4, EVT, the four
  RESULT bodies field-checked by regex,
  guest TOOL.DISPATCH x4, NOTOOL, WS echo
  frame).

Guest program user/ulink3.c (spawn-only, NOT in
uprogs[]): two legs (mock, then bridge with SKIP
when no peer listens), 41 markers + no-drops
marker + ULINK3: PASS/FAIL, sys_exit(fails).
Checks per leg: script armed; 4 dispatch
answers in script order (get_info, gpio_read,
gpio_write confirm, self_destruct refused); EVT
observed; mock-leg dispatch receipts read back
(ids 1-4, uptime_ms / pin 3 value 1 /
needs_confirm / refused); bad EVT -> link error
-> reopen -> TIME.GET ok; WS preopen drop
(mock: synchronous reject; bridge: send ok +
no echo) / open sid 1 / busy / echo / bad-CRC
drop (same backend split) / close / noconn;
guest dispatch get_info / gpio_read x2 /
fail_test / NOTOOL; final TIME.GET (stream in
sync); global no-drops. Total: 42 leg markers
+ no-drops marker + ULINK3: PASS.

## 12. Battery (standalone, worktree kernel)

Battery order (one loop, sequential): all 13
Python verifies PASS first try, no flakes, no
reruns:

| Script | Result |
|---|---|
| verify-c6l1.py | PASS 29/29 |
| verify-c6l2.py | PASS 36/36 |
| verify-c6l3.py | PASS 72/72 |
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS 21/21 |
| verify-fnd3.py | PASS 46/46 |
| verify-fnd4.py | PASS 36/36 |
| verify-fnd5.py | PASS 33/33 |
| verify-ipc2.py | PASS 34/34 |
| verify-ipc4.py | PASS 58/58 |
| verify-ipc5.py | PASS 20/20 |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS |

The four .sh scripts did NOT pass in battery
order, for harness/state reasons, and all four
passed standalone on the same kernel:

- verify-shell.sh: FAIL in battery order (its
  last 4 checks passed; one early check missed).
  Standalone: PASS, all 10 checks. No C6L-3
  regression is possible here: .text is
  byte-identical to C6L-2 (no kernel/shell code
  changed), and this script's serial harness is
  demonstrably racy (a standalone attempt died
  with a socket EINVAL before any prompt).
- verify-firstboot.sh: silent instant exit in
  battery order (see the harness note below).
  Standalone, first attempt: FAIL with exactly
  the 3 boot-1 precondition lines (banner /
  UFIRST / NOT-provisioned) -- the shared disk
  was provisioned by earlier runs, and the
  script does not scrub at start. After one
  `setup reset` boot (the script's own boot-2
  ending, reused as a scrub): PASS. It ends with
  `setup reset`, so the disk is left
  unprovisioned again.
- verify-safemode.sh: silent instant exit in
  battery order. Standalone: PASS (boot1
  fail_count=0 healthy, boot2 safe mode after
  the armed crash, boot3 normal, ending
  fail_count=0).
- verify-update.sh: silent instant exit in
  battery order. Standalone: PASS (5 boots;
  boot1 started at v261/slot B -- carried update
  state from prior runs, so the script's checks
  are transition-relative, not absolute v1/A
  as its header comment suggests; boot5 back to
  v262 idle after the rollback).

### Harness finding (pre-existing, not C6L-3)

verify-shell.sh uses `set -e`, so when its
Python check FAILs, the shell exits BEFORE the
`kill $QPID` lines: the battery shell FAIL left
its QEMU alive, holding the disk-image lock.
The next three .sh scripts' QEMUs then failed
instantly at startup (lock conflict) with errors
containing no PASS/FAIL string, which is why
the battery grep showed them as silent
(timestamps: each finished in ~1-2 s, no
boots). Evidence: leftover QEMU 30905 found
after the first standalone shell attempt, and
all four scripts passing standalone once
leftovers were killed by exact name. Follow-up
for the coordinator (not fixed here, out of
phase scope): give the .sh scripts an EXIT trap
that always kills their QEMU, so one FAIL
cannot poison the rest of the battery.

### Final disk state (confirmed by a boot after
the battery)

`bootctl`: fail_count=0, safe_mode=0;
`setup`: NOT provisioned. No QEMU and no peer
process left running (verified with
`pgrep -x qemu-system-arm` and a self-match-
proof `pgrep -f "mock-c6[.]py"`).

## 13. Headroom

Method: `llvm-objcopy-18 -O binary` on the linked
kernel, against the 4,194,304-byte partition
(the C6L-2 figure of 844,604 B was measured by
a different, unrecovered method and is NOT
directly comparable -- the comparable numbers
below are all objcopy-binary):

- C6L-3 binary: 3,455,616 B; headroom 738,688 B.
- C6L-2 binary (same method): 3,419,644 B;
  headroom 774,660 B.
- Phase delta: +35,972 B, ALL in .rodata
  (.text identical at 427,796 B both builds --
  no kernel code changed, as designed). The
  growth is the ulink3 embedded image plus the
  uartproto growth inside the ulinktest /
  ulinkbr / uvoice images that link the shared
  layer. ELF sizes for reference: C6L-3
  3,636,928 B vs C6L-2 3,601,720 B (+35,208 B;
  the ELF/binary deltas differ by symbol-table
  content, not code).

## 14. Limitations / carried forward

- linkd is C6L-5 (the §6 mapping is design, not
  code). qabotd is not wired to the link here.
- Confirm UX is ONB scope (CONFIRM answers
  needs_confirm and stops).
- WS is single-session, framing only; no
  consumer, no multiplexing, no cloud endpoint.
- FIDO.DEL/APPROVE/DENY deferred (no defined
  session semantics in any plan section;
  inventing them here would be scope growth).
  FIDO.EVENT rides the generic EVT mechanism.
- NET.HTTP chunked binary bodies still
  host-peer-only for GET (C6L-2 note, unchanged).
- The dispatch race (§1) is handled by
  queue + timeout + reopen + retry; a guest
  that never answers a dispatch stalls the
  peer's script (test failure, not wire state).
- AUD integrity at simulated 921600 is C6L-4.
