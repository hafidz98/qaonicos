# Research: Threading Model + Heterogeneous Compute

Date: 2026-10-23. Scope: RESEARCH ONLY. Three questions:
(1) is the cooperative threading model the right foundation,
and what would changing it cost; (2) what are the RV1103's
on-chip extra processors (RISC-V MCU, NPU, NEON/FPU) actually
worth to QaonicOS; (3) should the ESP32-C6 be used as a
general co-processor when idle.

Evidence levels used inline: **[code]** verified in the
tree, **[DS]** RV1103 Datasheet V1.4 (local PDF), **[TRM]**
RV1106 TRM via `docs/riset-power-battery/RINGKASAN.md`,
**[SDK]** Rockchip SDK / rkbin material in `~/workspace/
b0-sdk`, **[web]** vendor web sources, **[inf]** inference
from the above.

## Verdicts in one table

| Strand | Verdict |
|---|---|
| Threading | Cooperative + FND-3 watchdog stays the shipping model. Quantum preemption at user-return boundaries is feasible and bounded — adopt later, gated on a measured need. Full Mach-style preemption: reject. |
| RV1103 HPMCU (RISC-V) | Real, already in our boot chain, camera-shaped, no local materials to program it. Park behind a hardware evaluation. |
| RV1103 NPU 0.5 TOPS | Reject for a bare-metal Mach 3 OS: closed stack, no register documentation, no QEMU model, no workload. |
| NEON / VFP on the A7 | Enabled in hardware since M3, never used by any compiled code, and unsafe to enable today (no FP state in the context switch). Adopt: this is the cheapest real compute win in the project. |
| ESP32-C6 as co-processor | Fails the bandwidth law for general compute (and the C6 has **no FPU**). Wins only where keys must reside, and in the LP-core always-on tier. The protocol shape is recorded; execution belongs to the FW track. |

## 1. Strand 1 — Threading model

### 1.1 What runs today [code]

- All user execution rides **the boot thread**. Daemons
  (face, uiapp, qabotd, sh, utcpkeep; `NDAEMON` = 6 slots
  including the spawn slot) are switched by hand in
  `kernel/kernel/arm/user.c`: `sched_yield_switch()` runs
  only from the `SYS_YIELD` handler and
  `sched_exit_switch()` only from the exit paths. A switch
  saves a hand-rolled `struct user_ctx` (r0–r12, banked
  SP/LR USR, pc, spsr), swaps the pmap, and rewrites the
  trap frame so the boot thread "returns" into the next
  daemon.
- `net_pump()` — the entire network stack, including the
  HTTP server — has exactly **two** call sites in the
  kernel: `sched_yield_switch()` and `sched_exit_switch()`
  (user.c:1566, 1665). Nothing pumps the network on a
  timer, from IRQ, or from any other syscall return.
- One-shots run to completion: at boot, sequentially,
  before the daemon loop starts; at runtime via the spawn
  slot, whose parent waits (`SYS_SPAWN`/`SYS_SPAWN_WAIT`).
- The syscall ABI is deliberately **poll-shaped**:
  `SYS_TCP_RECV` returns 0 = "try again", `SYS_AUDIO_READ`
  0 = "nothing yet", `SYS_TCP_STATUS` exists so callers can
  poll. There are no blocking syscalls. This is why
  cooperative scheduling works as well as it does: every
  consumer is already written as a poll loop that yields.

### 1.2 Mapping onto Mach 3: kept vs bypassed [code]

- Kept: **VM**. `setup_uprog_task()` creates a real Mach
  `task_create` + `thread_create` per program; each
  program owns a Mach task and pmap, and memory isolation
  is genuinely Mach's.
- Bypassed: **scheduling and the thread object itself**.
  The `thread_t` returned by `thread_create` is a local
  variable — created, never stored, never scheduled. The
  Mach scheduler (`thread_block`/dispatch), Mach IPC ports
  for userspace, and per-thread PCB contexts play no part
  in the product path; product "IPC" is shared kernel
  globals behind syscalls (foundation audit #12: userspace
  IPC syscalls 10–12, 23 are reserved and unused).
