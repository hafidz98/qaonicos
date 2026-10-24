# LINKD-IPC v1 — the linkd client contract

Linkd is the single owner of the C6 transport. Guest programs
reach the link ONLY through linkd over frozen `uipc.h` v1
(see `user/uartproto/linkd.h` for the normative header).
This document is the human-readable contract; the header is
normative on any discrepancy.

## Names and identity

- IPC name: `link`, registered by linkd at startup through
  the kernel-assigned spawn identity (daemon_images index 4).
  Any other claimant gets EPERM (-13).
- Clients look `link` up fresh on every call and cache
  nothing: after a linkd respawn the name resolves to the
  new generation, and the stub recovers transparently.
- The tools consumer (peer-dispatch target) lives on IPC
  name `tools` (qabotd, progid 2, when it registers; the
  C6L-5 test stub `ulinktools` stands in with progid
  override 2). Linkd never executes a peer tool itself.

## Message shape

- Header: frozen `uipc.h` v1, 24 bytes. Body word 0 is the
  op; the reply body word 0 is the status; the reply
  message id echoes the request op (never 71, the Mach
  send-once death-notice id).
- The `len` passed to `uipc_rpc_user` is BOTH the sent size
  and the reply budget: a larger reply is E2BIG and is
  LOST. Clients therefore pad short requests to the
  per-op reply budget with zero bytes; linkd parses
  strings by NUL termination and ignores trailing zeros.

## Ops

| Op | Name | Request body | Reply body |
|----|------|--------------|------------|
| 1 | CMD | `[op, mode, line NUL]` | `[status, payload NUL]` |
| 2 | KV_SET | `[op, mode, vlen, key NUL, value bytes]` | `[status]` |
| 3 | KV_GET | `[op, key NUL]` | `[status, vlen, value bytes]` |
| 4 | KV_DEL | `[op, key NUL]` | `[status]` |
| 5 | TOOL_DISPATCH | `[op, tool NUL, args-json NUL]` | `[status, result-or-code NUL]` |
| 6 | NETHTTP_GET | `[op, url NUL]` | `[status, len, body]` |
| 7 | VOICE_START | `[op, rate, bits]` | `[status]` |
| 8 | VOICE_STOP | `[op]` | `[status, frames]` |
| 9 | AUD_FRAME | `[op, len, pcm bytes]` | `[status]` |
| 10 | EVT_WAIT | `[op, last_seq, budget_ms]` | `[status, seq, event-line NUL]` |
| 11 | STATUS | `[op]` | `[status, 19 words]` |
| 12 | DISPATCH_RESULT | — | — (UNSUP) |
| 13–16 | WS_OPEN/SEND/RECV/CLOSE | — | — (UNSUP) |

Modes (CMD, KV_SET): 0 = plain, 1 = retry-if-policy.
Linkd applies the C6L-4 retry classifier; it never
invents a policy. A refused retry is DENIED, and the
wire is not touched.

## Status codes

| Code | Name | Meaning |
|------|------|---------|
| 0 | OK | Success; payload as per the op table. |
| 1 | PEER_ERR | The peer answered ERR; payload is the wire code (or "" when the caller does not carry it). |
| 2 | LINK_FAIL | Transport failure; payload is the reason string: TIMEOUT, DEAD, CRC, OVERSIZE, MALFORMED, WIRE. |
| 3 | TRUNC | The reply did not fit the caller's budget (see the truncation semantics below). |
| 4 | TIMEOUT | EVT_WAIT with budget 0 and no queued event. |
| 5 | DENIED | A `_retry` request the C6L-4 policy refuses; wire untouched. |
| 6 | UNSUP | WS family, DISPATCH_RESULT: not served through linkd. |
| 7 | BUSY | The EVT waiter table (4) is full. |

## Caps and reply budgets

- Command line 159 chars (mirrors the direct tx-buffer
  truncation); tool/key/url 63 chars; TOOL.DISPATCH args
  192 bytes (wire cap); KV value 64 bytes (store cap);
  AUD payload 1024 bytes (`LINK_MAX_PAYLOAD`); EVT
  payload 256 bytes.
- Reply budgets (padded request sizes): CMD 2048,
  KV_SET 192, KV_GET 512, KV_DEL 128, TOOL 1024,
  NETHTTP 2048, VOICE 128, FRAME 1088, EVT 512,
  STATUS 512. Envelope violations are answered PEER_ERR
  "ARG" without a wire exchange.

## Truncation semantics (mirroring the direct layer)

- `cmd`: excess response bytes are dropped; still OK.
- `kv_get`, `nethttp_get`: over-long value/body returns
  -2 with the full length reported.
- `tool_dispatch`: over-long result returns -2 with the
  wire code copied out; a result that does not fit at
  all returns -1 + "TRUNC".

