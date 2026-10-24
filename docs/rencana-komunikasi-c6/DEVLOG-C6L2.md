# DEVLOG-C6L2 — Host-bridge backend + Python mock C6 peer

Phase C6L-2 of the C6 link track (`docs/rencana-komunikasi-c6/PLAN.md`).
Base: C6L-1 merge. No kernel change: the bridge is a guest
backend plus a host peer plus a harness, all in userspace
and scripts.

## 1. What was built

- `user/uartproto/bridge.c` — the `bridge` backend declared
  (but unimplemented) in `link.h` since C6L-1. Guest-side
  TCP client to the host peer at the slirp gateway
  `10.0.2.2:18082`, speaking the frozen wire v1.1 byte
  for byte through the link layer's existing backend
  interface (`open` / `write` / `read` / `send_frame` /
  `close`). The link layer keeps owning timeouts (2 s
  per command) and framing; the backend only moves bytes.
- `scripts/mock-c6.py` — host-side Python mock C6 peer
  (stdlib only). Implements the full frozen wire v1.1
  command set the in-process mock implements, plus the
  C6L-2 NET.HTTP addition (see §5). One client at a
  time; `--port` (default 18082), `--transcript`,
  `--stats-file`, `--delay-ms`, `--drop-after N`,
  `--kill-after N`, `--scan-list`.
- `user/ulinkbr.c` — guest test program, spawn-only via
  the `embed:ulinkbr` hook in `SYS_SPAWN` (see §3 for why
  it must NOT be a boot one-shot). Runs the v1.1 command
  set, AUD frames, a NET.HTTP hash check, and a
  peer-death + reconnect sequence against the host peer.
  Markers `ULINKBR: <check> ok`, final `ULINKBR: PASS`
  (or FAIL); the shell reports `exit <fails>`.
- `scripts/verify-c6l2.py` — harness: starts the peer
  itself, boots QEMU, sends `run embed:ulinkbr` at the
  `qaon>` prompt, kills + restarts the peer when the
  guest announces `ULINKBR: PHASE-A-DONE`, and asserts
  guest markers, peer transcript bytes, and peer stats.
  The peer is never left running at the end.
- `uproto_nethttp_get()` in `uartproto.c`/`uartproto.h` —
  the one new link-layer client primitive C6L-2 needs:
  it writes `NET.HTTP GET <url>` and reads an
  `OK <len>` + exactly `<len>` body bytes, reusing the
  `KV.GET` body-reader shape (truncate into the
  caller's buffer but always consume the full body;
  0 full, -2 truncated with `*len_out` = peer length,
  -1 error).

## 2. Backend wire mapping

The bridge maps the backend contract onto the guest's
single TCP client connection (syscalls 73–77):

