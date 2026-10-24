# C6 link: wire amendment v1.2 (C6L-3)

Versioned amendment to the frozen wire v1.1
(`docs/rencana-komunikasi-c6/WIRE-FREEZE.md`).
v1.1 is NOT rewritten here: every v1.1 rule stays in
force unless this document explicitly amends it, and
the v1.1 text remains the record for the v1.1 command
set. This document defines only what C6L-3 adds:
TOOL.DISPATCH (guest-originated), peer-initiated
DISPATCH + DISPATCH.RESULT, EVT as a live inbound
unit kind, and NET.WS framing. Later phases cite
v1.1 + v1.2 together; a future amendment (v1.3)
amends this one the same way.

Status: frozen at C6L-3. Changes after this point
need a new versioned amendment, not a silent edit.

## 0. What v1.2 does NOT change

- The v1.1 command table, KV framing, multi-line
  rules, and the v2 frame format (magic "QA", types
  AUD 0x01 / BIN 0x02 / EVT 0x03, len 2B BE, CRC-16-
  CCITT over header + payload) are unchanged.
- Strict request/response alternation still holds
  for commands, with exactly the two inbound
  exceptions in §1: EVT frames and DISPATCH lines
  may arrive at any time. Everything else
  unsolicited is still a link-reset event.
- The per-command timeout (2000 ms) is unchanged.
- The mock backend's NVS/Wi-Fi/BLE semantics are
  unchanged; the host peer mirrors them (C6L-2).

## 1. Inbound units (the C6L-3 reader)

The stream mixes text lines and binary frames. The
link layer's unit reader (user/uartproto/uartproto.c)
parses one unit at a time:

- At a unit boundary, bytes "QA" start a binary
  frame (type, len BE, payload, CRC BE, CRC-checked).
  Any other first byte starts a text line (a line
  starting with 'Q' but not "QA" is read as text).
- A frame with a bad CRC, or a declared length over
  LINK_MAX_PAYLOAD (1024), is a link-reset error:
  the caller reopens the backend. No partial unit is
  ever delivered.
- Queueable units (EVT frames, DISPATCH lines) met
  while reading a command response are queued, and
  the response read continues past them. A BIN data
  frame met during a text wait is a link-reset error
  (WS data must be consumed promptly by
  uproto_ws_recv(); see §4). A stray text line met
  during a poll is a link-reset error (the peer never
  sends unsolicited text except DISPATCH lines).
- The inbound queue holds 4 units. A unit arriving
  at a full queue is dropped and counted
  (uproto_inb_dropped()); the in-flight command is
  never failed for a full queue.
- Queued units survive a backend reopen: units are
  application-level, not transport state. The byte
  stream itself IS cleared on reopen (staged bytes
  and pushback are dropped), so a failed exchange
  cannot poison the next one. uproto_inb_flush()
  drops queued units for a fresh phase.
- Queueable-unit caps (parse-time link-reset errors
  when exceeded): EVT payload <= 256 bytes,
  DISPATCH argument body <= 512 bytes.

### The dispatch race, stated plainly

A peer DISPATCH line and a guest command can cross
in flight (both sides believe the other is idle).
The guest side handles it without losing the
dispatch: the DISPATCH line is queued during the
response read, the command itself times out (the
peer is waiting for a dispatch result, not
answering), the caller reopens, retries the
command, and the queued dispatch is still there to
be polled and answered. The dispatch is never
silently dropped by the race; the command is never
answered with dispatch bytes.

## 2. TOOL.DISPATCH (guest-originated tool call)

Wire: `TOOL.DISPATCH <tool> <len>\n<args-json>` ->
`OK <len>\n<result-json>` or `ERR <CODE>`.

- `<tool>`: no spaces, max 31 chars. `<len>`:
  decimal byte count of the JSON argument body.
  Outbound bodies are capped at 192 bytes (the
  peers' dispatch body cap; the guest refuses
  longer bodies without sending anything, mirroring
  the KV 64-byte guard).
- The result body is JSON: `{"ok":true,"result":...}`
  when the tool ran, `{"ok":false,"error":"..."}`
  when the tool ran and reported a failure. The
  dispatch itself succeeded in both cases.
