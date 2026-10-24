# C6 link: wire freeze v1.1 (C6L-1)

Canonical frozen-wire reference for the QaonicOS <-> ESP32-C6 link.
Every later phase (C6L-2 bridge, C6L-3 v2 completion, C6L-5 linkd,
FW-1 firmware) cites this document. It supersedes the KV and BLE
passages of `docs/rencana-app-qaonicos/RENCANA.md` §4 and the frame
sketch of `docs/rencana-komunikasi-c6/PLAN.md` F5 §5; those documents
are left untouched as historical records.

Status: frozen at C6L-1. Changes after this point need a new
versioned amendment, not a silent edit.

## 0. Transport model

The link layer (`user/uartproto/uartproto.c`) never knows which
transport carries the bytes. It talks to a backend
(`user/uartproto/link.h`, `struct uproto_backend`):

```c
struct uproto_backend {
	const char *name;
	int (*open)(void);
	int (*write)(const unsigned char *buf, unsigned len);
	int (*read)(unsigned char *buf, unsigned len);
	int (*send_frame)(const unsigned char *frame, unsigned flen);
	int (*close)(void);
};
```

Backends: `mock` (in-process, `user/uartproto/mock_comcu.c`,
backend #1, the QEMU test peer), `bridge` (C6L-2, guest TCP to
the host peer), `uart` (hardware only, FW track). Selection:
`uproto_backend_select(be)` (NULL = mock); the previous backend
is closed and the new one opened. `uproto_backend_current()`
never returns NULL.

Rules that hold on every backend:

- **Strict request/response alternation.** The guest sends one
  request and waits for its response before sending the next.
  No pipelining. Unsolicited bytes from the peer, or a malformed
  length in a response, are a **link-reset event**: the caller
  closes and reopens the backend (the in-process mock resets
  its parser on `close`/`open`; the bridge reconnects, C6L-2).
  There is no in-band resynchronisation: after a reset the
  next request starts a clean exchange.
- **Per-command timeout: 2000 ms** (`UPROTO_CMD_TIMEOUT_MS`).
  A peer that answers nothing within this window makes the
  call fail with a timeout; the caller decides recovery
  (reopen / retry). Retries, if any, are the caller's policy
  (C6L-4 fixes which commands may be retried: never WIFI.PASS,
  never KV.SET with a changed value).
- **Text wire is ASCII, lines end with `\n`.** The guest sends
  one line per command (KV.SET sends line + body, §2).
  Responses are `OK ...` or `ERR <CODE>`. Error codes in use:
  `ARG`, `NOTFOUND`, `AUTH`, `NOCONN`, `FULL`, `UNKNOWN`.
  An unknown command is `ERR UNKNOWN`.
- **No partial credit.** A response the reader cannot parse
  (bad count, bad length, bad CRC) is a failure of the whole
  call, not a truncated success — except the KV client rule
  in §2 (a value longer than the caller's buffer is delivered
  truncated with a distinct return, because the peer's full
  length is known and the stream stays in sync).

## 1. Frozen v1.1 command table

`->` is the guest request, `<-` the peer response. `OK` with
no payload means the bare line `OK`.

### Wi-Fi

| Request | Response | Notes |
|---|---|---|
| `WIFI.SCAN` | `OK <n>` then exactly `n` lines `SSID,RSSI,SECURITY` | Count-prefixed multi-line (§3). SECURITY is `OPEN`, `WPA2`, or `WPA3`. |
| `WIFI.CONNECT <ssid>` | `OK CONNECTED` (open network) or `OK NEEDPASS` (secured) | NEEDPASS arms a pending auth; the next WIFI.PASS completes it. |
| `WIFI.PASS <password>` | `OK CONNECTED`, or `ERR AUTH` (short/wrong), or `ERR NOCONN` (no pending auth) | Minimum password length 8. A successful PASS saves the network (see LIST). |
| `WIFI.LIST` | `OK <n>` then exactly `n` lines `SSID,RSSI,SECURITY` | Saved networks only. |
| `WIFI.FORGET <ssid>` | `OK` | Idempotent: forgetting an unknown SSID is still `OK`. |
| `WIFI.STATUS` | `OK CONNECTED <ssid>` or `OK DISCONNECTED` | Mock extension, kept for the screens. |
| `WIFI.DISC` | `OK` | Disconnects and clears any pending auth. Mock extension, kept for the screens. |

