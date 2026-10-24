# QaonicOS Architecture: The Kernel / Userspace Boundary

Date: 2026-10-23. Status: standing reference, next to
`docs/SYSCALL-ABI.md` (the interface list) and
`docs/MIGRASI-MACH3.md` (the port history). This document
is the **policy** those two never stated: what may live in
the kernel, what must live in userspace, what today's
kernel residents are doing there, and where everything
already planned is going to land.

Until now the split grew organically, one phase at a time.
Most of it is right. Some of it is in the kernel because
that was the shortest path in the week it was written —
the HTTP server is the flagship case (§4.1). Verdicts in
§4 and decisions in §6 are **recommendations to the
owner**, labelled as such; nothing in this document
moves code by itself.

Evidence discipline: every inventory claim names its
files. Execution-model claims rest on
`docs/riset-threading-compute/RINGKASAN.md` §1, which
verified them against the code.

## 1. The model as it actually is

QaonicOS is **not** a Mach-orthodox system, and this
document does not pretend otherwise:

- **Mach 3 core — VM only.** Per the threading research
  (§1.2), the port kept Mach's VM: `setup_uprog_task()`
  creates a real Mach task + thread per program, and
  memory isolation is genuinely Mach's. Everything else
  Mach is famous for is bypassed: the `thread_t` is
  created and never scheduled, the Mach scheduler and
  Mach IPC ports play no part in the product path, and
  the IPC syscall numbers (10–12, 23) have sat reserved
  and unused since Fase B (`docs/SYSCALL-ABI.md`).
- **A kernel extension layer** (`mach3-port/src/ext/`,
  centred on the 2,270-line `user.c`) owns everything
  product-shaped in the kernel: the syscall dispatch,
  a hand-rolled cooperative scheduler, the daemon table,
  the hardware arbiters, and a set of resident services
  (network stack, HTTP server, filesystem, credential
  store, boot state — inventoried in §2).
- **Userspace** is ELF programs linked against `ulib`,
  in two shapes: **daemons** (persistent: `face`,
  `uiapp`, `qabotd`, `sh` — `daemon_images[]` in
  `user.c`; `ntp` and `utcpkeep` exist but are currently
  disabled in that table) and **one-shots** (run to
  completion at boot or via the spawn slot). All user
  execution rides the boot thread: the scheduler switches
  a hand-rolled `struct user_ctx` between daemons, and
  only from two points — the `SYS_YIELD` handler
  (`sched_yield_switch`) and the exit paths
  (`sched_exit_switch`).
- **Syscalls are the only crossing.** There is no other
  channel: no shared memory ABI, no message passing.
  What other systems call "IPC" here is *shared kernel
  globals behind a syscall surface* (the display token,
  the web-shell bridge, the bootstate record).
- **The ABI is poll-shaped by design.** Consumers poll
  (`SYS_TCP_RECV` returns 0 = try again) and yield.
  There are no blocking syscalls, and the threading
  research rejected adding them (its option D).
- **Execution-context fact that drives §4.1:**
  `net_pump()` — one iteration of the whole network
  server (RX + TCP timers) — has exactly two call sites,
  `user.c:1566` and `user.c:1665`, both scheduler switch
  points. `netmain.c` states the context in its own
  header: called from `sched_yield_switch`, in SVC trap
  context, interrupts off, driver purely polled. Inside
  `net_pump`, `tcp.c` accumulates a request and calls
  `http_handle()` (`http.c`). **The HTTP server —
  request parsing, session handling, HTML generation —
  therefore executes in kernel context today.** Not as a
  design decision anyone recorded; as an emergent fact
  of where the pump had to live.

Consequences the rest of the document relies on: a fault
in a userspace program is isolated (`user_fault` kills
the one task — FND-2 is completing that story); a fault
in a kernel resident is a kernel fault, full stop. And
the kernel image is inside the boot-partition budget —
the UI research measured ~287 KB of headroom — so every
kernel resident also spends a scarce, shared resource.

