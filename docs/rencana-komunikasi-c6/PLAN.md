# Plan: RV1103 <-> ESP32-C6 Link, Board-less Track (C6L)

Date: 2026-10-23. Research: `docs/riset-komunikasi-c6/
RINGKASAN.md` (current-state inventory + QEMU channel
evaluation; read it first). Protocol baseline: UART v1
(`docs/rencana-app-qaonicos/RENCANA.md` section 4) + v2
draft (`docs/riset-esp32c6-companion/RINGKASAN.md` section
5). Hardware acceptance for the finished link lives in
`docs/rencana-uji-hardware/RENCANA.md` section H2 and is
**not** replaced by anything here: every phase below is
QEMU-scope and says so.

## Design decisions (from the research)

- **D-C6L-1: one link layer, pluggable backends.** The
  existing seam in `user/uartproto/uartproto.c` (`xchg()` +
  `mock_comcu_aud()`) becomes a backend interface
  (open/write/read-with-timeout/close). Backends: `mock`
  (existing in-process mock, unit tests), `bridge` (host
  Python peer over the guest TCP stack), `uart` (hardware,
  later). Protocol code never knows which backend is live.
- **D-C6L-2: the host peer is the executable spec.** A
  Python mock C6 (`scripts/mock-c6.py`) speaks the exact
  wire bytes. It is the fixture for all QEMU verification
  and the reference implementation FW-1 will be written
  against on the C6 side.
- **D-C6L-3: freeze the wire before building peers.**
  Two drifts found by the research must be resolved in
  C6L-1, on paper and in the v1/v2 documents: (i) KV
  values — length-prefixed per the frozen spec, or inline
  per the code (recommendation: length-prefixed, values may
  then contain spaces and exceed one TX buffer); (ii) the
  binary frame format — the shipped AUD frame (3-byte magic,
  no type byte) vs the F5 generic sketch (recommendation:
  generalise to `magic(2B "QA") | type(1B) | len(2B BE) |
  payload | CRC16 BE` with types AUD/BIN/EVT, keeping the
  CRC-16-CCITT definition unchanged; AUD payload format
  unchanged).
- **D-C6L-4: one owner of the transport.** In bridge mode
  the guest TCP stack is a single connection (research
  section 1.6). The link layer owns it; qabotd's provider
  and the bridge never hold it simultaneously. The
  arbitration rule (link yields, or provider yields) is
  decided in C6L-2 and documented in code.

## Interface contracts (existing pieces)

### qabotd (TOOL.DISPATCH target)
Tools live in `user/qabot/tools.c` (registry `qb_tool_at`/
`qb_tool_find`), execution flows through the ReAct loop and
the policy gate in `user/qabot/loop.c` + `policy.c`
(ALLOW runs, CONFIRM needs a physical/web confirm, BLOCK
never runs). Contract: a dispatched call is executed **only**
through that same gate — the link never calls tool functions
directly. qabotd does not link uartproto today
(kernel/build-md.sh), so the hand-off (link -> qabotd) is
new surface, designed in C6L-3.

### face (provisioning, ONB-3/4)
`user/face/main.c` setup state machine today only polls the
bootstate provisioned flag (syscall 98 sub 3). Contract:
when ONB-3/4 land, the setup flow calls WIFI.SCAN/CONNECT/
PASS through the link layer (same API the Wi-Fi screen uses)
and sets the provisioned flag only on OK CONNECTED. No
link calls are added to face in this plan; the contract is
recorded so C6L phases keep the API shape face will need
(async scan results, NEEDPASS state).

### webui / settings (KV vs credential store)
Two secret stores exist today: the kernel credential store
(B5.5, SYS_CRED 104, `kernel/kernel/arm/credstore.c`), used
by web settings for the API key; and KV.* via the mock,
used by `user/uiapp/scr_llm.c` for llm_provider/llm_model/
llm_key — persisted on the RV1103's SD card in QEMU, but
destined for the C6's NVS on hardware (app plan section 5).
Contract: webui/credstore stays the authoritative store for
keys entered via web settings; KV carries link-level data
(Wi-Fi credentials, BLE name, saved networks) plus, until
FW-1, the LLM settings the uiapp screens edit. **Open
decision for the owner (record, do not silently resolve):**
at FW-1 time, does llm_key move to the C6 with KV, or does
scr_llm migrate to SYS_CRED? The plan works either way;
C6L phases must not deepen the split.

### ntp (TIME.GET as a time source)
`user/ntp.c` syncs from UDP SNTP and marks quality via
SYS_TIME_QUALITY (syscalls 100/101); only a validated sync
sets the "synced" state (the UTIMEGATE rule). Contract: a
C6-sourced time (TIME.GET over the link) is a *second
source feeding the same gate* — the link layer reports the
timestamp + source, a consumer marks it synced only under
the same validation rules as NTP. The mock's TIME.GET
already reads the RV1103 clock (QEMU direction); on
hardware the value flows the other way. C6L-2's host peer
returns real host time so the guest-side consumption path
can be built and tested now.

