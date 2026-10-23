# Research: Luckfox (RV1103) <-> ESP32-C6 Communication Link

Date: 2026-10-23. Builds on F5 (`docs/riset-esp32c6-companion/
RINGKASAN.md`, decisions stand: C6 peer, UART4 on Pico Mini,
hybrid text control plane + binary data plane, 921600 baud for
voice) and the frozen UART v1 protocol (`docs/rencana-app-
qaonicos/RENCANA.md` section 4). Scope of this document: what
exists today in code, and how the link can be exercised
end-to-end in QEMU with neither the board nor a real C6.
Implementation plan: `docs/rencana-komunikasi-c6/PLAN.md`.

## 1. Current-state inventory (verified against code)

### 1.1 The v1 client library exists — userspace, one seam

`user/uartproto/uartproto.c` + `uartproto.h` implement the v1
framing: text lines `CMD arg..\n` -> `OK payload` /
`ERR code`, via `uproto_cmd()` / `uproto_cmd1()` /
`uproto_cmd2()`. Buffers: TX 160 B, RX 512 B (`uartproto.c`).

The transport is a single seam, `xchg()` in `uartproto.c`,
which today calls `mock_comcu_handle()` — an **in-process
function call**. No bytes cross any device in QEMU: no UART,
no socket, no IPC. The header comment in `uartproto.h` states
this honestly: QEMU = in-process mock; hardware = "UART data
PL011 (not yet implemented — the junction point is prepared)".

Only two programs link the library (kernel/build-md.sh,
sections 1d and 1d0): **uiapp** (with `cfg.o`) and the
**uvoice** one-shot test. qabotd does **not** link it.

### 1.2 The mock co-MCU — what it actually implements

`user/uartproto/mock_comcu.c` is a canned-response simulator
whose backing store is `user/cfg/cfg.{c,h}`: an `nvs_state`
struct (868 B, CRC-16/CCITT, magic `QNVS`) persisted
write-through to two SD sectors (LBA 552). Note the honest
deviation this creates: in QEMU the "co-MCU NVS" lives **on
the RV1103's own SD card**; on hardware, protocol section 5
of the app plan says secrets belong in the C6's encrypted
NVS. Same code, different trust boundary — flagged for the
plan (section 4.3 below).

Implemented commands (mock_comcu.c, verified line by line):

| Command | Status | Notes |
|---|---|---|
| WIFI.SCAN | yes | canned: 3 SSIDs (RUMAH-HAFIDZ/KANTOR-5G/WARUNGKOPI), fixed RSSI/security |
| WIFI.CONNECT | yes | OK NEEDPASS (WPA2) / OK CONNECTED (OPEN) / ERR NOTFOUND |
| WIFI.PASS | yes | OK CONNECTED (password >= 8 chars) / ERR AUTH / ERR NOCONN; saves network to NVS (max 2) |
| WIFI.LIST | yes | from NVS |
| WIFI.FORGET | yes | NVS write-through |
| WIFI.STATUS, WIFI.DISC | yes | **mock extensions** — not in the frozen v1 list |
| BLE.ON / BLE.OFF / BLE.NAME / BLE.STATUS | yes | state in NVS; STATUS returns `ON\|OFF <name>` (v1 spec says `<paired_addr\|NONE>` — drift) |
| KV.SET / KV.GET / KV.DEL | yes | max 8 keys, key 32 B / value 64 B (cfg.h) |
| TIME.GET | yes | real wall clock if `sys_time_get()` is set, else the honest literal `MOCK-2026-09-29T09:40:00+07:00` |
| FIDO.LIST | stub | always `OK 0` |
| FIDO.DEL, FIDO.APPROVE, FIDO.DENY | **no** | in v1 spec, absent from mock |
| FIDO.EVENT (async) | **no** | no async path exists anywhere (see 1.4) |
| NET.HTTP | **no** | in v1 spec (chunked binary body); absent from mock, client, and all callers (grep: no occurrence in user/) |

Wire-format drift vs the frozen v1 text: section 4 of the
app plan specifies `KV.SET <k> <len>` and `KV.GET -> OK <len>`
(length-prefixed values); the implementation sends the value
inline (`KV.SET <k> <v>`, built by `uproto_cmd2` and used by
`user/uiapp/scr_llm.c`). Values therefore cannot contain
spaces and are capped by the TX buffer. Either the spec or
the code must move before a real peer is written against it.

### 1.3 The v2 data plane — partially implemented (D4)

Implemented (audio track D4, 2026-10-19; `uartproto.c`,
`mock_comcu.c`, test `user/uvoice.c`):

- Control: `VOICE.START <rate> <bits>` /
  `VOICE.STOP -> OK <frames>` via `uproto_voice_start/stop()`.
  Caveat: the mock accepts only the exact string
  `VOICE.START 16000 16` (mock_comcu.c); any other rate/bits
  returns ERR UNKNOWN.