## 2. Placement inventory (today)

Layers: **MACH** (Mach 3 core), **KEXT** (kernel
extension layer), **DAEMON** (userspace daemon),
**1SHOT** (userspace one-shot), **LIB** (shared library,
linked into whichever side uses it). "Crossing" = the
syscall surface or data path between layers.

| # | Subsystem | Files | Layer | Crossing |
|---|---|---|---|---|
| 1 | Trap entry, syscall dispatch, daemon scheduler, spawn, exit/fault paths | `user.c`, `trap.c`, `context.s`, `userasm.s`, `uprog.s` | KEXT on MACH | all syscalls |
| 2 | Mach VM (tasks, pmaps), thread objects (created, unscheduled) | `pmap.c`, `pcb.c`, `thread.h` + Mach core | MACH | internal |
| 3 | FPU control (enable only; save/restore are stubs) | `fpu.c` | KEXT | none (THR-1 changes this) |
| 4 | GIC, timers, UART console driver, clock state | `gic.c`, `clock.c`, `uart.c` | KEXT | console via syscalls 20/59 |
| 5 | PSCI reset/poweroff | `psci.c` (FND-1) | KEXT | syscalls 106/107 |
| 6 | Boot splash (procedural logo into framebuffer) | `gpu.c`, `splash_text.h` | KEXT | none (pre-userspace) |
| 7 | Virtio block driver, block layer | `blk.c` | KEXT | syscalls 50/51 + internal |
| 8 | FAT32 | `fat32.c` | KEXT | syscalls 52–56; also kernel-internal (spawn loads images from it) |
| 9 | ramfs | `ramfs.c/.h` | KEXT | syscalls 30–34 |
| 10 | littlefs (vendored) + port (RAM backend, allocator, power-cut sim) | `littlefs/`, `lfs_port.c` | KEXT | syscalls 93–97 |
| 11 | Boot state + boot log ring (state sector) | `bootstate.c` | KEXT | syscall 98/99; `bootstate_note_*` internal |
| 12 | Display + console arbiters, event queue + key translation, event injection | `user.c` (dpy_holder, console_owner, evq, translate_key) | KEXT | syscalls 60–68, 105 |
| 13 | Face request state (expression + status text mailbox) | `user.c` (face_req_*) | KEXT | syscall 78 (`SYS_FACE_POLL` family) |
| 14 | Time state (wall clock, valid/synced flags) | `user.c`, `clock.c` | KEXT | syscalls 69/70, 100/101 |
| 15 | SYS_STAT aggregation (uptime, mem, blk, net counters) | `user.c` | KEXT | syscall 57 |
| 16 | GPIO (mock in QEMU, register driver on HW) | `gpio.c` | KEXT | syscalls 40/41 |
| 17 | Audio driver: ACODEC init, I2S0 RX ring, gain, PL330 stage 1 | `audio.c`, `pl330.c/.h` | KEXT | syscalls 82–92 |
| 18 | Virtio-net driver | `net.c` | KEXT | internal (pump) |
| 19 | Network stack: Ethernet/ARP/IPv4/ICMP/UDP demux (static IP 10.0.2.15; **no DHCP**) | `netstack.c` | KEXT | syscalls 71/72 (UDP) |
| 20 | TCP: one client connection (qabotd provider) + one server connection (HTTP), separate state machines | `tcp.c` | KEXT | syscalls 73–77 (client only); server side has **no** syscall surface |
| 21 | HTTP server: dashboard `/`, `/metrics`, routing | `http.c` (884 lines) | KEXT | called from `tcp.c` inside `net_pump` |
| 22 | Web UI: PIN gate + throttle, sessions, settings, web-shell bridge (ws_cmd/ws_out) | `webui.c/.h` (368), driven from `http.c` | KEXT | HTTP only; bridge via syscall 103 (sh side) |
| 23 | Credential store + AES-256-GCM | `credstore.c/.h`, `aesgcm.c/.h` | KEXT | syscall 104 |
| 24 | Update signature verification (RSA-2048 + SHA-256) + dev key | `updverify.c/.h`, `updkey.h` | KEXT | syscall 98 sub 9/10 |
| 25 | Hardware drafts, **not compiled**: SFC NAND, SPI0, ST7789 | `draft/` | KEXT (draft) | planned syscalls |
| 26 | face: procedural renderer, QR setup screen, power FSM, provisioning poll | `user/face/` | DAEMON (progid 0) | display + bootstate + lfs syscalls |
| 27 | uiapp: menu/settings/monitor/power screens, status bar | `user/uiapp/` | DAEMON (progid 1) | display + stat + cred syscalls |
| 28 | qabotd + Qabot harness (ReAct loop, policy gate, tools, providers) | `user/qabotd.c`, `user/qabot/` | DAEMON (progid 2) | TCP/TLS + face + tool syscalls |
| 29 | sh: the `qaon>` shell, all builtins, web-shell bridge server side, qfetch | `user/sh.c` | DAEMON (progid 3) | nearly all syscalls; bridge via 103 |
| 30 | ntp: SNTP client + its own DNS parser (daemon currently disabled) | `user/ntp.c` | DAEMON (off) | syscalls 71/72, 100/101 |
| 31 | One-shots: boot self-tests + feature probes (ulfs, ucred, uupdsig, uvoice, ublogtest, ufirst, utimegate, utimeset, upl330, uaudiotest/cap/gain, ugpio, usd, ufs, umon, …) | `user/u*.c`, `user/init.c`, `user/hello.c` | 1SHOT | per-feature syscalls |
| 32 | ulib: syscall wrappers + helpers | `user/ulib/`, `user/ulib.c/.h`, `user/usys.h` | LIB (user) | — |
| 33 | uartproto: C6 link protocol library + in-process mock co-MCU | `user/uartproto/` | LIB (user) | function calls today; C6L adds backends |
| 34 | cfg: NVS struct backing the mock's KV store (on SD sectors) | `user/cfg/` | LIB (user) | SD syscalls |
| 35 | TLS: vendored mbedTLS + port/bio/api glue | `user/tls/` | LIB (user) | syscalls 73–77 + entropy from `tls_port.c` |
| 36 | Qabot harness library (loop/policy/tools/JSON/history) | `user/qabot/*.c` (excl. main) | LIB (user) | — |
| 37 | TUI helpers (linked into umon) | `user/tui.c/.h` | LIB (user) | console syscalls |
| 38 | SHA-256 implementation | inside `updverify.c` (`upd_sha256`) | KEXT | shared in-kernel by webui + credstore |

