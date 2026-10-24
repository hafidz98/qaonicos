# DEVLOG-C6L1 — link layer + wire freeze

Phase: C6L-1 (first phase of the C6 link track).
Branch: `dev/c6l1` (worktree `~/workspace/qaonic_os-c6l1`).
Base: IPC-5 merge (`13f2698`).
Status: DONE, verified. No merge, no push (coordinator's job).

## 1. What was built

The existing v1 client library (`user/uartproto/uartproto.{c,h}`
+ `mock_comcu.c`) was formalised into a transport-agnostic
link layer. The change is deliberately boring: the same
commands, the same mock peer, the same call sites — plus a
seam where the transport can be swapped, plus the two wire
drifts fixed, plus a test that proves all of it.

### 1.1 The seam: `user/uartproto/link.h`

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

- `uproto_backend_select(be)` — NULL selects the mock.
  The previous backend is closed, the new one opened.
  `uproto_backend_current()` never returns NULL.
- The protocol code (`uproto_cmd/cmd1/cmd2`, `uproto_kv_*`,
  voice start/stop, AUD framing) calls only the backend
  interface. It never knows which transport is live.
- Backends: `mock` (in-process, this phase), `bridge`
  (C6L-2, guest TCP to the host peer), `uart` (hardware
  only, FW track). No UART syscall or driver was invented
  here — that is hardware-only work.
- The seam sits in userspace, in the client library — not
  in the kernel, not in IPC. C6L-5's linkd will be the
  first IPC consumer of this layer (via a client stub with
  the same `uproto_*` signatures); it does not change the
  wire.

### 1.2 The link layer: `user/uartproto/uartproto.c`

- **Backend selection with lazy open.** The first call on a
  fresh backend opens it; `select` closes the old backend
  and opens the new one. The mock's `open`/`close` reset
  only its parser and response queue — NVS, Wi-Fi runtime
  state, and BLE state survive a reopen. That is what makes
  "close + reopen" a valid resynchronisation primitive.
- **Streaming byte reader.** Responses are read through a
  32-byte staging buffer, one byte at a time from the
  caller's view, with an elapsed-time timeout
  (`UPROTO_CMD_TIMEOUT_MS` = 2000 ms, via `sys_uptime()`
  which returns milliseconds). A peer that answers nothing
  makes the call fail after 2 s instead of hanging — the
  malformed-KV probe proves this end to end.
- **Text-response reader.** Single-line responses by
  default; count-prefixed multi-line responses only for
  `WIFI.SCAN` and `WIFI.LIST` (the reader consumes exactly
  the announced number of lines). Everything else is a
  single line, except `KV.GET` (§1.3).
- **KV framing.** `uproto_kv_set(key, val, vlen)` sends
  `KV.SET <key> <len>\n` + exactly `vlen` bytes, with a
  client-side guard: `vlen > 64` fails with -1 without
  sending anything. `uproto_kv_get(key, val, vsz, &vlen)`
  reads `OK <len>\n` + exactly `len` bytes and returns 0
  (full), **-2** (truncated into `val`, still
  NUL-terminated, `*vlen_out` = the peer's full length),
  or -1 (error / NOTFOUND). Over-long bodies are always
  consumed from the stream, never left behind.
- **AUD framing.** `uproto_aud_frame()` builds the frozen
  v2 frame (§3) and sends it via `backend->send_frame`.
  `voice_crc16()` (CRC-16-CCITT, poly `0x1021`, init
  `0xFFFF`) is public so tests and future backends build
  valid frames the same way.
- **No kernel change.** No new syscall, no ABI change, no
  daemon table change. The whole phase is userspace +
  build wiring.

### 1.3 The mock peer: `user/uartproto/mock_comcu.c`

Rewritten as a wire-level backend: it parses the byte
stream (line phase + `KV.SET` body phase) instead of
receiving pre-parsed calls, so the frozen wire is what
actually gets exercised.

- Strict `KV.SET <key> <len>` parsing: a non-numeric
  third token (the legacy inline shape) is answered
  `ERR ARG` immediately, with no body wait. A numeric
  length over 4096 drops the accumulator (the peer never
  waits on an unbounded body). A body over the 64-byte
  store cap is consumed, then rejected `ERR ARG` — never
  stored truncated.
- Response byte queue drained at most 32 bytes per
  `read()`, so the client's streaming reader is genuinely
  exercised even against the in-process peer.
- `BLE.STATUS` returns the spec shape `OK ON|OFF NONE`
  (the mock never pairs). `KV.GET` returns `OK <len>\n` +
  exactly `len` bytes, no trailing newline.
  `VOICE.START 16000 16` exact-string acceptance kept.
- AUD validation: `QA` magic + type `0x01` + length +
  CRC; legacy 3-byte `AUD` magic rejected, bad CRC
  rejected, frames outside a voice session rejected.
- `WIFI.DISC` now also clears a pending auth (a
  disconnect aborts a pending NEEDPASS). Found while
  writing the test's Wi-Fi sequence; the old code left
  the pending SSID armed across a disconnect.
- Accumulator cap 256 bytes (largest single write from
  the link layer is ~160); overflow makes `write` return
  -1 and the caller must reopen. Response queue 256 bytes
  (largest mock response is ~85 bytes).

## 2. Drift resolutions

The plan names two drifts; the spec text is canonical in
both cases (D-C6L-3). A third, smaller drift was fixed in
the same pass because the plan orders it into C6L-1.

1. **KV inline vs length-prefixed.** Code sent
   `KV.SET <k> <v>`; spec said `KV.SET <k> <len>` + body.
   Fixed to length-prefixed (§1.2, §1.3). Values may now
   contain spaces; empty values are distinguishable from
   missing keys; the framing carries arbitrary lengths
   (the 64-byte cap is a store property, not a framing
   property). The old shape is a protocol error, not a
   fallback.
2. **BLE.STATUS payload.** Code returned the advertise
   name; spec said the paired peer address or `NONE`.
   Fixed to the spec shape. Client impact is one file:
   `user/uiapp/scr_ble.c` — the file-local `ble_name`
   became `ble_peer`, and the screen's second token now
   honestly shows the peer (`NONE` under QEMU). The
   `NAMA: QABOT-01` row is a label only; the screen never
   sends `BLE.NAME`.
3. **AUD frame header.** Shipped `AUD` 3-byte magic, no
   type byte. Generalised per D-C6L-3 (ii) to
   `QA | type | len BE | payload | CRC BE` (§3). Rates,
   PCM16 LE, the 1024-byte cap, and the CRC algorithm are
   unchanged — only the header changed, and legacy frames
   are rejected. This is a deliberate judgment call,
   recorded here: the task brief's "audio slice untouched
   in shape" is read as rates / encoding / cap / CRC
   unchanged, while the header migration is exactly what
   the plan orders into C6L-1.

## 3. The frozen wire (summary)

Full reference: `docs/rencana-komunikasi-c6/WIRE-FREEZE.md`
(the single canonical document later phases cite). It
supersedes the KV/BLE passages of
`docs/rencana-app-qaonicos/RENCANA.md` §4 and the frame
sketch of `docs/rencana-komunikasi-c6/PLAN.md` F5 §5;
those documents are untouched as historical records.

- Text wire: `WIFI.SCAN/CONNECT/PASS/LIST/FORGET/STATUS/
  DISC`, `BLE.ON/OFF/NAME/STATUS`, `KV.SET/GET/DEL`,
  `TIME.GET`, `FIDO.LIST` stub. Errors: `ARG`, `NOTFOUND`,
  `AUTH`, `NOCONN`, `FULL`, `UNKNOWN`.
- Strict request/response alternation, no pipelining.
  Unsolicited bytes / malformed lengths = link-reset
  event: close + reopen the backend.
- Per-command timeout 2000 ms, inherited by the bridge.
- KV: `KV.SET <key> <len>\n<body>` /
  `OK <len>\n<body>`, byte-exact, no trailing newline
  on the body. Multi-line rule: only SCAN/LIST, exactly
  the announced line count.
- v2 frames: `QA | type(0x01 AUD, 0x02 BIN, 0x03 EVT) |
  len BE | payload (<=1024) | CRC-16-CCITT BE`.
- NOT frozen here: TOOL.DISPATCH/EVT/NET.WS wire (C6L-3),
  NET.HTTP chunked bodies (C6L-2), baud/pins (hardware),
  bridge TCP framing/backoff (C6L-2), linkd protocol
  (C6L-5).

## 4. Caller migrations

- `user/uiapp/scr_llm.c`: `kv_load` now uses
  `uproto_kv_get` (a -2 truncation still counts as loaded,
  matching the old truncation behaviour); the `llm_key`
  presence check is `uproto_kv_get(...) != -1`; the three
  `KV.SET` calls are `uproto_kv_set` with explicit lengths
  (buffers: `prov[16]`, `model[33]`, `apikey[65]`).
  A local `slen` helper was added; three now-unused
  `resp[64]` declarations were removed.
- `user/uiapp/scr_ble.c`: see drift 2 above. No
  behavioural change to the screens beyond the honest
  peer token.
- `scr_wifi.c`, `scr_passkey.c`, `uvoice.c`: untouched —
  they use `uproto_cmd/cmd1/cmd2` and voice start/stop,
  whose signatures and semantics did not change.

## 5. Build wiring

Mirroring the `uvoice` §1d0 pattern (uartproto/cfg
objects are built at the uiapp section, so a dedicated
section — not the line-68 single-file loop):

- `mach3-port/build-md.sh`: new §1d1 `ulinktest` section
  (compile + link with `uartproto_uartproto.o`,
  `uartproto_mock_comcu.o`, `cfg.o`, `ulib.o`, `udiv.o`,
  embed cap 65536 — the 32768 one-shot cap does not fit
  the 35,025-byte test binary, same guard-only rationale
  as `sh`'s 65536); `ulinktest` added to the Fase C
  `_img.c` compile loop.
- `mach3-port/src/ext/user.c`: `ulinktest_img` externs +
  `{ "ulinktest", ulinktest_img, &ulinktest_img_len, 1u }`
  in `uprogs[]` **after** `uvoice` (sequential one-shots;
  the uvoice count check must not interleave).
- `uiapp` embed cap raised 65536 -> 98304 (see §7).

## 6. Verification

### 6.1 Test-first evidence

`scripts/verify-c6l1.py` (29 checks): boot 1 waits for
`ULINK: PASS` + 20 marker lines + `UVOICE: PASS` + no
`ULINK: FAIL`/FAULT + `qaon>` prompt; boot 2 (after
`reboot`) repeats `ULINK: PASS` + `qaon>` + `bootctl`
showing `fail_count=0`. Kernel overridable via `KERNEL`.

- **Pre-change FAIL (old kernel, no link layer):**
  `C6L1-VERIFY: FAIL (7/29)` — all 20 ULINK markers
  absent, both boots missing `ULINK: PASS`. (The 7 passes
  are the pre-existing UVOICE/shell-health checks.)
- **Post-change PASS (new kernel):**
  `C6L1-VERIFY: PASS (29/29)` on the first run.

### 6.2 The test program: `user/ulinktest.c`

Boot one-shot, self-cleaning (test-only `ulink_*` keys,
`WIFI.FORGET RUMAH-HAFIDZ`, `WIFI.DISC`, `BLE.OFF` +
`BLE.NAME QABOT-01` restore, at start AND end, so a
crashed run is repaired by the next run). 20 markers:

- Wi-Fi: scan (3 SSIDs), NEEDPASS, short-pass AUTH error,
  saved-pass CONNECTED, open-network CONNECTED (+STATUS
  `CONNECTED WARUNGKOPI`), unknown-SSID NOTFOUND,
  no-pending NOCONN, LIST/FORGET round-trip, STATUS
  DISCONNECTED.
- BLE: STATUS spec shape (`OFF NONE` / `ON NONE` /
  still `ON NONE` after `BLE.NAME` — the drift-killer),
  ON/OFF/NAME trio.
- KV: length-prefixed round-trip incl. a value with
  spaces and an empty value; NOTFOUND, DEL-then-missing,
  69-byte client-guard rejection, FULL (fill to 8 slots,
  next SET fails, created keys deleted); truncation
  (-2, `vlen_out` = 16, value `0123456` in an 8-byte
  buffer); legacy inline shape rejected + immediate
  `TIME.GET` still works (no desync).
- AUD: 8 frames of 512 B accepted, `VOICE.STOP` returns
  8; legacy `AUD` frame and bad-CRC QA frame rejected
  while voice is active (so the rejection proves the
  frame, not the state).
- Seam proof: a local `stub` backend (records wire
  bytes, canned responses) is selected; the same client
  calls return the same results, the recorded `KV.SET`
  wire equals byte-for-byte
  `KV.SET ulink_stub 3\nxyz`, then mock is restored and
  SCAN returns 3 SSIDs again.
- TIME/FIDO/unknown: `TIME.GET` (real or `MOCK-`
  labelled), `FIDO.LIST` = `0`, unknown command fails.
- Malformed KV (LAST): `KV.SET ulink_bad 5` with no body
  times out after 2 s (no hang); backend close+reopen
  resynchronises and `TIME.GET` succeeds.

### 6.3 Battery (standalone, worktree kernel)

Full battery, worktree kernel `mach3-port/build/mach3.elf`:

| Script | Result |
|---|---|
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS (21/21) |
| verify-fnd3.py | battery run FAIL (45/46, "GUI daemons launched"); **standalone rerun PASS (46/46)** |
| verify-fnd4.py | PASS (36/36) |
| verify-fnd5.py | PASS (33/33) |
| verify-ipc2.py | PASS (34/34) |
| verify-ipc4.py | PASS (58/58) |
| verify-ipc5.py | PASS (20/20) |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS |
| verify-c6l1.py | PASS (29/29), also re-passed inside the battery |
| verify-shell.sh | PASS |
| verify-safemode.sh | battery run FAIL (state); **standalone rerun PASS** |
| verify-firstboot.sh | battery run FAIL (state); **standalone rerun PASS** |
| verify-update.sh | PASS (battery run; also PASS on the post-reset standalone rerun) |

The three battery-run failures are NOT C6L-1 regressions;
all three pass standalone, and two of them have a proven
state-precondition violation in the battery run:

- **FIRSTBOOT**: requires an unprovisioned disk at boot 1.
  The battery ran ONB1 (which provisions) before it, so
  boot 1 saw a provisioned device ("banner missing").
- **UPDATE**: passed in the battery run (v262/A -> trial
  -> rollback -> v262/A/idle, ROLLBACK ev=7 present).
  It is state-sensitive, and that sensitivity was
  demonstrated directly: a manual rerun immediately
  after the battery, on the dirty post-battery disk,
  failed on the missing ROLLBACK event, while the
  post-reset standalone rerun passed from v1/A/idle.
  The battery-run result stands as PASS; the manual
  mid-state rerun is not counted as a battery result.
- **SAFEMODE**: requires a healthy start (fail_count=0).
  The battery run entered boot 2 with an inherited
  fail_count=3 (from FND1/FND3 crash tests), so boot 3
  re-entered safe mode via the counter path. Mechanism,
  verified in `bootstate.c`: the healthy mark resets
  `fail_count` but does not clear the in-RAM
  `bs_safe_mode` flag set at init, so a boot that
  enters safe mode reports `safe_mode=1` for its whole
  lifetime even after becoming healthy. Pre-existing
  behaviour, untouched by this phase.
- **FND3**: no precondition violation was proven for its
  single failed check ("GUI daemons launched", a 60 s
  wait after `gui` on serial). It passes standalone
  (46/46), which matches the documented back-to-back
  flake pattern for this battery. It is reported here
  as a flake, not hidden, and not claimed as proven
  state-caused.

Standalone reruns used a pristine boot state: the state
sector (second-to-last blk sector) and the 16-entry
bootlog ring below it were zeroed (backup at
`/tmp/mach3-blk-pre-reset.img`, worktree-local images
are untracked so the commit is unaffected). A zeroed
state sector is exactly the fresh-device path in
`bootstate_init` (magic mismatch -> defaults: boot #1,
unprovisioned, v1/slot A/idle). After the reruns the
disk was verified healthy and unprovisioned: final
boot shows `boot #12 fail_count=0 safe_mode=0` and
`setup: NOT provisioned`. The `update` rerun advanced
the update version to 2/slot B/idle (its own scripted
flow, not a leftover); provisioning and health are
clean.

## 7. Size impact and build hygiene

- `uiapp.bin` grew 62,249 -> 69,133 bytes (padded).
  Measured object growth: uartproto 4,127 -> 6,487 and
  mock 10,279 -> 14,141 (streaming reader + KV framing +
  wire-level parser). The link layer costs ~6.2 KB;
  the rest is screen migration noise. The old 65536
  embed cap no longer fits, so it was raised to 98304.
  The cap is a build-time guard only — the loader takes
  the real image size — and the boot partition headroom
  absorbs it (below).
- Mock buffers right-sized in the same pass: response
  queue 1024 -> 256 (largest response ~85 B),
  accumulator 512 -> 256 (largest single write ~160 B).
- Kernel binary (`objcopy -O binary`): 3,310,540 bytes;
  **headroom 883,764 B** (4,194,304-byte partition),
  down from IPC-5's 950,840 B. About half the delta is
  `ulinktest` itself (35,025 B, boot-only test program).
  Follow-up for UI-1: decide whether boot-only test
  programs stay embedded in production images, since
  they now cost ~35 KB each against the LVGL budget.
  (The stale-`_img`-object hygiene note from IPC-5 still
  stands: this build was clean, MD 43 ok, but the
  `*_img.o ok (16 programs)` message text is stale —
  the Fase C loop now covers 43 programs.)

## 8. Honest limitations

- The bridge backend does not exist yet (C6L-2). The
  seam is proven by the stub backend, not by a real
  second transport — a TCP bridge could still surprise
  the reader (partial reads across segment boundaries
  are handled by the streaming reader, but reconnect
  backoff and arbitration vs qabotd are C6L-2 work).
- The mock is generous in ways real hardware will not
  be: it answers instantly, never drops a byte, never
  sends unsolicited data. CRC corruption, mid-response
  drops, and silent peers are C6L-2 fault-injection
  cases for `mock-c6.py`, not covered here.
- AUD integrity at simulated 921600 baud (rate
  limiting, retry policy, stall absorption) is C6L-4.
  This phase proves frame validity only.
- `TIME.GET` before any sync still returns a
  `MOCK-...` label; the SYS_TIME_QUALITY gate (syscalls
  100/101) is untouched, and C6-sourced time feeding
  that gate is FW-track work.
- The `llm_key` via KV vs SYS_CRED migration question
  (owner decision at FW-1 time) is NOT resolved here:
  `scr_llm.c` still uses KV, as before. The credstore
  stays authoritative for web-entered keys.

## 9. What comes next (not built here)

C6L-2 (host-bridge backend + `scripts/mock-c6.py`,
NET.HTTP chunked bodies, fault injection, D-C6L-4
arbitration), C6L-3 (TOOL.DISPATCH/EVT/NET.WS, inbound
path), C6L-4 (AUD integrity at simulated 921600),
C6L-5 (linkd daemon + `link` registry binding +
shell `link` status command).