| Backend op | Bridge behaviour |
|---|---|
| `open()` | Idempotent (already ESTABLISHED → 0). Otherwise waits up to 1 s for a lingering half-open state to settle to CLOSED, then up to 3 connect attempts at ~1 s each (bounded backoff, never forever). |
| `write()` / `send_frame()` | Refuses when not ESTABLISHED (-1, no silent auto-reconnect inside a write — recovery is the caller's close + reselect, per the frozen rules). Sends in ≤1200-byte chunks (kernel stop-and-wait window), looping with yields on 0 (unacked), 2 s budget. Frames are ≤1031 bytes, normally one chunk. |
| `read()` | Returns received bytes, 0 when empty (after a yield, so the network pump runs), -1 when the TCP state is CLOSED (peer FIN/RST processed by the pump = link dead). |
| `close()` | Sends FIN if ESTABLISHED, clears state. |

Error mapping, as required: unreachable peer → `open()`
fails after bounded retries (guest prints
`ULINKBR: SKIP (no peer)` and exits 0 when run without
a peer, so ordinary shell use stays clean); refused
peer → RST settles the state to CLOSED fast, same
path; peer dying mid-command → the next `write()` or
`read()` fails (state CLOSED, or the link layer's 2 s
timeout if the FIN is still in flight) and the client
sees a plain command error — never a hang, never a
kernel fault. Recovery is explicit: close + reselect +
retry, proven by the reconnect phase (§6).

## 3. Why ulinkbr is spawn-only (the C6L-2 finding)

The first build ran `ulinkbr` as a boot one-shot from
`uprogs[]` and printed `ULINKBR: SKIP (no peer)` even
with the peer running, while the peer log showed the
guest's connection accepted but zero request bytes.
Root cause, found in `mach3-port/src/ext/user.c`:

- Boot one-shots run **sequentially to completion in
  the boot thread** (`launch_uprog` → `user_enter_test`),
  **before the scheduler activates** (`sched_active`
  is set at line ~2657, after the one-shot loop).
- `SYS_YIELD` in that phase is an explicit nop:
  `if (sched_active) sched_yield_switch(frame); return 0;`
  with the comment "Fase one-shot (belum aktif): nop."
- `net_pump()` runs only at the scheduler switch
  points (`sched_yield_switch`, `sched_ipc_wait_switch`,
  `sched_exit_switch`). So during a boot one-shot the
  network is **never pumped**: the guest's SYN goes out
  (the peer accepts the slirp leg), the SYN-ACK arrives
  but is never processed, the state stays SYN_SENT,
  and `open()` times out.

The fix follows the house pattern used by the FND-3
and IPC probes (`embed:uyield`, `embed:uipcaud`):
`ulinkbr` lives in the `SYS_SPAWN` `hooks[]` table
(progid -1, like the other test probes) and runs via
`run embed:ulinkbr` from the shell, where the child
runs under the active scheduler and every yield pumps
the network. It was **removed from `uprogs[]`** so the
boot run cannot wedge the single TCP connection in
SYN_SENT (which would have poisoned the later shell
run until the 120 s connection timeout). This is also
why the C6L-2 verify script is shell-driven, unlike
the boot-driven C6L-1 script.

Consequence for later phases: **any guest program
that needs the network must run as a spawned daemon
or from the shell, never as a boot one-shot.** C6L-5's
linkd is a daemon by design, so this constraint is
already satisfied there; C6L-3/C6L-4 tests must use
the same spawn-only shape.

## 4. Peer semantics, incl. reconnect/state behaviour

The peer mirrors `user/uartproto/mock_comcu.c`
semantics exactly, including its quirks (verified
against the in-process mock's source, not just the
freeze doc):

- Scan list canned: `RUMAH-HAFIDZ/-52/WPA2`,
  `KANTOR-5G/-67/WPA2`, `WARUNGKOPI/-78/OPEN`.
- CONNECT: known SSID → OPEN = CONNECTED, secured =
  NEEDPASS; unknown → `ERR NOTFOUND`. PASS: no pending
  → `ERR NOCONN`; length ≥ 8 → CONNECTED + save;
  short → `ERR AUTH`. Saved-network cap is **2**
  (duplicates not re-saved). LIST prints `SSID,WPA2`
  (security always WPA2 in list output). FORGET
  idempotent. STATUS/DISC as frozen (DISC clears
  pending auth).
- BLE: ON/OFF + NAME (stored truncated to **23**
  chars, as the mock's `ble_name[24]`); STATUS returns
  `ON NONE` / `OFF NONE` (never paired) — the second
  token is the paired peer, never the advertise name.
- KV: 8 slots, key ≤ 31, value ≤ 64 by the wire, but
  the store keeps **63 chars + NUL** — a 64-byte SET is
  accepted and GET then returns 63 bytes (mirrored
  mock quirk, not fixed: a client must not be able to
  tell the peers apart). Over-slot SET → `ERR FULL`;
  over-64 value → `ERR ARG`, body consumed, never
  stored truncated. Empty key → ARG. Non-numeric
  `KV.SET` third token (legacy inline shape) →
  `ERR ARG` immediately, no body wait; numeric length
  > 4096 → `ERR ARG`.
- TIME.GET: real host wall time, fixed +07:00 offset,
  always "synced" (host clock) — unlike the in-process
  mock, which answers `MOCK-…` before NTP sync.
- FIDO.LIST → `OK 0` stub. VOICE.START exact string
  only → OK; VOICE.STOP returns the frame count
  **without resetting it** (START resets). AUD frames
  counted only when a session is active, type 0x01,
  length matches, CRC-16-CCITT (poly 0x1021, init
  0xFFFF) matches; anything else is rejected silently
  (no response ever for frames) and never counted.
  Legacy 3-byte `AUD` magic frames are consumed and
  rejected on the stream so they cannot desynchronise
  it (a text-line parser would otherwise swallow them
  as an unterminated line).
- Unknown command → `ERR UNKNOWN`.

Deliberate differences from the in-process mock
(documented, not bugs):

- **State lives in the peer process's memory.** A TCP
  reconnect keeps state (same process); a peer
  **restart resets it** (fresh NVS — the C6-reboot
  analogue). The in-process mock instead persists NVS
  to the guest SD card across guest reboots. The
  verify script's post-reconnect checks therefore use
  fresh keys only, never pre-kill keys.
- **TIME.GET is always real time** (see above).
- The verify script's port-liveness probe opens one
  extra connection per peer process (logged as a
  session with no requests); session counts in the
  transcript are therefore ≥ the guest's sessions.

Transcript format (append, line-flushed): `SESSION <n>
ACCEPT`, `REQ <repr of raw request bytes>` (line +
body, or frame), `RESP <repr>`, `FRAME AUD <len>
ok|bad-crc|bad-type|outside-session`, `SESSION <n>
CLOSE <reason>`. Stats JSON (rewritten after every
completed request, via tmp+rename): `sessions`,
`commands`, `aud_ok`, `aud_bad`, `aud_outside`,
`nethttp_bytes`, `kv_sets`.

## 5. NET.HTTP scope note (plan-required, beyond the brief)

The task brief's item 2 enumerates only the v1.1 set
the in-process mock implements, but PLAN.md's C6L-2
verification explicitly requires "NET.HTTP body
integrity checked by hash", and WIRE-FREEZE §6 lists
NET.HTTP chunked binary bodies as a C6L-2 item. So
NET.HTTP is implemented in this phase, kept clearly
separated from the frozen set: the peer answers only
`NET.HTTP GET <url>` with `OK 1024` + a deterministic
1024-byte body (`body[i] = (i*7+13) & 0xFF`); any other
NET.HTTP shape → `ERR ARG`. The guest helper
(`uproto_nethttp_get`, §1) reads the length-prefixed
body and the test compares its CRC-16 against the
locally generated pattern. Chunked/binary bodies
beyond this fixed shape, TOOL.DISPATCH, EVT, and
NET.WS remain C6L-3 work; they are not built here.

## 6. Verification

### C6L2-VERIFY (new)

`scripts/verify-c6l2.py`: **PASS 36/36** (post-change
kernel, peer started by the script itself).

Guest markers asserted (all present): wifi scan,
connect needpass, pass connected, open connected,
err notfound, err auth, err noconn, list forget,
status disc; ble status spec, ble on off name; kv
len-prefixed, kv errors, kv trunc (-2, `vlen_out`=16,
value `0123456` in an 8-byte buffer), kv legacy
rejected + TIME.GET still ok; aud frames (8×512 B
good, STOP = 8); nethttp hash (1024 bytes, CRC
matches the local pattern); peer death detected
(TIME.GET fails after the kill); peer reconnect
(reselect + retry succeeds, fresh key round-trips);
plus `ULINK: PASS` + `UVOICE: PASS` mock regressions
from the boot run, `exit 0` from the shell, no FAULT,
no SKIP, no FAIL line.

Peer-side evidence asserted: transcript holds the
exact wire bytes `b'KV.SET ulinkbr_wire 3\nxyz'`;
≥ 2 ACCEPT sessions (probe + guest, × 2 peer
processes across the restart); `FRAME AUD 512 ok`
lines; pre-restart stats `aud_ok == 8`,
`aud_bad == 2` (1 legacy + 1 bad-CRC), `nethttp_bytes
== 1024`. No `mock-c6.py` process remains at the end
(the check allows a 1 s grace retry for a peer still
shutting down; the shell-exit check matches
`exit 0\n` exactly, not the boot logs' `exit 0x…`
strings).

### Pre-change FAIL (test-first evidence)

- Base-kernel probe (C6L-1 kernel, peer running):
  boot reaches `qaon>`, `ULINK: PASS` present, **zero
  `ULINKBR` lines** — the bridge program does not
  exist yet.
- New script against the base kernel (stash +
  rebuild, script + peer copied aside): the shell
  answers `run embed:ulinkbr` with `spawn gagal`
  (no such hook) and the script FAILs —
  **C6L2-VERIFY: FAIL (8/36)**, the 8 passes being
  vacuous absence checks (no FAIL line, no SKIP, no
  FAULT, probe-only transcript sessions). The guest
  never connects: the base run's peer transcript
  holds probe sessions only, no requests.

### Regression battery (standalone, post-change)

All scripts run standalone, in order, against the
C6L-2 kernel — no failures, no standalone reruns
needed (no order-state flakes this run):

| Script | Result |
|---|---|
| `verify-c6l1.py` | C6L1-VERIFY: PASS (29/29) |
| `verify-fnd1.py` | FND1-VERIFY: PASS |
| `verify-fnd2.py` | FND2-VERIFY: PASS (21/21) |
| `verify-fnd3.py` | FND3-VERIFY: PASS (46/46) |
| `verify-fnd4.py` | FND4-VERIFY: PASS (36/36) |
| `verify-fnd5.py` | FND5-VERIFY: PASS (33/33) |
| `verify-ipc2.py` | IPC2-VERIFY: PASS (34/34) |
| `verify-ipc4.py` | IPC4-VERIFY: PASS (58/58) |
| `verify-ipc5.py` | IPC5-VERIFY: PASS (20/20) |
| `verify-onb1.py` | ONB1-VERIFY: PASS |
| `verify-onb2.py` | ONB2-VERIFY: PASS |
| `verify-c6l2.py` | C6L2-VERIFY: PASS (36/36) |

Size impact: kernel binary (`objcopy -O binary`)
3,349,700 bytes; **headroom 844,604 B** (4,194,304-byte
partition), down 39,160 B from C6L-1's 883,764 B —
`ulinkbr` itself is 36,717 B plus the bridge object.
The standing UI-1 follow-up (whether boot-only test
programs stay embedded in production images) now
covers two programs, ~72 KB combined.

## 7. TCP single-connection constraint, and how the harness respects it

The kernel enforces one TCP client connection
(`tcp_client_connect` fails unless CLOSED; `daemon_
reclaim` closes a dead daemon's connection). The
bridge backend owns that connection for the whole
session, from `open()` to `close()` (D-C6L-4
arbitration note): qabotd's provider must not connect
while a bridge session is open. In the current product
flow this holds because link sessions run in shell
spawn children before GUI/qabotd activity, and strict
request/response alternation means no second speaker
can interleave on the wire. C6L-5's linkd will
arbitrate once consumers share the transport.

The harness respects the constraint three ways: (1)
the guest connects **outbound** to `10.0.2.2:18082`
via slirp — no `hostfwd` is needed for this direction
(proven by `utcpcli` → `10.0.2.2:18081` in earlier
work); the QEMU line keeps the existing
`hostfwd=tcp::18080-:80` for the guest web server
only, unchanged. (2) The guest runs one bridge
session at a time (single spawned child). (3) The
peer serves one client at a time and returns to
`accept()` on disconnect.

## 8. Honest limitations

- The bridge has no auto-reconnect inside a command:
  a dead peer fails the in-flight command and the
  caller must reselect + retry (by design, per the
  frozen rules — but it means a C6L-5 linkd client
  will need an explicit retry policy; C6L-4 defines
  it: never retry WIFI.PASS, never retry KV.SET with
  a changed value).
- `open()`'s settle window (1 s) assumes a lingering
  SYN_SENT/FIN_SENT settles via RST/ACK promptly; a
  truly black-holed peer leaves the state wedged until
  the kernel's 120 s connection timeout, during which
  a new `open()` fails. The harness never hits this
  (slirp RSTs closed ports immediately).
- The peer is a test double, not a C6: it speaks TCP,
  not UART; baud, pins, RTS/CTS, and BLE coexistence
  are hardware-phase work (FW-1..3, still parked).
  AUD integrity at simulated 921600 baud is C6L-4.
- The guest's `ulinkbr` binary (36,717 B) plus the
  bridge object live in the boot partition image
  alongside the other test programs; whether boot-only
  test programs stay embedded in production images is
  the standing UI-1 follow-up from C6L-1.
- The in-process mock backend is untouched and its
  C6L-1 suite still passes unchanged (ULINK: PASS in
  the C6L-2 run's boot section).