Not runtime subsystems, for completeness: `ipc_test.c`,
`sched_test.c`, `db_stubs.c`, `mig_stubs.c` are Mach-core
glue/self-tests, not product residents; host build
tooling (`user/embed.py`, `user/pad-bss.py`) never ships.

## 3. Placement rules

### Must live in kernel

- **R1 — Traps, scheduling, memory.** Context switching,
  the trap frame, pmaps: the kernel is the only place
  these can exist by definition.
- **R2 — Hardware arbitration.** A hardware resource
  contended by two or more programs gets exactly one
  kernel-owned arbiter (a token, an owner field, a
  queue). Programs hold policy; the kernel holds the
  fact of who owns it. (Display token, console owner.)
- **R3 — Non-skippable safety invariants.** A check whose
  bypass breaks a product promise — *no unsigned update
  is ever staged*, *the credential key never leaves the
  store*, *a locked-out PIN stays locked out* — must be
  enforced where a buggy or hostile caller cannot route
  around it: in the kernel, atomically with the action
  it gates.
- **R4 — Hardware-only facts.** Data only registers can
  produce (timer ticks, ADC samples, entropy, device
  counters) is collected in the kernel and published as
  facts. Interpretation of those facts is not the
  kernel's job (see R6).
- **R5 — The pre-userspace path.** Whatever must work
  before any program can run — boot state, crash
  recording, the drivers that load programs, the boot
  splash — is kernel by construction.