- Frames: `[ 'A' 'U' 'D' | len u16 BE | payload (PCM16 LE,
  <= 1024 B) | CRC u16 BE ]`. CRC = CRC-16-CCITT (poly
  0x1021, init 0xFFFF) over magic+len+payload
  (`voice_crc16()` in uartproto.c; duplicated as
  `mock_crc16()` in the mock, which validates magic, length
  and CRC per frame and counts accepted frames:
  `mock_comcu_aud()`).
- Test: `user/uvoice.c` one-shot sends 8 x 512 B chunks from
  the kernel audio path and checks the mock's frame count —
  `UVOICE: PASS` in QEMU.

Drift vs the F5 draft: F5 section 5 sketched a generic frame
`magic(2B) | type(1B) | len(2B) | payload | CRC`. What shipped
encodes the type *in* a 3-byte magic ("AUD") and has no type
byte. Functionally equivalent for one frame type, but the
generic BIN/EVT types were never built:

| v2 item (F5 section 5) | Status |
|---|---|
| VOICE.START / VOICE.STOP | implemented (see above) |
| AUD frames | implemented |
| BIN frames (generic blob) | **not implemented** |
| EVT frames (C6 -> RV1103 async events, e.g. WIFI.DROPPED) | **not implemented** |
| TOOL.DISPATCH | **not implemented** (no occurrence in user/ or kernel/) |
| NET.WS | **not implemented** |

### 1.4 Async events do not exist

The v1 spec assumes asynchronous peer->host events
(FIDO.EVENT, WIFI.DROPPED) delivered "via UART IRQ -> ring
buffer -> event queue". Nothing of this exists: `xchg()` is
strictly synchronous request/response, the kernel UART
(`kernel/kernel/arm/uart.c`) is a polled console driver, and
there is no UART-related syscall at all (docs/SYSCALL-ABI.md;
the syscall table in kernel/kernel/arm/user.c has console
ownership, SYS_CONSOLE_TAKEOVER 81, but no data-UART call).
Any design that needs peer-initiated traffic (TOOL.DISPATCH
is peer-initiated by definition) must add an inbound path —
today there is none.

### 1.5 Consumers today

- `user/uiapp/scr_wifi.c` — WIFI.STATUS/SCAN/CONNECT/PASS/DISC.
- `user/uiapp/scr_ble.c` — BLE.STATUS/ON/OFF.
- `user/uiapp/scr_llm.c` — KV.GET/KV.SET for llm_provider,
  llm_model, llm_key.
- `user/uiapp/scr_passkey.c` — FIDO.LIST.
- `user/uvoice.c` — D4 audio uplink test.
- The face (`user/face/main.c`) does **not** use the link at
  all today; its setup state machine only polls the
  bootstate provisioned flag.

### 1.6 Kernel side

`kernel/kernel/arm/uart.c` is console-only: one PL011 at
0x09000000, polled TX/RX; `uart_init()` enables the 16-byte
FIFO + 8N1 and does not even program the baud divisor (QEMU
initialises the PL011). There is no second-UART driver, no
RV1103 UART driver (drafts exist only for SFC NAND and SPI
LCD in kernel/kernel/arm/draft/), and no link daemon: the
daemon table (user.c, NDAEMON 6) is face, uiapp, qabotd, sh,
utcpkeep + one spawn slot.

Also relevant: the guest TCP stack behind SYS_TCP_*
(syscalls 73–77, `user/ulib/ulib.h` sys_tcp_connect/send/
recv/status/close) is a **single connection** state machine
(kernel/kernel/arm/tcp.c). Any second user of TCP must
arbitrate with qabotd's provider connection.

## 2. The QEMU channel question

The guest has exactly one UART (the console PL011, owned by
sh). Four routes to a byte channel for the link were
evaluated.

### (a) A second PL011 — impossible on this machine

Evidence: the device tree of the toolchain QEMU (8.2.2,
`-M virt`, dumped via `-machine dumpdtb`) contains exactly
**one** UART node, `pl011@9000000`. `pl011` is not a
user-creatable device (absent from `-device help`). Extra
`-serial` options only attach additional chardev backends to
that same single UART — they contend with the console
instead of creating a second port. Verdict: **no second
PL011 exists for the guest to address.** (On the real
RV1103 this is moot — the chip has real UART2/3/4 — but in
QEMU the route is closed.)

### (b) virtio-console (virtio-serial) — possible, expensive, throwaway