## Phases

### C6L-1 — Link layer formalisation + wire freeze
Goal: the transport seam becomes a real backend interface;
the two wire drifts are frozen; coverage of the existing
mock becomes testable command-by-command.
Scope:
- Freeze documents: update the v1 text (KV framing) and
  the v2 frame format per D-C6L-3, in the app plan section
  4 / F5 section 5 respectively, marked "frozen v1.1/v2".
- Refactor `user/uartproto/`: backend interface + `mock`
  backend (current behaviour preserved byte-for-byte);
  `uproto_*` API unchanged for existing callers.
- Raise the RX path for multi-line responses: streamed
  reads instead of the fixed 512 B buffer (a real scan
  list does not fit; the canned 3-SSID mock hides this).
- New one-shot `user/ulinktest.c`: every v1 command against
  the mock with expected responses, including error paths
  (ERR NOTFOUND, ERR AUTH, ERR NOCONN, KV ERR FULL,
  unknown command), plus AUD round-trip via the frame API.
Deliverables: refactored uartproto, frozen wire docs,
ulinktest printing `ULINK: PASS`.
Verification (QEMU): build; boot; `UVOICE: PASS` and
`ULINK: PASS` in the boot log; GUI regression — Wi-Fi/BLE/
LLM/Passkey screens driven per docs/VERIFIKASI-GUI.md
(screenshots + transcript) unchanged in behaviour.
Non-goals: no new transport, no new commands, no kernel
changes.

### C6L-2 — Host-bridge backend + Python mock C6 peer
Goal: the link runs end-to-end over a real byte channel in
QEMU, against a peer process — the first time framing,
timeouts and partial reads are honestly exercised.
Scope:
- `scripts/mock-c6.py`: wire-protocol peer on a host TCP
  port. Full v1 coverage *as frozen in C6L-1*, including
  the pieces the in-process mock lacks: **NET.HTTP** (with
  the chunked binary body transfer), FIDO.DEL, and
  configurable scan lists / auth outcomes. Real host time
  for TIME.GET. Fault-injection flags: response delay,
  CRC corruption rate, mid-response drop, silent peer.
- `bridge` backend in the link layer: guest connects via
  SYS_TCP_* to 10.0.2.2:<port> (slirp), line/frame codec
  shared with the mock backend's expectations, connect/
  reconnect with bounded backoff, command timeout
  (default 2 s) and the D-C6L-4 arbitration rule vs
  qabotd's provider connection.
- Backend selection at boot (shell command or bootstate
  flag), defaulting to `mock` so existing flows are
  untouched.
Deliverables: mock-c6.py, bridge backend, `scripts/
verify-c6link.py` (style of scripts/verify-onb1.py:
boot QEMU with a reader thread, start the peer, run
ulinktest against the bridge, assert markers).
Verification (QEMU): verify-c6link reports PASS with:
ulinktest fully green over the bridge; NET.HTTP body
integrity checked by hash against the peer's payload;
peer killed and restarted mid-session -> link reports
ERR TIMEOUT, then recovers without a guest reboot.
Non-goals: no TOOL.DISPATCH/EVT yet; no baud simulation;
no changes to FW (peer is a test fixture, not firmware).

### C6L-3 — v2 completion: TOOL.DISPATCH, EVT, NET.WS framing
Goal: the peer-initiated half of v2 exists and is tested.
Scope:
- Inbound path in the link layer: while idle, the backend
  listens for peer-initiated frames; EVT frames (per the
  frozen format) are surfaced to consumers (first consumer:
  a link status line + ulinktest assertions; WIFI.DROPPED
  updates the Wi-Fi screen state).
- TOOL.DISPATCH round-trip: peer sends a dispatch (JSON
  tool call); the link hands it to qabotd's tool path
  **through the policy gate** (contract above); result
  JSON returns to the peer. CONFIRM-class tools return a
  distinct "needs confirm" result in this phase (the
  confirm UX itself is ONB scope, not here).
- NET.WS: framing only — open/close/data over the data
  plane against a host echo endpoint in mock-c6.py; no
  product consumer yet.
- mock-c6.py learns to initiate: scripted dispatch of
  `get_info` and `gpio_read`, an EVT WIFI.DROPPED, and a
  WS echo session.
Deliverables: inbound/EVT/dispatch support in the link
layer + qabotd hand-off, extended mock-c6.py, extended
verify-c6link.py.
Verification (QEMU): verify-c6link PASS including:
dispatch round-trip returns the tool's real output;
a BLOCK-class dispatch (self_destruct) is refused by the
gate and reported as refused; injected WIFI.DROPPED is
observed by the guest; WS echo returns payload intact.
Non-goals: no real cloud endpoint; no confirm UX; no BLE.