### Must not live in kernel

- **R6 — No policy.** Thresholds, escalation ladders,
  retry choices, UX behaviour, feature rules: userspace.
  The kernel supplies mechanism and facts. (The one
  principled exception is a policy that must run when
  userspace cannot — FND-3's hang escalation — and it
  must be small, fixed, and justified under R14.)
- **R7 — No parsing of untrusted input.** Bytes from the
  network, from storage media, or from a link peer are
  parsed in userspace, where a parser bug is one dead
  program (§1). The kernel may do fixed-size,
  length-checked framing/demux — never grammars.
- **R8 — No application logic, no presentation.** No
  HTML, no screens, no human formatting in the kernel.
  The kernel publishes facts (R4); rendering them is a
  userspace act.
- **R9 — No waiting.** Kernel code never blocks on an
  external event. The poll-shaped ABI is the house
  style; it is what makes cooperative scheduling and
  the current non-reentrant kernel survivable.

### Grey-zone criteria (for everything in between)

- **R10 — Atomicity shape.** If correctness needs an
  atomic check-and-act across programs that a syscall
  round-trip cannot express, the kernel may hold the
  *state* (and only the state); the *policy* over it
  still lives in userspace.
- **R11 — Blast radius.** When two placements are
  otherwise equal, choose the one whose worst bug is a
  dead program, not a dead device. Kernel code is held
  to a higher audit bar precisely because it cannot
  fail small.