The device models exist in this QEMU (`-device help` lists
`virtio-serial-device`, `virtconsole`, `virtserialport`),
and the virt machine exposes 32 virtio-mmio slots at
0x0a000000 + n*0x200 (DTB). But the guest has no virtio
transport framework: blk.c, gpu.c and net.c each hand-roll
their own virtio-mmio probing/queues, and there is no PCI
support anywhere in kernel/kernel/arm/. A virtio-console
driver means a new kernel driver (transport + console port
protocol: control queue, port open/close events, two
virtqueues) that is **QEMU-only by construction** — the
RV1103 target has no virtio anything. Highest cost of all
options, zero code transfer to hardware; only the transport
*interface* would survive. Verdict: **rejected for now**;
revisit only if a byte-exact device-level emulation is ever
needed for its own sake.

### (c) Transport-agnostic link layer, pluggable backends — recommended

The seam already exists (`xchg()` + `mock_comcu_aud()`); this
option formalises it into a backend interface (open / write /
read-with-timeout / close) with three backends:

1. **mock** — the existing in-process mock, kept as the
   unit-test backend. Zero new risk; UVOICE and the screens
   keep passing byte-identically.
2. **bridge** — a host-side software C6 peer (Python) that
   speaks the *exact wire protocol* (text lines + AUD/EVT/
   BIN frames) over a channel the guest already has: its
   TCP stack via virtio-net + slirp, guest connecting to
   10.0.2.2:<port>. No new kernel driver of any kind. This
   exercises everything the in-process mock cannot: real
   serialisation, partial reads, framing resync, timeouts,
   reconnects, corrupted CRCs, peer death/restart — and the
   host peer can simulate Wi-Fi scan/connect outcomes, an
   HTTP fetch (NET.HTTP with chunked binary body), TIME.GET,
   KV, FIDO events, TOOL.DISPATCH requests, and EVT frames.
   Known risks, both manageable: (i) slirp/TCP quirks found
   in track F1 (keep-alive flakiness; the
   `Connection: close` pattern is the house rule) — a dev
   bridge tolerates reconnect semantics that the plan must
   specify anyway; (ii) the single-connection TCP stack
   (section 1.6) — the bridge backend and qabotd's provider
   cannot hold connections simultaneously, so the link layer
   must own and serialise TCP use in bridge mode. Acceptable
   for a development channel; on hardware the backend below
   replaces it.
3. **uart** — the hardware backend (RV1103 UART4), written
   with the board (see plan, hardware follow-ups). Same
   interface, so protocol code and tests do not change.

A sub-variant of the bridge: the peer could also live
*inside* the guest as a separate "virtual co-MCU" daemon
process over Mach IPC. That removes the host dependency but
IPC message semantics are not a byte stream, so framing/
timeout behaviour would again be simulated, not exercised.
Not recommended as the main channel.

### (d) Other routes

- PCI 16550 (`pci-serial` exists in `-device help`): the
  kernel has no PCI host driver at all — strictly worse
  than (b).
- Semihosting, debug channels, 9p/shared files: no kernel
  support; not byte channels in any useful sense.
- USB serial: no USB host stack in the kernel.

## 3. Recommendation

Adopt (c): formalise the existing seam into a link layer
with pluggable backends — mock (unit), TCP host-bridge
(QEMU end-to-end), UART4 (hardware). Write the host peer
once, in Python, against the wire spec; it doubles as the
executable reference for the future C6 firmware (FW-1) and
as the test fixture for every QEMU verification. Freeze the
two wire drifts found above (KV length-prefixing; frame
format) as part of the plan's first phase, before the peer
is written against them.

## 4. Interface notes for neighbouring pieces

(Full treatment in the plan; summary here.)

- **qabotd** — TOOL.DISPATCH must land in the existing tool
  registry (`user/qabot/tools.c`) and pass the existing
  policy gate (`user/qabot/policy.c`, loop in
  `user/qabot/loop.c`: ALLOW/CONFIRM/BLOCK). qabotd does not
  link uartproto today; the dispatch hand-off is new work.
- **face** — ONB-3/4 provisioning will drive WIFI.* through
  this link from the setup flow; today face only polls the
  bootstate flag and never calls the link (section 1.5).
- **webui / settings** — B5.5 put API keys in the on-device
  credential store (SYS_CRED, kernel/kernel/arm/credstore.c),
  while scr_llm.c still writes llm_key through KV.* (i.e. to
  the mock's SD-backed store). Two stores for the same secret
  is a decision the plan surfaces explicitly.
- **ntp** — `user/ntp.c` sets the wall clock from UDP SNTP
  (sys_time_set + the SYS_TIME_QUALITY gate, syscalls
  100/101). On hardware the direction of TIME.GET reverses:
  the C6 learns time over Wi-Fi and the RV1103 consumes it —
  through the same quality gate, not around it.

## Sources

All claims above are from files in this repository as read
on 2026-10-23 (paths cited inline), plus two QEMU probes run
against the workspace toolchain: `-device help` and a
`-machine dumpdtb` dump of `-M virt` (one `pl011@9000000`
node; 32 `virtio_mmio` slots). Statements about the future
(C6 firmware behaviour, RV1103 UART register model) are
marked as plan items, not findings.