- A tool the peer does not know is a wire error,
  not a JSON error: `ERR NOTOOL` (new wire code).
  Other wire codes from v1.1 (ARG, NOTFOUND, AUTH,
  NOCONN, FULL, UNKNOWN) keep their meanings.
- Guest helper: uproto_tool_dispatch() returns 0
  (result filled), -1 (wire error/timeout, or an
  over-long result consumed and reported as TRUNC,
  never delivered partial), -2 (peer ERR, with the
  wire code copied out).

### Stub tools (both peers, identical semantics)

- `get_info` (args ignored): peer identity.
  In-process mock: `{"device":"mock-comcu",
  "link":"v1.2"}`. Host peer: `{"device":"mock-c6",
  "link":"v1.2"}`. A real C6 (FW-1) returns its own
  identity here; the shape, not the values, is the
  contract.
- `gpio_read` (args `{"pin":N}`): `{"pin":N,
  "value":V}` with V = N & 1 (deterministic pin
  table shared by both peers; FW-1 reads real pins).
  Missing/unparseable pin is `ERR ARG`.
- `fail_test` (args ignored): always
  `{"ok":false,"error":"test_failure"}` -- the JSON
  failure path, distinct from the wire NOTOOL path.

## 3. Peer-initiated DISPATCH + DISPATCH.RESULT

Wire (peer -> guest):
`DISPATCH <id> <tool> <len>\n<args-json>`.
Wire (guest -> peer):
`DISPATCH.RESULT <id> <len>\n<result-json>` -> `OK`.