- The port anticipated this: the AST check point exists in
  `trap.c` (`thread_exception_return()`: "M6: AST check
  point for user trap return") but is an empty stub, and
  `need_ast` is only ever set by `sched_test.c`. The M6
  report (`~/workspace/mach3-port/docs/M6-REPORT.md`)
  records timer-driven AST preemption as deferred future
  work with a four-item requirement list (per-thread trap
  frames; timer IRQ sets AST; trap return checks it;
  `ast_taken()` preempts via `thread_block`).

### 1.3 Can a user program have more than one thread?

No [code]. There is no thread syscall at all — not even a
reserved number (the next free number is 108, already
earmarked for `SYS_POWER` by the power research — an ABI
land-grab to avoid). Each program is exactly one Mach
thread, driven as in §1.2. What SYS-level threads would
require: new syscalls (create/exit/join, numbers ≥ 109);
per-thread `user_ctx` and stacks allocated from the task's
existing pmap; `pick_next()` generalised from daemons to
(daemon × thread); FP state per thread (§2.4 is a hard
prerequisite — an involuntary *or* voluntary switch today
would corrupt FP state); ulib thread support; exit
semantics (today `SYS_EXIT` ends the program).

First credible consumers, none blocking today: linkd
full-duplex link I/O (C6L-5 — currently expressible with
the poll ABI), an audio-capture thread with deadline
isolation, an LVGL render thread (UI track). **Park until
one of them names it in its design.**

### 1.4 Concrete costs of the cooperative model

Beyond the known hang risk (FND-3's brief), three costs
are structural:

- **GUI latency is convention, not mechanism.** The face
  targets 33 ms frames (`FRAME_MS` in `user/face/main.c`,
  `user/uiapp/main.c`). Every daemon yields per loop
  iteration by design contract, so steady-state jitter is
  bounded by the longest single non-yielding stretch in
  any daemon — a qabotd provider step, a JSON parse, a
  face sphere render. Nothing enforces the contract; any
  future loop that forgets to yield freezes GUI *and*
  network together, silently. FND-3 detects this class;
  it cannot prevent it.
- **Networking inherits scheduling latency.** While all
  daemons are inside compute stretches, inbound packets
  wait in the virtio ring because `net_pump()` only runs
  at switch points (§1.1). A runtime-spawned long one-shot
  is the worst case: children are not written to yield,
  so HTTP stalls for the child's entire runtime unless it
  happens to exit. (The mechanism would tolerate a
  yielding child — the spawn slot is a daemon-table slot —
  but the one-shot convention doesn't use it.)
- **Audio deadlines are consumer-driven.** `audio_poll()`
  (I2S FIFO → 8 KB ring) is called only from inside the
  `SYS_AUDIO_READ` handler (`kernel/kernel/arm/audio.c`).
  The ring holds 256 ms at 16 kHz/16-bit, but it only
  buffers what a read call already drained; on hardware
  the true deadline is the I2S hardware FIFO depth, which
  is milliseconds-class. A daemon that is late reading
  loses samples at the FIFO, before the ring can help.
  The designed relief is PL330 DMA (D2b stage 2,
  hardware), not the scheduler. In QEMU the registers are
  inert, so this deadline is **invisible in emulation** —
  a blind spot to remember when H4 plans its tests.

### 1.5 Preemption options

| Option | Mechanism | What breaks / risks | Effort | QEMU-testable |
|---|---|---|---|---|
| A. Cooperative + watchdog (status quo + FND-3) | — | Latency jitter remains convention-bounded | done / in flight | yes |
| B. Quantum preemption at user-return boundaries | Timer IRQ sets an AST flag; when a trap returns *toward USR mode* with AST set, run the existing ctx switch instead of returning. AST check point already stubbed in `trap.c`; switch keys on `sched_cur` (the daemon), never on the interrupted frame's mode | Mid-SVC preemption is excluded by construction (an IRQ taken inside a syscall returns to SVC, AST stays pending until the syscall completes) — this is exactly the save/restore-by-frame-mode bug class the archived kernel hit in its Fase 10, avoided by keying on thread identity. Display strip flush is a single syscall → completes atomically; token hand-off between strips can interleave two programs' strips (already possible via event timing today). Requires §2.4 FP state first if FP codegen is ever enabled. Kernel subsystems stay non-reentrant-safe because switches still only happen at clean user boundaries | M | fully (spin-hog sliced; battery + soak) |
| C. Full Mach preemption | M6 list: per-thread trap frames, `ast_taken()` + `thread_block` continuations, Mach scheduler drives user threads | Replaces the layer's ctx model; mid-syscall preemption meets non-reentrant net/blk/littlefs state — the corruption surface is the whole kernel, not the switch | L+ | partially (soak would be the only real evidence) |
| D. Blocking-syscall ABI | Wait queues in kernel; poll ABI rewritten as blocking | Every consumer rewritten; the poll ABI already expresses the same waits; C6L-2 already specs client-side timeouts (2 s) instead | L | yes, pointlessly |

### 1.6 Strand 1 verdict

Stay cooperative + FND-3 as the shipping model — with
four daemons and a poll-shaped ABI on one core, it is the
right-sized design, and FND-3's design (heartbeat from
normal syscall/yield activity, escalation via FND-2
reclaim and FND-1 reset) already assumes it. Adopt option
B as a **later, gated** phase (THR-3): it buys smoothness
(frame cadence during another daemon's compute stretch,
net-pump fairness), not survival, so it should be
triggered by a measurement — LVGL animation jitter (UI
track) or TLS-in-qabotd latency — not by taste. Reject C
and D. Park multi-threaded user programs (§1.3).

## 2. Strand 2 — On-chip heterogeneous assets

First, a taxonomy correction the rest of this section
depends on: the RV1103 contains **two different RISC-V
blocks**. The **HPMCU** sits in VD_CORE next to the A7
[TRM via power research §1.1]; the **LPMCU** sits in
VD_PMU and exists to run the PMU's software sleep
sequence [TRM §7.6.2]. They are not the same core. The
old research's "RISC-V MCU @ 400 MHz" is the HPMCU (the
400 MHz figure is [web] — the V1.4 datasheet's feature
list does not state an MCU clock).

### 2.1 HPMCU — the fast-boot camera core

- **What it is for** [SDK]: every RV1106 HPMCU image in
  rkbin is named after a camera sensor (`..._gc2093_...`,
  `..._sc230ai_...`, `..._sc3338_...`). The vendor pattern
  is fast boot for IP cameras: the MCU boots the sensor
  and ISP and produces first frames while Linux on the A7
  is still loading. RV1103B ships two generic images:
  `rv1103b_hpmcu_tb_v1.00.bin` (test board) and
  `rv1103b_hpmcu_wrap_v2.02.bin` (TEE-packaged variant).
- **It is already in our boot chain** [SDK]: the ini our
  B0 idbloader is built from (`RV1103BMINIALL_SPI_NAND_
  CONT_TB.ini`) declares `LOADER3=Hpmcu` with the TB
  image. So the vendor test-board MCU firmware is part of
  the QaonicOS boot package today [inf: whether the
  packed image actually starts it, and what it then owns,
  is unverified — see THR-4]. An unexamined second core
  in the shipping boot chain is a finding in itself.
- **Memory + IPC** [DS §1.2.3]: the TOS wrap ini loads
  the HPMCU image at `0x210f6000` — inside the system
  SRAM window (256 KB system SRAM). The datasheet gives
  the MCU 16 KB of cache, its own interrupt controller
  (lines also routed to the GIC), and JTAG debug pins
  (muxed with SDMMC/UART5/UART2 options). CPU↔MCU IPC is
  a hardware **mailbox**: four elements, each one data
  word + one command word + a flag bit that can raise an
  interrupt, plus 32 lock registers. No mailbox *protocol*
  (message semantics) is documented in our materials.
- **Can user code run on it?** In principle [web]: the
  upstream Luckfox SDK carries an RT-Thread tree for the
  MCU (`sysdrv/source/mcu/rt-thread`, noted by the 2026-09
  Mach research) — Rockchip's model is "the MCU runs
  Rockchip's RT-Thread firmware", customisable at SDK
  level for camera products. In practice, **not from
  anything we hold**: the local clone's `sysdrv/source/`
  is empty (not checked out), we have no MCU toolchain
  setup, no mailbox register map work, and the firmware's
  contract with the boot chain (who loads it, when the
  A7 may reset/release it) is only visible in ini files.
- **Verdict: park.** Not vendor-locked in principle, but
  zero local materials, a camera-shaped vendor firmware,
  and no product need the A7 or C6 don't cover — our
  fast-boot need is an early splash, which the kernel
  already draws itself. THR-4 (hardware) first establishes
  ground truth: is it running under our boot, and what
  does it touch?

### 2.2 LPMCU — not a compute resource

Covered so this doc and the power research agree: the
LPMCU is the PMU's own sequencer for the §7.6.2 software
sleep path. It is not programmable capacity for the OS in
any product sense, and the power track's open question
Q2 stands (no rv1103b PM/suspend blob exists in rkbin,
which hints the pure-FSM §7.6.1 path suffices). Nothing
in this strand changes PWR-3's plan.

### 2.3 NPU 0.5 TOPS — reject

[DS §1.1]: INT4/INT8/INT16 hybrid, "up to 0.5 TOPS".
What using it actually requires [web/SDK]: PC-side model
conversion with the closed RKNN-Toolkit2, the closed
`librknnrt` userspace runtime, and a kernel NPU driver —
in the BSP, a Linux driver for a register block Rockchip
does not document at register level in any material we
hold (the datasheet gives the marketing line only).

On a bare-metal Mach 3 OS that means: write the register
driver from zero documentation or reverse-engineer the
Linux driver's submission sequences, reimplement the
runtime's command/memory management, and accept closed
conversion tooling — weeks to months, **unverifiable in
QEMU** (virt has no such device), in exchange for
accelerating a workload we do not have: the product's
inference story is cloud LLM (Modes A/B) with, someday,
tiny on-device STT — for which an integer model on the
A7+NEON is the proportionate tool (§2.4). **Reject.**
Revisit only if a real vision requirement appears, and
even then a Linux co-boot is the sane vehicle, not a
Mach driver.

### 2.4 NEON / FPU — enabled, unused, and currently unsafe

The single most actionable finding in this strand:

- The A7 integrates NEON + FPU [DS §1.1]. `fpu_init()`
  has enabled them at boot since M3 (CPACR CP10/CP11 full
  access, FPEXC enable) [code: `kernel/kernel/arm/
  fpu.c`]. Directly below, `fpu_save()` / `fpu_restore()`
  are **empty stubs** — "no per-thread state yet".
- No build line asks for FP codegen [code:
  `kernel/build-md.sh` — kernel, user and TLS flags are
  all `-march=armv7-a` with no `-mfpu` and no
  `-mfloat-abi`, i.e. clang's soft-float ABI default]:
  any C `float`/`double` compiles to `__aeabi_*` library
  calls (the kernel carries `aeabi.c`, the soft-float
  runtime). All product rendering/audio math is integer
  by design, so in practice **no VFP/NEON instruction
  executes anywhere in the product today** [inf from
  flags + code survey].
- The trap: `user_ctx` (§1.1) has no FP state. The day
  anyone compiles one program with `-mfpu`, two daemons
  using FP registers will silently corrupt each other at
  every switch. The hardware has been powered and enabled
  the whole time; only the context switch stands between
  us and using it — and it is currently a loaded footgun.

What adopting it takes (THR-1, all QEMU-testable):
(1) grow `user_ctx` with the VFP/NEON register file
(d0–d31 = 256 B + FPSCR) and save/restore it in
`ctx_save`/`ctx_load` — eager, not lazy: at a 33 ms
switch cadence the cost is noise, and eager is the
simple, auditable semantics; implement the existing
`fpu_save`/`fpu_restore` stubs as the mechanism.
(2) Per-program flags: `-mfpu=neon-vfpv4
-mfloat-abi=softfp` for the programs that want it (face
first, then audio tools). `softfp` deliberately keeps
the existing calling convention — no ulib ABI upheaval —
while letting local math compile to VFP/NEON. The kernel
itself stays soft-float and never touches FP registers.
(3) An isolation one-shot: two FP-using programs
interleaved through yields, each verifying its registers
survive — the FP analogue of the existing sched
self-test.

Where the wins show: face sphere/waveform math (trig and
per-pixel shading, today integer approximations), audio
gain/mix and any future MFCC, LVGL (the UI track's code
uses floats internally), JSON float handling in qabotd.
Where they don't: TLS and the B5 crypto are integer
bignum work — NEON buys them nothing, and this doc does
not pretend otherwise.

### 2.5 The PL330 precedent [code]

The project's one working piece of "extra silicon" is
the PL330 DMA (D2b): a microcode builder + structure
verification in QEMU (`UPL330: PASS`), with the actual
DMAGO start reserved for hardware (stage 2). Note its
shape: **fixed-function, narrow, programmed by a tiny
verifiable artefact, no shared-memory coherence story**.
Every successful offload in this project so far has that
shape; §3 judges the C6 against the same bar.

## 3. Strand 3 — ESP32-C6 as co-processor

F5 scoped the C6 to connectivity (Wi-Fi/BLE backhaul,
KV/FIDO store, Muse Gadget role). This strand assesses
the question F5 never asked: general compute offload
from the RV1103 to the C6 "when it is idle".

### 3.1 Compute inventory [web — Espressif primary]

- HP core: 32-bit RISC-V, up to 160 MHz, 512 KB SRAM,
  320 KB ROM. LP core: 32-bit RISC-V, up to 20 MHz,
  16 KB LP SRAM — the always-on tier (LP peripherals,
  wake sources, sensor watch while the HP system sleeps).
- **The C6 has no FPU.** Espressif's own FPU survey
  (developer.espressif.com, "Floating-Point Units on
  Espressif SoCs") lists FPU-equipped cores as ESP32,
  ESP32-S3, ESP32-H4, ESP32-P4 — the C6 row is blank.
  Float on the C6 is software, like ours today — except
  the A7 has a real NEON/FPU waiting (§2.4) and the C6
  never will.
- No vector/SIMD/DSP extensions (contrast ESP32-S3's AI
  instructions and ESP32-P4's DSP + FPU). 
- HW accelerators: AES-128/256 (with DMA), SHA family,
  RSA, ECC, HMAC, a digital-signature peripheral (keys
  generated and used inside the peripheral, unexportable),
  TRNG; secure boot + flash encryption.
- No PSRAM on standard modules (F5 finding stands):
  working memory is the 512 KB SRAM, shared with the
  Wi-Fi/BLE stacks.

So the C6 is a 160 MHz integer RISC-V with excellent
crypto peripherals and radios — a *connectivity and
security* island. As a number cruncher it is strictly
weaker than the 1.2 GHz A7 it would be assisting, before
transport is even considered.

### 3.2 The bandwidth law

The link is UART4 at 921600 baud [F5 §7: supported by
the RV1103, RTS/CTS advised] = 92,160 B/s raw at 8N1.
After framing (C6L-1's frozen frame) and request/response
turnaround, ~80 KB/s is the honest planning figure for
bulk movement. Every offload candidate is therefore
judged on **compute per byte moved**, with the transfer
time as a hard floor:

| Candidate | Bytes moved | Transport floor | Compute saved on A7 | Verdict |
|---|---|---|---|---|
| Bulk AES-GCM (one 16 KB TLS record) | 16 KB in + 16 KB out | ~356 ms round trip | ~2 ms (software AES-GCM at A7 speeds; C6 HW AES itself is <1 ms — transport is 99% of the cost) | **Reject** — ~100× slower end-to-end |
| TLS handshake pieces (ECDHE etc.) | ~1–2 KB total | ~20 ms | ~100–300 ms of bignum | Marginal win, large complexity, key-placement questions — **reject** in favour of F5's full TLS termination on the C6, which is a topology, not an offload |
| RSA-2048 / ECC sign with a **C6-resident key** | ~40 B in, ≤256 B out | ~3 ms | ~0.1–2 s class (A7 software private-key ops) | **Wins on speed** — but see §3.6: the real reason to do this is key residency, not speed. No current feature needs device-side signing (update verify is public-key; the signing key is offline by B5 rule) |
| Audio feature extraction (MFCC) on the 16 kHz stream | 32,000 B/s **continuously in** (~35% of raw line, ~40% of planning figure) | permanent | a few % of one A7 core | **Reject** — the input *is* the cost; the A7 is already where the audio is |
| Wake-word inference on the C6 | same 32 KB/s stream, forever | permanent | WakeNet9s exists for the C6 [web: ESP-SR docs — depthwise-separable cost-down model, official C6 support] | **Wrong shape for this board** — see below |
| JSON / protocol parsing | ≤ a few KB | ≥11 ms per KB each way | sub-ms per KB | **Reject** by ≥10× |
| Anything image-sized | 115,200 B (one 240×240 RGB565 frame) to ~10 MB (one ISP frame) | 1.25 s to ~2 min | — | **Reject** by 2+ orders of magnitude; vision stays on the RV1103 |

The wake-word row deserves its sentence: WakeNet on the
C6 is a real, supported Espressif feature — for products
whose microphone is wired to the C6. In our topology the
mic is on the RV1103 codec (F3/F5 decisions), so the C6
could only listen if the RV1103 captures and streams
32 KB/s to it forever: the A7 and codec stay awake to
feed it, the link is permanently a third full, and the
power saving that justified an always-on listener is
gone. Wake-word belongs on the RV1103 (a small integer
model on the A7, §2.4 applies) — or stays in the cloud
per Modes A/B.

### 3.3 What does make sense on the C6

Three classes pass the bar of §2.5 (narrow, verifiable,
residency- or power-shaped rather than throughput-shaped):

1. **What is already there by design.** TLS termination
   (F5), the Wi-Fi/BLE stacks, KV/FIDO storage — the v1
   protocol already makes the C6 a *key-resident* peer:
   FIDO private keys never cross the UART by design.
   That is the template: the C6's value is what it *holds*
   and what it *radios*, not what it computes.
2. **Key-resident operations, if a feature ever needs
   them** (e.g. device attestation signing): tiny
   payloads, huge compute/byte, and the DS peripheral
   keeps keys unexportable. Pattern recorded; no phase
   until a feature names it.
3. **The LP-core always-on tier** — the honest reading of
   "use the C6 while it is idle": jobs whose currency is
   microamps, not bytes/second. Link keepalive and peer
   watchdog, C6-side ADC sampling (the power research
   §3.3 noted the C6's ADCs and rejected C6-as-gauge as
   *primary* because it couples sensing to link health —
   as an LP-tier auxiliary it remains available), BLE
   wake watch, wake-source aggregation. These ride the
   existing EVT/KV-class traffic; they need **no** job
   machinery.

### 3.4 Protocol shape for outbound jobs (recorded, not scheduled)

If a JOB class ever ships, it should mirror C6L-3's
inbound design rather than invent a second idiom: a JOB
frame in the frozen frame format (job_id u16, op name,
args as chunked BIN where needed — the NET.HTTP chunking
is the precedent), completion as an EVT frame
(JOB.DONE / JOB.ERR carrying the job_id), JOB.CANCEL by
id, at most one job in flight per class (the link is
effectively half-duplex; pretending otherwise buys
nothing). Placement: a **C6L-3 addendum or a C6L-6** —
C6L-3 itself stays inbound-only as planned. The QEMU
mock peer (C6L-2's `mock-c6.py`) can execute jobs
immediately, so the protocol half is board-less; a real
executor exists only in the deferred FW track. Proposing
this shape is all this research does — it does not
replan C6L.

### 3.5 The power conflict

Co-processing and the power story pull in opposite
directions: a job wakes (or keeps awake) the C6 HP core
and the UART, i.e. it spends exactly the budget PWR-4's
sleep choreography exists to save, and the C6's sleep
states are owned by that choreography, not by the link's
convenience. The shared constraint, stated once for both
tracks: **a compute job must take a power lease** (the
same shape as the display token), and the rule for
granting it is — offload only when (a) the C6 is already
awake for connectivity, or (b) the job is security-bound
(key residency, §3.3.2), never merely speed-bound; LP-tier
jobs (§3.3.3) are exempt because they *are* the sleep
state working as intended.

### 3.6 Trust boundary [PLAN-B5 threat model]

B5's T3 is credentials read out of the NAND; B5.5's
answer keeps the credstore and its device key **on the
RV1103**, wrapped on-device. The line for offload follows
directly:

- **May live on / be done by the C6:** TLS session keys
  for C6-terminated connections (F5 design — the session
  is the C6's own), Wi-Fi/BLE link credentials it needs
  to operate (KV per the v1 design), FIDO private keys
  (v1 design: they never cross), its own firmware trust
  (secure boot / flash encryption).
- **Must never leave the RV1103:** the device PIN and
  anything derived from it, the credstore device key and
  every unwrap operation, API keys at rest (an API key in
  use by a C6-terminated TLS session is *in use*, not at
  rest — that distinction is F5's, and it is why session
  residency above is acceptable), and update-verification
  authority (the RSA public key and the verify decision
  stay on the RV1103; the C6 is transport for packages,
  never a trust anchor for them).

A general JOB channel (§3.4) must encode this list as an
allowlist of ops, not discover it later.

### 3.7 "If the board supports it" — what actually gates this

The owner's caveat resolves precisely: **the Pico board
itself gates nothing.** The C6 is an external module on
the Pico Mini header's UART4 (F5: UART4 is the exposed
link UART; UART2 is the debug console). Support hinges
on: (1) the UART4 link itself at 921600 with RTS/CTS —
an RV1103 capability [DS: UART to 4 Mbps], not a board
feature; (2) two header pins for flow control being
free in the final wiring; (3) **our own C6 firmware**
(the deferred FW track) implementing whatever the peer
must do — every strand-3 capability beyond F5's
connectivity scope is firmware scope, not board scope;
(4) module variant choice (flash size; no PSRAM is
needed or available). QEMU-side, nothing gates the
protocol work at all (mock peer). So: "if the board
supports it" = "if the wiring and our firmware do" —
the board already does its part.

## 4. Combined recommendation

**Adopt (in order):**
1. **THR-1 — NEON/FPU for user programs** (§2.4). The
   only strand with a large, certain, board-less win.
2. **THR-2 — cooperative-model hardening** (§1.4): a
   written yield contract, a net-pump audit for spawned
   one-shots, and client-side timeouts where the C6L
   plan already asks for them. Small, and it de-risks
   FND-3's assumptions.
3. **The C6 rules** (§3.5, §3.6) written into the C6L/FW
   documents when those tracks next move: power lease +
   trust allowlist. No code.

**Adopt later, gated:**
4. **THR-3 — quantum preemption at user-return
   boundaries** (§1.5 option B), triggered by a measured
   jitter problem (LVGL UI track is the likely source).

**Park:**
5. Multi-threaded user programs (§1.3) — until linkd,
   audio, or LVGL names a concrete second thread.
6. HPMCU custom use (§2.1) — behind THR-4 ground truth
   on hardware.
7. C6 JOB offload (§3.4) — protocol shape recorded;
   execution waits for the FW track to exist.

**Reject:**
8. NPU via the RKNN stack on bare metal (§2.3).
9. General-purpose C6 compute offload (§3.2) — the
   bandwidth law and the missing FPU settle it.
10. Full Mach-style preemption and a blocking-syscall
    ABI rewrite (§1.5 options C, D).

## 5. Proposed phases

- **THR-1 — FPU/NEON enablement** *(board-less)*:
  FP state in `user_ctx`; `fpu_save`/`fpu_restore`
  implemented; per-program `softfp` flags (face first);
  FP isolation one-shot; before/after frame-time numbers
  for the face sphere/waveform in QEMU.
- **THR-2 — Cooperative hardening** *(board-less)*:
  yield contract documented in SYSCALL-ABI; net-pump
  behaviour for spawned one-shots audited and fixed or
  explicitly accepted; audit of every daemon loop for
  non-yielding stretches, filed against FND-3's heartbeat
  table.
- **THR-3 — Quantum preemption (gated)** *(board-less)*:
  §1.5 option B. Entry condition: a recorded jitter
  measurement that option A cannot meet. Exit: spin-hog
  slicing test + full battery + soak, all green.
- **THR-4 — HPMCU ground truth** *(hardware)*: with the
  board: is the TB firmware running under our boot
  chain, what does it own (mailbox, SRAM window, sensor
  lines), does a NOMCU boot change anything we ship?
  Output: a use-it-or-remove-it decision for the boot
  package, feeding B0.5's open questions.
- **THR-5 — C6 JOB protocol** *(board-less protocol,
  hardware execution)*: §3.4 shape implemented against
  `mock-c6.py` after C6L-3; real executor only inside
  the FW track; LP-tier always-on jobs specified jointly
  with PWR-4.

## 6. Open questions

1. Does the packed QaonicOS idbloader actually start
   the HPMCU TB firmware, and if so what does that
   firmware do with the mailbox/SRAM while our kernel
   runs? (THR-4; also a B0.5 open question now.)
2. The HPMCU's mailbox register map and boot/release
   registers need the RV1103 TRM's system chapters —
   our TRM excerpts cover the PMU only. Where does the
   "400 MHz" figure come from, and is it the HPMCU's
   clock at all?
3. When LVGL lands (UI track), is the 33 ms cooperative
   cadence smooth enough for its animations, or does
   THR-3 become a prerequisite? Measurable in QEMU once
   UI-2 exists.
4. Eager vs lazy FP state: this doc recommends eager
   (256 B × slots, cadence makes it free). If daemon
   count grows an order of magnitude, does that still
   hold? (Revisit at THR-1 implementation, not before.)
5. Does qabotd's TLS move behind C6 TLS termination on
   hardware (F5's direction)? If yes, strand 1's TLS
   latency cost mostly evaporates on the board and
   remains a QEMU-only property — the THR-3 trigger
   measurement must say which world it measured.

## Sources

Local: `kernel/kernel/arm/user.c`, `netmain.c`,
`trap.c`, `fpu.c`, `audio.c`; `kernel/build-md.sh`;
`docs/MIGRASI-MACH3.md`; `~/workspace/mach3-port/docs/
M6-REPORT.md` + `RELEASE.md`; `docs/rencana-fondasi-
tanpa-board/PLAN.md` (FND-2/FND-3); `docs/riset-
fondasi-tanpa-board/RINGKASAN.md` (#12); `docs/riset-
esp32c6-companion/RINGKASAN.md` (F5, §4/§7);
`docs/rencana-komunikasi-c6/PLAN.md` (D-C6L-3, C6L-2/3);
`docs/riset-power-battery/RINGKASAN.md` (§1.1, §3.3,
§4, PWR-4); `docs/rencana-hardening/PLAN-B5.md` (T3);
`docs/riset-mach-kernel-luckfox/laporan-riset-mach-
kernel-luckfox.md`; RV1103 Datasheet V1.4 (§1.1, §1.2.3,
pin-mux tables); `~/workspace/b0-sdk/rkbin` (RKBOOT
RV1103B inis, RKTRUST/RV1103BTOS_MCU_WRAP.ini, bin/rv11
HPMCU images).
Web (Espressif primary): ESP32-C6 product brief (HP/LP
cores, SRAM, crypto accelerators); ESP-SR documentation
(WakeNet9s targets include ESP32-C6); developer.
espressif.com, "Floating-Point Units on Espressif SoCs"
(FPU table — C6 absent).