### Bluetooth

| Request | Response | Notes |
|---|---|---|
| `BLE.ON` | `OK` | |
| `BLE.OFF` | `OK` | |
| `BLE.NAME <name>` | `OK` | Sets the advertise name (max 31 chars). |
| `BLE.STATUS` | `OK ON <paired>` or `OK OFF <paired>` | `<paired>` is the paired peer address, or `NONE` when nothing is paired. **This is the frozen shape: the second token is NEVER the advertise name** (see §5, drift 2). |

### Key-value store (link-level NVS)

| Request | Response | Notes |
|---|---|---|
| `KV.SET <key> <len>\n<body>` | `OK` | §2. Length-prefixed, not inline. |
| `KV.GET <key>` | `OK <len>\n<body>` | §2. `ERR NOTFOUND` when absent. |
| `KV.DEL <key>` | `OK` | Idempotent. |

Store limits (this implementation): key max 31 chars, value
max 64 bytes (`UPROTO_KV_VAL_MAX`, equal to the NVS value
size), 8 slots (`NVS_KV_MAX`). A SET that would exceed the
slot count is `ERR FULL`; a value over 64 bytes is
`ERR ARG` and the body is consumed, not stored truncated.
The limits are a property of the store, not of the framing:
the framing itself carries arbitrary lengths.

### Time, FIDO

| Request | Response | Notes |
|---|---|---|
| `TIME.GET` | `OK <iso8601>` | Wall clock, `+07:00` offset. Before any sync the mock answers a clearly-labelled `MOCK-...` timestamp; the guest treats it as unsynced. |
| `FIDO.LIST` | `OK 0` | Stub. FIDO.DEL/APPROVE/DENY and FIDO.EVENT arrive in C6L-3. |

## 2. KV framing, byte-exact

SET (guest -> peer):

```
KV.SET <key> <len>\n<body>
```

- `<key>`: no spaces. `<len>`: decimal, the exact byte count
  of `<body>`. `<body>` is exactly `<len>` raw bytes, with no
  trailing newline and no escaping — values may contain
  spaces and any byte except the framing's own length limit.
- The peer reads exactly `<len>` bytes after the newline,
  then answers `OK` or `ERR <CODE>`.

GET (peer -> guest):

```
OK <len>\n<body>
```

- `<len>`: decimal, the exact byte count of `<body>`.
  `<body>` is exactly `<len>` raw bytes, with no trailing
  newline. `ERR NOTFOUND` replaces the whole response when
  the key is absent.

Client mapping (`uproto_kv_set/get/del`):

- `uproto_kv_set(key, val, vlen)`: returns 0 on `OK`, -1 on
  any error. A client-side guard rejects `vlen > 64` with -1
  **without sending anything** (the store could not take it).