- `<id>`: decimal, echoed back verbatim in the
  result so the peer can match answers to calls.
  `<tool>`/`<len>` follow the §2 grammar; the
  argument body cap on this direction is 512 bytes
  (the guest's inbound queue entry size).
- The guest surfaces peer dispatches through
  uproto_poll_inbound() (kind DISPATCH, with id,
  tool, and args). The consumer answers with
  uproto_dispatch_result(). There is no timeout on
  the answer: the peer waits for the result as part
  of its own script (a guest that never answers
  stalls the peer's script, which is a test
  failure, not a wire state -- production linkd
  answers promptly; see §5).
- Result JSON uses the §2 shape. Policy outcomes
  are JSON tool failures, not wire errors:
  CONFIRM-class tools return
  `{"ok":false,"error":"needs_confirm"}` (the
  confirm UX itself is ONB scope, not here);
  BLOCK-class tools (including tools the guest
  does not know at all) return
  `{"ok":false,"error":"refused"}`.
- The guest-side policy mirror for C6L-3 (from
  user/qabot/tools.c + policy.c): ALLOW =
  get_info, gpio_read (executed by the test
  consumer with real guest data: uptime_ms, pin
  table); CONFIRM = gpio_write, file_write
  (needs_confirm, never executed); BLOCK =
  everything else, including self_destruct (not
  in the registry; policy.c blocks unknown tools).
  This table is a stand-in: C6L-5's linkd
  forwards peer dispatches to qabotd's `tools`
  IPC name, where the real gate decides (§5).

## 4. EVT (peer-initiated events)

Wire: a v2 frame, type 0x03, payload = one ASCII
event line (no trailing newline in the payload).
Payload cap: 256 bytes (§1).

- Defined event names in v1.2: `WIFI.DROPPED`
  (optionally followed by a space + SSID). Other
  names are carried opaquely: the mechanism does
  not enumerate events, so FIDO.EVENT and any
  future event ride the same frames with no wire
  change.
- The guest surfaces events through
  uproto_poll_inbound() (kind EVT, payload = the
  event line). Events are never answered: there is
  no EVT response frame, and the peer does not wait
  for one.
- First consumer (per the plan): a link status
  line + test assertions; WIFI.DROPPED updates the
  Wi-Fi screen state (screen work itself is UI
  scope, not here -- C6L-3 proves the event
  arrives intact and the stream stays in sync).

## 5. NET.WS (framing only)

Wire: `NET.WS.OPEN <url>` -> `OK <sid>`;
`NET.WS.CLOSE <sid>` -> `OK`; data rides BIN
frames (type 0x02), payload = the raw message
(<= 1024 bytes). The peer echoes the payload
verbatim while the session is open.

- One session at a time (sid is always 1 in
  v1.2). A second OPEN while one is open is
  `ERR BUSY`. CLOSE with no open session, or a
  wrong sid, is `ERR NOCONN`.
- A BIN frame sent with no open session is
  dropped by the peer (frames get no response).
  Whether the guest send reports -1 depends on
  the backend: the in-process mock rejects
  synchronously (-1), because it knows; over the
  bridge the send succeeds (TCP accepted it) and
  the rejection surfaces as absence of echo,
  proven by the peer's ws_drop stats. A BIN
  frame with a bad CRC behaves the same way on
  both backends. The guest test asserts each
  backend's own semantics; pretending the bridge
  rejects synchronously would be a false
  assertion.
- Guest helpers: uproto_ws_open(),
  uproto_ws_send(), uproto_ws_recv() (0 full, -2
  truncated with the full length reported, -1
  error), uproto_ws_close().
- WS sessions survive a transport reopen on both
  peers (session state, not parser state) -- but
  v1.2 tests never depend on that: sessions are
  opened and closed inside one transport session.
- No product consumer exists yet (per the plan).
  Multiplexed sessions, subprotocols, and any
  cloud endpoint are future work, not v1.2.

## 6. C6L-5 mapping (linkd; not built here)

- linkd owns the single transport (D-C6L-4) and
  multiplexes on one IPC port (IPC-5 verdict).
  Guest IPC clients dispatch C6-side tools by
  asking linkd, which issues §2 TOOL.DISPATCH on
  the wire and returns the result JSON.
- Companion-initiated dispatches arrive as §3
  DISPATCH units; linkd forwards them to qabotd
  over IPC name `tools` (SYS_RPC 12, frozen
  uipc.h v1), qabotd executes through its real
  policy gate, and linkd sends the §3 result back.
  The C6L-3 test consumer (ulink3) stands in for
  linkd + qabotd with the §3 policy mirror.
- EVT units are fanned out by linkd to subscribed
  IPC clients (subscription shape is C6L-5
  design, not v1.2). WS sessions are per-client
  state in linkd; the v1.2 single-session rule
  becomes per-client in linkd, not global.

## 7. Test-only commands (never on the real wire)

- `TEST.SCRIPT c6l3` -> `OK ARMED`, then the peer
  sends, in order: DISPATCH get_info, DISPATCH
  gpio_read, DISPATCH gpio_write, DISPATCH
  self_destruct, EVT WIFI.DROPPED. The guest
  polls and answers them; the peer records the
  results (host transcript / mock receipt API).
- `TEST.BADFRAME` -> `OK`, then one EVT frame
  with a corrupt CRC. The guest must report a
  link error (never the event), reopen, and
  carry on. This is the malformed-frame probe
  for §1.
- These commands exist in both peers (in-process
  mock + host peer) with identical script bytes.
  FW-1 must NOT implement them.

## 8. Conformance checklist (for a new peer)

A peer conforms to wire v1.2 if, in addition to
the v1.1 checklist, it passes scripts/verify-
c6l3.py against the same client code: guest
TOOL.DISPATCH round-trips for get_info,
gpio_read (pins 3 and 4), fail_test, and unknown
tool (NOTOOL); peer DISPATCH get_info /
gpio_read / gpio_write / self_destruct answered
with the §3 policy outcomes in script order;
EVT WIFI.DROPPED observed with the next command
still succeeding; WS open/busy/echo/bad-CRC/
close/noconn; a corrupt EVT frame reported as a
link error with recovery after reopen; no unit
dropped at a full queue in normal operation.

## 9. Deferred, explicitly (not v1.2)

- FIDO.DEL / FIDO.APPROVE / FIDO.DENY: a command
  family with no defined session semantics in any
  plan section; inventing them here would be
  scope growth. FIDO.EVENT rides §4 generically.
  Carried to a later phase with a semantics
  section first.
- WS multiplexing, WS subprotocols, cloud WS
  endpoints, NET.HTTP chunked binary bodies
  (C6L-2 note, still host-peer-only for GET).
- AUD integrity at simulated 921600 (C6L-4),
  linkd itself (C6L-5), confirm UX (ONB scope).