- **R12 — QEMU testability.** A kernel resident that
  cannot be exercised in QEMU must ship with a simulated
  source or a one-shot harness (precedents: the RAM
  littlefs backend with its power-cut simulator; the
  power research's simulated SYS_POWER source). A
  resident that can only ever be tested on hardware is
  a liability by default.
- **R13 — The image budget.** The kernel is linked into
  a boot partition with ~287 KB of measured headroom.
  Every resident spends it. Size is an architectural
  argument, not an afterthought.

### The admission rule

- **R14 — Kernel admission.** A proposal to add a new
  kernel resident must state, in its plan: (a) which of
  R1–R5 requires it; (b) why a syscall surface over a
  userspace owner fails (not merely "is more work");
  (c) the untrusted-input surface it adds, against R7;
  (d) its QEMU test story, against R12; (e) its size,
  against R13. If the answers are weak, the default is
  userspace. Future phase plans cite this rule instead
  of re-arguing placement from scratch.


## 4. Verdicts for today's kernel residents

Recommendations to the owner. **KEEP** = placement is
right, with the rule that justifies it. **MOVE WHEN
TOUCHED** = placement is wrong but tolerable; the next
feature change in that area pays the move. **MOVE
CANDIDATE** = placement is wrong enough to deserve its
own planned phase.

### 4.1 HTTP server + web UI (`http.c`, `webui.c`) — MOVE CANDIDATE

The flagship case, argued both ways.

*The case against (security + isolation).* This is
1,250 lines that parse untrusted network input —
request lines, headers, POST bodies up to the 3,072-byte
request buffer, cookies, form fields — and generate
HTML, executing in kernel context (§1) inside the
scheduler's switch path. Against the rules it is a
clean sweep: grammars in the kernel (R7), presentation
in the kernel (R8 — the dashboard is hand-built HTML),
policy in the kernel (session expiry, login flow, form
semantics; the PIN throttle is arguably R3 state, see
below). And against R11 it is the worst resident we
have: a parsing bug here is not a dead program, it is
a kernel compromise or a wedged scheduler — the pump
runs with interrupts off, so a hang in a handler is a
hang of *everything*, GUI and watchdog food included.

*The case for the defence (pragmatics).* Three facts.
(1) The TCP stack it sits on is itself kernel-resident
and single-connection; there is no listener the server
could move *to* — the server-side TCP state machine in
`tcp.c` has **no syscall surface at all** (row 20); the
only TCP syscalls expose the *client* connection, and
that connection is single-owner by design (the C6L
plan's D-C6L-4 arbitration exists because of exactly
this). (2) The data the dashboard shows is kernel facts
(R4); a userspace httpd would re-import them through
SYS_STAT-shaped calls — fine, but it is plumbing that
does not exist. (3) It works, it is small as web servers
go, and it is the owner's live settings surface: ONB-1's
19-check suite pins its behaviour, and any move that
breaks login/settings/web-shell breaks the product's
only admin path.

*What a move actually requires.* A server-socket
syscall surface (accept/read/write/close over the
server connection, poll-shaped per R9); a scheduling
story in which `net_pump` delivers bytes to a daemon
instead of calling `http_handle` in trap context; a
new `httpd` daemon owning `http.c`/`webui.c` nearly
unchanged (the code is already written against flat
buffers — the port is mostly mechanical); and the
19-check ONB1 suite re-run green against it. Rough
cost: **M** (a phase, not a weekend). Risk: moderate —
concentrated in the pump/scheduling seam, not in the
HTTP code itself. One part should *not* move: the PIN
gate's state (hash compare, throttle counters) is R3 —
a userspace httpd can render the login form, but the
verdict "is this PIN right, and are we locked out" is
exactly the class of check a compromised front-end
must not be able to skip. Keep a thin kernel gate
syscall; move pages, forms, sessions and HTML.

**Verdict: MOVE CANDIDATE** — as its own phase, gated
on the server-socket surface design, sequenced with
D-ARCH-3 (it is the same design problem as the
netstack's second consumer). Until it moves: treat
`http.c`/`webui.c` as hostile-input code — fixed
buffers only, no new routes or parsers without review,
and no other phase may deepen the residency (e.g. no
new kernel-side web features; new settings belong
behind the existing forms).

### 4.2 TCP/IP stack (`net.c`, `netstack.c`, `tcp.c`) — KEEP, with a boundary drawn

The virtio-net driver is a driver (R4/R5); the stack
is a protocol state machine with exactly one server
consumer and one client consumer, welded to the pump
mechanics (polled, no IRQ) that the cooperative model
depends on. Extracting it to a `netd` daemon today
would buy orthodoxy and cost a byte-copying boundary
on every frame plus a scheduling dependency for the
device's only network — a bad trade at one consumer.

The boundary to draw instead: **the stack stays; new
protocol work does not automatically join it.** The
trigger for revisiting is a second concurrent consumer
— and one is already visible: C6L's bridge backend
wants the same single client connection qabotd's
provider uses, which is why D-C6L-4 exists. When that
pressure becomes a design (shared socket surface,
connection multiplexing), the stack's top half —
transports and their syscall surfaces — is what gets
re-drawn, not the driver. **Verdict: KEEP** (driver +
stack); **MOVE WHEN TOUCHED** for any *new* transport
or second-consumer work, which must come with the
socket-surface design of §4.1/D-ARCH-3.

### 4.3 littlefs (`littlefs/`, `lfs_port.c`) — KEEP

A filesystem in the kernel offends microkernel taste
and is right here, for two concrete reasons: the
kernel itself is a client (the credential store and
provisioning config live in it, and the boot path
touches it before daemons exist — R5), and the
alternative — a filesystem server daemon — needs a
request/reply IPC channel that does not exist (§1;
D-ARCH-4). Its clients are already mediated by a
syscall surface (93–97), the port layer cleanly splits
backend (RAM today, NAND on hardware) from the
vendored FS logic, and its failure modes are data
errors, not control-flow corruption of other programs.
**Verdict: KEEP.** Revisit only if D-ARCH-4 ever
reverses and real IPC lands.

### 4.4 Credential store + AES-GCM (`credstore.c`, `aesgcm.c`) — KEEP

Textbook R3. The device key and the unwrap operation
must live where a caller cannot extract them: through
SYS_CRED 104 a program can store and retrieve *its*
secrets, but the key material never crosses the
boundary, and the store's file format (QSEC) is
fixed-size records — no grammar, no untrusted parsing
(R7 is satisfied: the input surface is names and
bounded values). The crypto is small, constant-shape,
and host-verified against published vectors (B5.5 dev
log). Moving it to userspace would *create* the
exposure it exists to prevent. **Verdict: KEEP.**

### 4.5 Update verification (`updverify.c`) — KEEP

Also textbook R3, and the sharpest invariant in the
system: *an image with a bad signature is never
staged.* Verification and staging are one atomic
syscall (98 sub 9: verify, then stage, or refuse with
-3). If verification lived in userspace, any program
could call the metadata-only staging paths and the
invariant would depend on every caller behaving —
the B5 threat model (T1/T2/T4) exists precisely to
not depend on that. FND-5 keeps this shape on purpose:
the shell reads the package *file* (userspace act),
the kernel verifies and stages (kernel invariant).
**Verdict: KEEP.**

### 4.6 Boot state + boot log (`bootstate.c`) — KEEP

R5 in its purest form: it must be written by the panic
path, read before userspace exists, and survive both.
It is also the substrate every recovery promise reads
(fail counts, safe mode, update slots, the FND-3
watchdog's reserved `BSR_WATCHDOG=3`). Its surface is
a fixed-size record + an event ring — no parsing, no
policy (the *decisions* over the record — safe mode
entry, rollback — are small, fixed, and part of the
boot mechanism itself). **Verdict: KEEP.**

### 4.7 Arbiters + event queue + face mailbox (`user.c`) — KEEP

R2's reference implementation: each is a handful of
words of state (a holder progid, an owner, a
16-entry ring, a pending flag + 24-byte text), every
policy decision about them already lives in userspace
(face decides when to grant; uiapp decides when to
release; qabotd decides what to request). This is the
shape §3 asks future arbiters to copy. **Verdict:
KEEP.**

### 4.8 The rest, briefly

- **Audio driver** (`audio.c`, `pl330.c`) — **KEEP**
  the register/FIFO/DMA layer (R4; its deadlines are
  hardware facts, and the threading research showed
  they are already tight). The voice *protocol* —
  framing, session policy — already lives in
  userspace (`uvoice`, uartproto), which is correct.
- **Time state** — **KEEP** as-is: kernel holds the
  fact (clock + valid/synced flags), the *rule* for
  what counts as synced is enforced at the syscall
  gate (only the NTP path may set it — the UTIMEGATE
  rule), and the NTP client itself is a userspace
  program (`user/ntp.c`). This is the model split for
  stateful facts.
- **SYS_STAT aggregation** — **KEEP**: pure fact
  assembly (R4). Its presentation already lives
  outside (qfetch, Monitor, and — §4.1's exception —
  the dashboard).
- **Spawn / program lifecycle** — **KEEP**: it *is*
  the scheduler (R1). Noted dependency: spawn loads
  images from kernel FAT32, a second concrete reason
  §4.3's verdict holds.
- **FAT32 + ramfs** — **KEEP**, by §4.3's argument
  (kernel-internal consumers, pre-userspace path).
- **Boot splash** (`gpu.c`) — **KEEP**: R5; it draws
  before any program exists. Runtime rendering is
  userspace's, through the §4.7 arbiters.
- **uartproto + mock co-MCU** — **correctly placed**:
  a link protocol is peer-input parsing plus policy,
  and it sits in a userspace library with its test
  double beside it. C6L keeps it there and gives it a
  daemon owner (linkd, C6L-5) — the pattern §5
  generalises.
- **TLS** (`user/tls/`) — **correctly placed**, the
  model citizen: large, parse-heavy, per-connection
  state, isolated failure. Its kernel needs are a
  client socket (exists) and entropy (weak today —
  SOC-2 fixes the *source*, in the kernel per R4,
  without moving the consumer).

## 5. Forward application (planned work, placed)

Each row cites the rule(s) that decide it. "Surface" is
the syscall boundary between the parts.

| Work | Kernel part | Userspace part | Surface | Rules |
|---|---|---|---|---|
| CRU / GRF / IOC helpers (SOC-1) | clock-gate + reset + pinmux register service | — (consumers call it) | kernel-internal API; narrow syscalls only where a program must request a gate | R1, R4, R14 |
| TRNG + entropy service (SOC-2) | TRNG driver + entropy pool | every consumer (TLS, credstore nonces, ONB secret) | new `SYS_RANDOM` | R4, R12 (simulated source in QEMU) |
| Crypto engine v3 (SOC-2) | driver behind the existing in-kernel crypto users (credstore, updverify) | TLS keeps its software path unless a bulk-op surface is justified per-op | internal for R3 paths; any `SYS_CRYPTO` needs R14 | R3, R7, R14 |
| RGA2 (SOC-4) | register driver: job submit, physical buffers, cache coherency | composition policy: face, uiapp/LVGL choose what to blit; software fallback stays | extension of the display surface (`SYS_DISPLAY_FLUSH`-family) | R2, R4, R8 |
| SFC NAND (HW phase) | SFC driver as the `lfs_port` backend | — | existing 93–97 (unchanged) | R5, §4.3 |
| SPI0 → ST7789 (HW phase) | SPI transfer primitive only | panel init sequences + all rendering (face) | `SYS_SPI_XFER` (per the draft's own plan) | R4, R8 |
| SARADC + power (PWR-1/2) | SARADC driver, 1 s sample cache, simulated source in QEMU | policy ladder + UI: the **face** owns policy (power research §4.2–4.3, already decided) | `SYS_POWER` (108) | R4, R6, R12 |
| SDMMC (SOC-3) | SDMMC driver under the block layer | card policy (insert/remove behaviour, the BootROM SD-priority decision) + FAT seams that already exist | existing 50/51 + 52–56 | R2, R6 |
| USB DWC3 gadget (SOC-3) | UDC driver: TRB rings, EP0 control transfers, link state | gadget class logic (CDC/RNDIS/composite), provisioning + update-delivery flows, in a daemon | new `SYS_USB_*` event + transfer surface (design in SOC-3's plan) | R2, R4, R7 (class parsing is userspace's) |
| GMAC (SOC-6, bench) | MAC driver feeding the existing kernel stack | — | existing net syscalls | §4.2 |
| C6 link (C6L-1..5) | on hardware only: the UART4 backend driver | **linkd** owns the transport + protocol (C6L-5, already decided); in QEMU the bridge backend is pure userspace over `SYS_TCP` | link-layer API in uartproto; `SYS_UART_*` only with the HW backend | R6, R7, D-C6L-1/4 |
| Watchdog (FND-3 + SOC synthesis) | supervisor mechanism + (on HW) the dw-wdt arming | policy is fixed + minimal by necessity — the R6 exception, justified in FND-3's plan | bootstate notes + syscalls 106/107 | R3, R6-exception, R14 |
| FPU context (THR-1) | FP state in `user_ctx`, save/restore in the switch | programs opt in via build flags (`softfp` ABI) | none (no new surface) | R1 |
| LVGL (UI-1..6) | — | entirely inside uiapp (UI research, already decided); face stays custom | existing display syscalls | R8 |
| Camera pipeline (SOC-5, product gate) | VICAP/ISP register drivers, DMA buffer machinery | sensor config policy, capture orchestration, Qabot vision logic; VEPU session control | new `SYS_CAM_*` surface (design in SOC-5's plan) | R2, R4, R6, R7 |
| HPMCU (THR-4, if ever) | mailbox/boot control, if the ground truth justifies it | the firmware is the vendor's; any QaonicOS-side logic is a consumer | none today | R14 (admission not yet passed) |
| Factory reset (backlog #6) | the erase primitive over credstore + bootstate (must not be skippable) | the flow, confirmations, physical-press gating (ONB-4 design) | existing 98/104 subs + one wipe sub | R3, R6 |

## 6. Owner decisions

Recommendations are this document's; the choices are
the owner's.

### D-ARCH-1 — The HTTP server + web UI's fate

- **(a) Keep in kernel + harden indefinitely.** Cheapest
  now; accepts a permanent R7/R8/R11 exception at the
  network edge, and every future web feature deepens it.
- **(b) Planned move to a userspace httpd daemon**,
  gated on a server-socket syscall surface; the kernel
  keeps only the PIN-gate verdict (§4.1). *Recommended.*
  Unblocks: fault isolation for the admin surface, web
  features without kernel changes, an honest §3.
  Forecloses: nothing structural — the code moves
  nearly as-is. Costs one phase (M), best sequenced
  with D-ARCH-3's design and after C6L (which needs the
  same arbitration thinking for the client connection).
- **(c) Move when touched.** Defers the design until a
  web feature forces it — at which point it will be
  done in a hurry, inside another phase's budget.

### D-ARCH-2 — Adopt the kernel admission rule (R14)

- **Adopt as binding** for FND/SOC/C6L/PWR/UI phase
  plans: each plan states placement against §3 instead
  of re-arguing it. *Recommended.* Unblocks: consistent
  reviews, smaller kernel. Forecloses: nothing but
  unexamined kernel growth.
- **Advisory only.** Keeps flexibility; history says
  advisory rules lose to deadlines.

### D-ARCH-3 — The netstack's long-term home

- **(a) Kernel indefinitely.** The §4.2 status quo,
  extended forever; every new consumer negotiates
  ad-hoc arbitration (D-C6L-4 is the first).
- **(b) Split at the transport.** Driver + IP stay in
  kernel; transports gain a real socket surface shared
  by the httpd move (D-ARCH-1b) and any second
  consumer; connection ownership becomes a §4.7-style
  arbiter problem. *Recommended* — it is the smallest
  step that ends the single-consumer era cleanly.
  Forecloses: a future `netd` daemon becomes unlikely
  (accepted: at this scale it buys little).
- **(c) Full netd daemon later.** Mach-orthodox;
  costs a frame-crossing boundary on the only network
  and a scheduling dependency for all networking.
  Not recommended at this device's scale.

### D-ARCH-4 — Userspace IPC: build it or bury it

Syscalls 10–12 and 23 have been "reserved for IPC"
since Fase B, and every service since has routed
around their absence with shared kernel state (§1).

- **(a) Build a minimal IPC** (synchronous message
  copy between programs). Unblocks: orthodox server
  decomposition (an FS server, a netd) becomes
  *possible*. Costs a phase-class effort plus
  re-opening scheduling questions the threading
  research just settled (blocking semantics vs R9).
- **(b) Formally abandon general IPC**; per-service
  syscall surfaces are the house pattern (the pattern
  that has actually shipped: display, cred, lfs,
  power). *Recommended.* Unblocks: plans stop waiting
  for a mechanism that is not coming. Forecloses:
  general server decomposition — a future reversal
  remains possible but would be a deliberate project,
  not an accident.

---

*Companion documents: `docs/SYSCALL-ABI.md` (what the
crossings are), `docs/riset-threading-compute/
RINGKASAN.md` §1 (why the model is what it is),
`docs/riset-fondasi-tanpa-board/RINGKASAN.md` (the
audit this document's §4.1 hardening rule feeds).*