### C6L-4 — AUD streaming integrity + error semantics at simulated 921600
Goal: prove the voice data plane and the link's failure
semantics under realistic rate and fault conditions,
before hardware has to.
Scope:
- Rate limiting in the bridge to a true 921600-baud byte
  budget (and a 115200 profile to demonstrate why v1 baud
  fails voice, per F5 section 4).
- Streaming test: 10 s of 16 kHz/16-bit audio (320 KB,
  1024 B frames) from the kernel audio path through the
  link to the peer; peer validates every CRC and counts
  frames/bytes.
- Error/timeout/retry semantics, documented and enforced:
  per-command timeout; at most one retry, only for
  idempotent commands (never WIFI.PASS, never KV.SET with
  a changed value); frame CRC failure = drop + count,
  stream continues; peer silence > timeout = link-down
  state surfaced to consumers, auto-recover on return.
Deliverables: streaming test in verify-c6link.py (or a
sibling verify-c6voice.py), semantics section in the
uartproto docs/header.
Verification (QEMU): stream completes with 0 CRC errors,
exact frame/byte counts at the peer, within the simulated
baud budget; fault runs (corruption rate > 0, peer drop
mid-stream) produce the documented link-down/recovery
behaviour and counters — asserted, not eyeballed.
Non-goals: real-time pacing against the RV1103 audio DMA
(that is H4); no STT endpoint.

### C6L-5 — Link daemon (linkd) as single transport owner
Goal: consumers stop linking the mock/transport directly;
one daemon owns the backend and serves uiapp, uvoice,
qabotd (and later face) — the shape the hardware build
will ship.
Scope:
- New daemon image `linkd` (daemon table grows: NDAEMON
  and daemon_images in kernel/kernel/arm/user.c are
  touched deliberately, with the load-order caveat those
  tables carry); consumers reach it via a small client
  stub with the same `uproto_*` signatures.
- Backend selection and link state (up/down, peer info)
  owned by linkd; the shell gains a read-only `link`
  status command.
Deliverables: linkd, client stub, migrated consumers.
Verification (QEMU): full regression battery — boot
one-shots (ULINK/UVOICE/UCREDSTORE/ULFS...), ONB1/ONB2
verify scripts, GUI screens via linkd, verify-c6link
green end-to-end.
Non-goals: no protocol changes (this phase is topology
only); no hardware backend.

## Hardware follow-ups (explicitly NOT board-less)

- **FW-1..FW-3** — the real C6 firmware (F5 section 6):
  control plane + NET.HTTP/TLS, then the binary data plane
  + voice uplink, then the Muse Gadget coexistence
  evaluation. Written against the wire frozen in C6L-1 and
  the behaviour of scripts/mock-c6.py.
- **RV1103 UART4 kernel driver** — Rockchip UART (not the
  QEMU PL011 register model), RTS/CTS flow control, baud
  divisor for 921600 from the real clock tree; a `uart`
  backend for the link layer behind it. Pinout per F5
  section 7 (UART4 on the Pico Mini header; UART2 stays
  the debug console).
- **H2 acceptance** (docs/rencana-uji-hardware/RENCANA.md):
  H2.1 1 MB random-pattern integrity at 921600 on the
  wire; H2.2 WIFI.SCAN/CONNECT against real APs; H2.3
  NET.HTTP to a real HTTPS endpoint; H2.4 TIME.GET vs
  phone clock < 5 s; H2.5 real TLS provider call; H2.6
  10 s voice uplink, frame/sample counts, playback; H2.7
  TOOL.DISPATCH from the C6 side. Plus H3 (BLE/Wi-Fi
  coexistence, SRAM footprint) and H4.2 (full audio chain
  to STT).
- **Electrical/mechanical**: wiring UART4 + RTS/CTS
  between Pico Mini and the C6 module, level check, power
  budget (C6 Wi-Fi TX peaks ~354 mA, F5 section 1).
- **Secrets decision** (contract above): at FW-1, settle
  whether llm_key follows KV to the C6 NVS or scr_llm
  migrates to SYS_CRED; likewise Wi-Fi passwords move
  from the RV1103-side store to the C6 with WIFI.PASS
  semantics on real hardware.

## Phase map

| Phase | One line | Depends on |
|---|---|---|
| C6L-1 | Backend interface + wire freeze + full mock coverage test | — |
| C6L-2 | TCP host-bridge backend + Python mock C6 (incl. NET.HTTP) | C6L-1 |
| C6L-3 | TOOL.DISPATCH + EVT + NET.WS framing | C6L-2 |
| C6L-4 | AUD integrity + error semantics at simulated 921600 | C6L-2 (C6L-3 for EVT counters) |
| C6L-5 | linkd daemon owns the transport; consumers migrate | C6L-2..4 |