## STATUS layout

`[status, state, reason, ok, peer_err, timeout, dead,
crc_bad, oversize, malformed, resets, retries,
retry_denied, frames_sent, frames_send_fail,
inb_dropped, evt_dropped, evt_seq, waiters, qlimit]`
(word indices 0–19). `state` is 0 DOWN / 1 UP; `reason`
is the C6L-4 reason string. The shell `link` command
prints this line; `link: unavailable` means linkd is
not running at all.

## EVT fan-out

- Linkd keeps a broadcast ring of the last 8 EVT units
  (sequence numbers from 1, monotonically increasing)
  and up to 4 parked EVT_WAIT waiters.
- EVT_WAIT with `last_seq`: if the ring holds an event
  with seq > last_seq, the OLDEST such event is
  returned immediately (a waiter more than 8 events
  behind gets the oldest held event — a sequence jump
  the client can detect; jumps count in `evt_dropped`).
- Else if `budget_ms` == 0, TIMEOUT is returned
  immediately; else the waiter parks (send-once reply
  name + last_seq) and is answered when the next event
  arrives. Every parked waiter receives every event
  arriving while parked (true fan-out). A waiter
  consumes exactly one event per wait call and must
  re-issue.
- If the client's RPC times out first, its reply port
  is destroyed and linkd drops the waiter on the next
  event (a failed reply is not an error); the event
  stays in the ring for the next poll.
- Waiters survive a transport reopen (application-level,
  like the inbound queue) but NOT linkd's death: the
  client sees EPEER/EDEST and re-issues after respawn.

## Concurrency and queueing

- Linkd is single-threaded and serves one wire exchange
  at a time. Client requests queue in linkd's IPC port
  (default qlimit 5). A sender to a full queue gets
  EFULL immediately (sends never block) and decides its
  own retry/drop policy — for voice frames the honest
  policy is drop + count.
- Each reply goes to the requesting client's own
  send-once reply name, so responses cannot cross
  clients even when a slow command holds the wire
  while others queue behind it.
- Measured at voice rate (C6L-5): one client streams
  32 frames at production pace (~31 ms, 1024 B) while
  a second client issues commands concurrently — peer
  aud_ok 32, STOP 32, client frame failures 0. Qlimit
  stays 5; the 16-deep option (66 KB cost) is NOT
  taken (see DEVLOG-C6L5 §5).

## Failure behaviour

- Transport DOWN propagates as LINK_FAIL + reason,
  never a hang, never a silent drop. The link reopens
  lazily on the next command (C6L-4); DOWN clears on
  the next success.
- A client dying while its request is queued or in
  flight does not break other clients: linkd still
  executes the wire exchange (it cannot un-execute
  it) and its reply to the dead send-once name fails
  harmlessly (`LINKD: reply fail op=… rc=…`, never a
  linkd error).
- If linkd itself dies, FND-3 respawns it (budget
  2/boot). It restarts from scratch: re-registers
  `link` (generation bumps per IPC-4), re-selects the
  bridge backend, empty EVT ring, no waiters. Clients
  see EDEST/EPEER transiently and recover on their
  next call (fresh lookup per call, nothing cached).

## Watchdog

Linkd parks in blocking IPC receive with a 100 ms
deadline, drains inbound units between receives (at
most 8 per turn, so IPC service cannot starve behind
a flooding peer), and yields every turn. Transport
reopen attempts are throttled to at most one per
2000 ms while the link is DOWN. Wire execution yields
naturally (bridge read yields on empty, bridge write
yields on window-full), so `net_pump` runs and the
watchdog stays fed. Linkd must park or yield — never
spin 30 s without a switch (FND-3 class-(b)).

## The client stub

`user/uartproto/linkcli.c` implements the full
`uartproto.h` surface over IPC, with signatures
identical to the direct layer, so consumers migrate
by changing only the link line (drop
`uartproto_uartproto.o` + `uartproto_mock_comcu.o`,
add `linkcli.o`). One deliberate omission:
`uproto_retry_policy` is NOT provided by the stub —
the classifier lives in linkd via the real `_retry`
wrappers, and a second copy in the stub would
diverge. Consumers needing the classifier link the
direct layer or ask linkd.

## Non-goals (still direct-link only)

- WS sessions: per-client WS state + BIN draining
  would need a second loop shape. WS stays
  direct-link only until a later phase designs it;
  the stub `ws_*` return -1 and linkd answers WS ops
  UNSUP.
- Policy decisions: linkd never executes, confirms,
  or refuses a peer tool. The C6L-3 policy mirror
  lives in the tools consumer.
- Protocol changes: none. Topology only; the wire
  stays v1.1 + v1.2.