- `uproto_kv_get(key, val, vsz, &vlen_out)`: returns 0 when
  the full value fits; **-2 when the value was truncated
  into `val`** (still NUL-terminated, `*vlen_out` = the
  peer's full length); -1 on error or NOTFOUND. Over-long
  bodies are always consumed from the stream, never left
  behind, so the next exchange starts clean.
- `uproto_kv_del(key)`: 0 on `OK`, -1 otherwise.

Why length-prefixed: the old inline form (`KV.SET <k> <v>`)
could not carry spaces, could not distinguish an empty value
from a missing one, and capped values at the TX buffer. The
length prefix fixes all three and makes the body opaque.

## 3. Multi-line responses

Only `WIFI.SCAN` and `WIFI.LIST` are multi-line. Their first
line is `OK <n>` and the reader consumes exactly `n` further
lines, no more. All other commands are single-line responses
(`OK ...` or `ERR ...`), except `KV.GET` (§2). A count that
does not parse, or a stream that ends early, fails the call.

## 4. v2 frame format (audio slice, frozen)

Binary frames are **not** text lines. Format (5-byte header):

```
magic(2)  type(1)  len(2, big-endian)  payload(len)  CRC(2, big-endian)
```

- `magic`: ASCII `Q`, `A` (`LINK_FRAME_MAGIC0/1`).
- `type`: `0x01` AUD, `0x02` BIN, `0x03` EVT
  (`LINK_FRAME_TYPE_AUD/BIN/EVT`). Only AUD is live in v1.1;
  BIN and EVT are reserved type codes, not live commands.
- `len`: payload length, big-endian, max 1024
  (`LINK_MAX_PAYLOAD`).
- `CRC`: CRC-16-CCITT, polynomial `0x1021`, init `0xFFFF`,
  computed over header + payload, transmitted big-endian
  (`voice_crc16()`).

AUD payload: PCM16 little-endian audio, unchanged from the
v1 audio slice. Session control stays textual:
`VOICE.START <rate> <bits>` (the mock accepts exactly
`VOICE.START 16000 16`) and `VOICE.STOP`, which answers the
frame count accepted by the peer.

Rejection rules (enforced by the mock, required of every
peer): a frame with the legacy 3-byte `AUD` magic is
rejected; a frame with a bad CRC is rejected; a frame
arriving outside a voice session is rejected. Rejected
frames are not counted and do not disturb the stream.

## 5. Drifts resolved by this freeze

Two places where the shipped code disagreed with the spec
text. The spec text is canonical; the code and the mock were
fixed to it (C6L-1, see DEVLOG-C6L1).

1. **KV inline vs length-prefixed.** Code sent
   `KV.SET <k> <v>` (value as a space-separated token, no
   spaces allowed, TX-buffer-capped); the spec said
   `KV.SET <k> <len>` + body. Resolution: length-prefixed
   (§2). The old inline shape is now a protocol error:
   a `KV.SET` line whose third token is not a number is
   answered `ERR ARG` immediately, with no body wait, so
   the stream never desynchronises. A `KV.SET` line with a
   numeric length over 4096 is dropped by the mock's
   accumulator guard (the peer never waits on an unbounded
   body).
2. **BLE.STATUS payload.** Code returned
   `OK ON|OFF <advertise-name>`; the spec said
   `OK ON|OFF <paired_addr|NONE>`. Resolution: the spec
   shape (§1). The mock never pairs, so it always answers
   `NONE`. Client impact: the BLE screen's second token is
   now the paired peer, not the name (renamed `ble_peer`
   in `scr_ble.c`); the `NAMA: QABOT-01` row on that screen
   is a label only — the screen never sends `BLE.NAME`.

A third, smaller drift was fixed in the same pass: the AUD
frame shipped with a 3-byte `AUD` magic and no type byte.
Per the plan's D-C6L-3 (ii) ordering it into C6L-1, the
frame was generalised to the §4 format. Rates, PCM16 LE,
the 1024-byte cap, and the CRC algorithm are unchanged;
only the header changed, and legacy frames are rejected.

## 6. What is NOT frozen here

- TOOL.DISPATCH / EVT / NET.WS wire details (C6L-3).
  The mock dispatch shape from the IPC track (id
  `0x3000+s` echo XOR `0xA5`, `0x3100` structured dispatch)
  is the reference for the dispatch *semantics*, not a wire
  format — the v2 command wire is defined in C6L-3.
- NET.HTTP chunked binary bodies (C6L-2, with the bridge).
- Baud rate, UART pins, flow control (hardware; the UART4
  driver is FW-track work, not a wire property).
- The bridge backend's TCP framing and reconnect backoff
  (C6L-2). The timeout (§0) and the reset rule (§0) are
  inherited by the bridge unchanged.
- The linkd daemon protocol (C6L-5). Linkd speaks the
  frozen wire to the C6 and the frozen IPC v1 contract
  (`uipc.h`) to its clients; neither side changes here.

## 7. Conformance checklist (for a new peer)

A peer conforms to wire v1.1 if it passes
`scripts/verify-c6l1.py` against the same client code:
every command in §1 round-trips; KV values with spaces and
an empty value round-trip; the old inline KV shape and the
legacy `AUD` magic are rejected; a numeric-length KV.SET
with no body times out in 2 s and the next request after a
reopen succeeds; the wire bytes of a `KV.SET` are exactly
`KV.SET <key> <len>\n<body>` (asserted byte-for-byte by
the stub-backend probe in `user/ulinktest.c`).
