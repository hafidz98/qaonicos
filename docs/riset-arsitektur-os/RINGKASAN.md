# Research: OS architecture patterns vs QaonicOS

Date: 2026-10-23. Commission: compare the common operating-
system architecture patterns against QaonicOS's constraints
and as-built reality, name precisely what QaonicOS's
architecture *is* today, and state the smallest set of
architectural commitments it should converge to — so that
development has a direction instead of a series of locally
reasonable decisions.

Companions: `docs/ARSITEKTUR-SISTEM.md` (the master
architecture document this research feeds),
`docs/ARSITEKTUR.md` (kernel/userspace placement rules),
`docs/SYSCALL-ABI.md` (the interface contract),
`docs/riset-mach-kernel-luckfox/` (why Mach, and what the
port actually is), `docs/riset-soc-rv1103/RINGKASAN.md`
(what the silicon offers), `docs/riset-threading-compute/
RINGKASAN.md` (the scheduling verdict this doc builds on).

## 0. The constraints any pattern must survive

These are not negotiable inputs; a pattern that needs one
of them relaxed fails by definition.

| # | Constraint | Source |
|---|---|---|
| C1 | The kernel is the **real Mach 3** (CMU), owner-mandated; the from-scratch Fase 1–17 kernel is archive | owner, 2026-09-28 |
| C2 | Single-core Cortex-A7 (1.104 GHz per vendor DTS), 64 MB DDR2, 128 MB SPI NAND | SoC inventory |
| C3 | Daemon scheduling is **cooperative**; preemption is gated on a measured jitter need | threading research |
| C4 | Userspace programs are **embedded in the kernel image**; ~287 KB headroom left in the 4 MB boot partition | UI research (measured) |
| C5 | Networking on hardware terminates at an **ESP32-C6** companion MCU over UART4 @ 921600; the guest TCP stack is a dev/QEMU surface | F5 + foundation docs |
| C6 | The product is an **appliance**: plug-and-play out of the box — 100/100 unattended cold boots, power-cut-safe FS, failed update falls back, 3 crashes → recovery, never a reset-loop brick | out-of-box research |
| C7 | Everything must be **verifiable in QEMU** before hardware exists; hardware-only code hides behind a seam with a simulated source | standing workflow |
| C8 | One physical button-class input device, a 240×240 SPI display, no keyboard; all configuration arrives from a phone | ONB decisions D-ONB-1..7 |

## 1. The patterns, one by one

### 1.1 Pure microkernel — Mach (as designed), seL4, QNX Neutrino

- **Service model.** The kernel provides address spaces,
  threads, IPC and little else; filesystems, network
  stacks, drivers and policy live in userspace servers,
  composed per system.
- **IPC cost model.** Every service crossing is a message
  pass: two context switches plus copies (or page
  remapping) per request. Mach's historical lesson is that
  this is affordable only when IPC is *the* fast path and
  servers are few and coarse; seL4 shows a verified kernel
  can make IPC genuinely fast, QNX shows message passing
  can carry a commercial driver ecosystem.
- **Fault isolation.** The strongest story of any
  pattern: a crashed server restarts without the kernel
  or its peers dying; the TCB is small enough to audit
  (seL4: formally verify).
- **Driver model.** Userspace drivers, interrupt as IPC
  notification.
- **Footprint.** Small kernel; total system size depends
  on how many servers you actually run — the pattern
  itself doesn't force bloat, but a general-purpose
  server set (POSIX personality, full FS + net) is large.
- **Fit against QaonicOS.** The *kernel* fits — it is
  literally Mach 3, and C2's single core plus C3's
  cooperative model remove the two classic microkernel
  taxes (SMP locking, IPC storms under preemption). The
  *service model* does **not** fit as built: the port's
  Mach threads are created per program and never
  scheduled, no service is reached over Mach IPC, and the
  syscall numbers reserved for IPC (10–12, 23) are
  unused. Adopting the textbook model now would mean
  building a server framework, a message ABI and a driver
  split against C4's 287 KB headroom, to serve four
  daemons that currently talk to the kernel through a
  flat syscall table. **Verdict: keep the kernel, do not
  adopt the service model wholesale.** Borrow two ideas
  only: (a) *one owner per resource*, enforced by the
  kernel (already the display-token/console-owner shape);
  (b) kernel minimalism as an *admission* pressure, not
  as a description of today (see ARSITEKTUR.md).

### 1.2 Hybrid — XNU (Mach + BSD + I/O Kit in one kernel)

- **Service model.** A microkernel substrate with a
  monolithic personality *inside* the same address space:
  BSD syscalls, filesystems and drivers run in-kernel for
  speed; Mach IPC remains available for what wants it.
- **IPC cost model.** Dual: in-kernel calls are function
  calls (free), cross-process services still pay IPC.
- **Fault isolation.** Weak inside the kernel boundary —
  a driver bug is a kernel panic — strong between user
  processes.
- **Driver model.** In-kernel driver framework (I/O Kit),
  object-oriented, dynamically matched.
- **Footprint.** Large; assumes a general-purpose machine.
- **Fit against QaonicOS.** **This is the closest
  textbook relative of what QaonicOS actually is**: a
  Mach substrate plus an in-kernel personality that does
  the product's real work (in our case the extension
  layer in `kernel/kernel/arm/`: blk/net/HTTP/littlefs/
  bootstate/credstore), with userspace reached through a
  syscall surface rather than IPC. XNU's history also
  carries the warning: hybrids drift monolithic, because
  every performance or convenience argument pulls code
  across the boundary and nothing pushes back. QaonicOS
  already shows the drift (an HTTP server and an HTML
  dashboard inside the kernel — §3). The pattern fits;
  the discipline must be supplied deliberately, which is
  what the placement rules in ARSITEKTUR.md are for.
  **Verdict: name the architecture honestly as hybrid;
  adopt XNU's pragmatism, refuse its drift by rule.**

### 1.3 Monolithic — Linux embedded

- **Service model.** Everything in one privileged address
  space; userspace asks via syscalls; loadable modules
  blur even that line.
- **IPC cost model.** No IPC tax for services; the cost
  is paid in blast radius instead.
- **Fault isolation.** Weakest of the process-based
  patterns: any kernel/driver bug can panic the system.
  Mitigated in practice by watchdogs and reboot
  resilience — exactly the mechanisms QaonicOS's B2/FND
  tracks build (bootstate, safe mode, A/B rollback).
- **Driver model.** In-kernel drivers, huge ecosystem,
  device-tree described hardware.
- **Footprint.** Configurable down to a few MB, but the
  ecosystem's centre of gravity is far above 64 MB.
- **Fit against QaonicOS.** The extension layer already
  behaves monolithically, and for a fixed-function
  appliance on C2 hardware that is defensible: one
  address space, no IPC tax, the whole system auditable
  by one team. What does *not* transfer is the ecosystem
  assumption (drivers, subsystems, community maintenance)
  — every line of QaonicOS's kernel side is ours to
  maintain, so the monolith must stay *small on purpose*.
  Also non-transferable: Linux's recovery story assumes an
  operator; C6 demands the device recover *itself*, which
  is why the resilience machinery is kernel-resident and
  non-optional here. **Verdict: the extension layer may
  stay monolithic in style; the system as a whole must
  not become Linux-shaped (no general-purpose ambition,
  no subsystem sprawl).**

### 1.4 RTOS / executive — Zephyr, FreeRTOS

- **Service model.** The kernel is a library linked into
  one firmware image; "applications" are threads in a
  shared address space (FreeRTOS) or a configurable
  single image with optional userspace threads (Zephyr).
  Services are function calls and queues, not processes.
- **IPC cost model.** Near zero — queues and direct calls.
- **Fault isolation.** Minimal by default (shared address
  space; MPU-based isolation is opt-in and coarse).
- **Driver model.** In-image drivers behind subsystem
  APIs; devicetree/Kconfig in Zephyr's case.
- **Footprint.** The smallest of all patterns — tens of
  KB kernels are routine.
- **Fit against QaonicOS.** QaonicOS *userspace* already
  resembles an RTOS image more than a Unix: a fixed set
  of programs linked from one tree, statically embedded,
  cooperatively scheduled, talking through a narrow call
  surface, with one-shot test programs run in sequence.
  If the only goal were "ship the appliance", an RTOS
  would have been the cheaper base. It fails C1 outright
  (the owner mandated real Mach 3), and it would give up
  the two things the Mach base buys: genuine PL0 process
  isolation for daemons (a faulting daemon dies alone —
  FND-2 exists precisely because that isolation is real)
  and a path to richer programs later. **Verdict:
  rejected as the base (by C1), but its habits — static
  composition, no dynamic loading, subsystem APIs instead
  of ad-hoc globals — are the right discipline for the
  userspace side and for linkd-style single-owner daemons.**

### 1.5 Library OS / unikernel

- **Service model.** One application linked with the OS
  libraries it needs into a single-purpose image; no
  processes, no shell, no multi-tenancy.
- **IPC cost model.** None — there is nobody to talk to.
- **Fault isolation.** Irrelevant internally; the unit of
  failure is the whole image (the hypervisor/host is the
  isolation boundary).
- **Driver model.** Only the drivers the one application
  needs, linked in.
- **Footprint.** Minimal per image; scales by images, not
  by features.
- **Fit against QaonicOS.** Fails the product shape: the
  device runs a face, a settings UI, an agent daemon, a
  shell and a web UI concurrently, must be recoverable by
  a non-technical owner (C6), and must update as one
  appliance. A unikernel per function would multiply the
  boot/update story for no gain on a single core with no
  hypervisor. One unikernel-shaped idea *does* transfer:
  **the C6 firmware is effectively a unikernel** (one
  image, one job — network + keys), and treating it that
  way (small, single-purpose, replaceable as a whole) is
  the right mental model for the FW track. **Verdict:
  no for the RV1103 side; yes as the C6's design shape.**

### 1.6 Pattern comparison, condensed

| Pattern | Service model | IPC tax | Fault isolation | Driver home | Footprint | QaonicOS verdict |
|---|---|---|---|---|---|---|
| Pure microkernel (Mach/seL4/QNX) | userspace servers | high, per crossing | strongest | userspace | small kernel, servers extra | kernel yes; service model no (§1.1) |
| Hybrid (XNU) | in-kernel personality + IPC available | dual | weak in-kernel, strong between processes | in-kernel framework | large | **closest relative of the as-built system** (§1.2) |
| Monolithic (Linux emb.) | all in-kernel | none | weakest; watchdog-recovered | in-kernel | MBs, sprawl-prone | extension layer's style; not the system's ambition (§1.3) |
| RTOS/executive (Zephyr/FreeRTOS) | library + threads | ~none | minimal | in-image | smallest | fails C1; habits adopted for userspace (§1.4) |
| Library OS / unikernel | one app + OS libs | none | n/a (image = unit) | linked in | minimal per image | no on RV1103; **yes as the C6's shape** (§1.5) |

## 2. Verdict: what QaonicOS's architecture actually is

> QaonicOS is a **hybrid appliance OS**: a real Mach 3
> kernel used as a porting substrate — its VM, task and
> trap machinery gives daemons genuine PL0 isolation, but
> its IPC and thread services are *not* the system's
> service mechanism — overlaid by a monolithic in-kernel
> **extension layer** (`kernel/kernel/arm/`) that owns the
> product's shared services and safety invariants (block
> devices, the TCP/HTTP stack and web UI, littlefs,
> bootstate/bootlog resilience, the credential store,
> display and console arbitration), and fronted by a
> fixed, statically embedded set of userspace programs —
> daemons (face, uiapp, qabotd, sh) and sequential
> one-shots — that reach the kernel only through a wide,
> flat syscall ABI and are scheduled **cooperatively**,
> with the network pump driven from the scheduler's two
> switch points. In one line: XNU-shaped in structure,
> RTOS-shaped in composition, Mach by substrate and by
> owner mandate — and appliance-shaped in its non-
> negotiables (self-recovery, signed A/B updates, one
> owner per shared resource).

### 2.1 What it should converge to

The smallest commitment set that keeps C1–C8 and the
queued tracks (FND → C6L → UI, then SOC/PWR/THR) coherent.
Each commitment is already either an owner decision or a
research verdict; this doc's contribution is making them
one list.

1. **The Mach core is frozen.** Product logic enters via
   the extension layer or userspace, never by changing
   core semantics. (FND plan rule; ANALISIS-PORTING's
   MI/MD split is the reason this is cheap.)
2. **The syscall ABI is the only kernel↔user contract**,
   and it is a *contract*: documented in SYSCALL-ABI.md,
   grown by append, never silently repurposed.
3. **Cooperative scheduling stays** until a measurement —
   not a feeling — gates preemption (threading research).
   Liveness is guaranteed by supervision (FND-3
   heartbeat + escalation) and reclamation (FND-2), not
   by the scheduler being cleverer.
4. **Kernel owns arbitration and safety invariants;
   policy and presentation live in userspace** (the
   placement rules of ARSITEKTUR.md). The kernel decides
   *who may* and *what must hold*; daemons decide *what
   to do*.
5. **One owner per shared resource** — display token,
   console owner, the single TCP connection, the link
   transport (C6L-4), the credential store. Contention
   is resolved by ownership transfer, never by locking
   conventions between peers.
6. **Every subsystem is QEMU-verifiable**; hardware-only
   behaviour hides behind a TODO(HW) seam with a
   simulated source (the PWR-1 pattern, the C6 mock
   pattern, the audio draft pattern).
7. **Resilience is architecture, not a feature**: no
   boot path bricks (bootstate + safe mode), no update
   path strands (signed A/B + trial + rollback), no
   daemon death wedges (reclaim + respawn), no secret
   rests in plaintext (credstore). New subsystems inherit
   these or argue their exception in writing.
8. **On hardware, the C6 is the network boundary.** The
   guest TCP/TLS stack is a development surface; the
   product's network, and its private keys, terminate at
   the companion. (F5 verdict; threading research §3.6.)

## 3. Where trajectory and direction conflict

Flagged, not resolved — resolution belongs to the owner
questions in §4 or to the tracks named.

1. **HTTP/web UI inside the kernel.** `http.c`/`webui.c`
   parse untrusted network input in kernel context, and
   their handlers run from `net_pump()` inside scheduler
   switch points. This contradicts commitment 4 (parsing
   and presentation in userspace) and is the single
   largest placement debt in the system. It exists for an
   honest reason — there is no socket-like userspace
   surface to serve from — but the debt should be
   *named*, and C6L (which builds a link daemon owning a
   transport) is the first natural place to design the
   exit.
2. **The network pump lives in the scheduler.** `net_pump()`
   is called from exactly the two switch points in
   `user.c`; a daemon that stops yielding starves the
   network, the web UI and its own supervision heartbeat
   alike. FND-3 detects the wedge; nothing yet *unwedges*
   the pump itself. Architecturally the pump wants an
   owner that is not "whoever yielded last".
3. **Userspace size is coupled to the boot partition.**
   Embedded daemon images (C4) mean LVGL (UI track) is a
   kernel-image budget question, and every update ships
   the whole userspace whether it changed or not. The
   known exits — harder LVGL trim, load daemon images
   from littlefs — have very different architectural
   weights; UI-1 measures, but the *decision* is
   architectural, not a UI-track detail.
4. **The ABI doc lags the ABI.** SYSCALL-ABI.md is 28
   syscalls stale (foundation audit #14) while
   commitment 2 calls the ABI the contract. The FND
   track's rule (doc updated in the same commit as any
   new syscall) fixes the future; the backfill is still
   owed.
5. **Two secret stores.** The kernel credential store
   (B5.5) and the C6's KV/FIDO key residency are both
   "where secrets live", with the trust boundary between
   them only sketched (C6 plan, interface contracts).
   Mode B (device as companion, keys never crossing the
   UART) sharpens this: the boundary needs one written
   rule before C6L-3 designs the hand-off.
6. **Mode B ambition vs the network stack's shape.**
   The companion model wants the device to *serve* (MCP
   server, cloud as client) over a real connection;
   today's stack is a single-connection, single-request-
   at-a-time kernel server built for a dashboard. The gap
   is not a bug — it is an unmade architectural decision
   about what the device's serving surface is.

## 4. Open architecture questions for the owner

1. **Q-A1 — The kernel HTTP server's fate.** Keep it as a
   permanent, documented exception (LAN appliance, PIN-
   gated, single connection), or schedule its move to a
   userspace httpd once C6L gives us a daemon-owned
   transport to serve from?
2. **Q-A2 — Daemon image delivery.** Stay embedded in the
   kernel image (simple, atomic updates, hard size cap),
   or move to images loaded from littlefs (size freedom,
   per-daemon updates, a new load/verify path to secure)?
   UI-1's measurement informs this but cannot decide it.
3. **Q-A3 — Is Mach IPC ever switched on?** The reserved
   syscalls (10–12, 23) and the port's IPC machinery sit
   unused. Does any planned hand-off (linkd ↔ qabotd
   TOOL.DISPATCH is the live candidate, C6L-3) justify a
   minimal message surface — or is "syscalls + shared
   state owned by one daemon" the permanent answer?
4. **Q-A4 — A general process model?** Today: a fixed
   daemon set + sequential one-shots, no argv, no spawn
   from storage. Is a process-model v2 (foundation audit
   backlog) a product need or a Unix reflex?
5. **Q-A5 — Where does the pump end up?** If net_pump
   leaves the scheduler switch points, what owns it — a
   kernel thread, linkd, or a dedicated net daemon — and
   does that answer change when the C6 owns the wire?
6. **Q-A6 — The device's serving surface.** For Mode B,
   what does the device serve, on which transport, with
   what concurrency — and is that surface the web UI's
   successor or a separate thing entirely?
7. **Q-A7 — One secrets rule.** A single written rule for
   what may live in (a) the kernel credstore, (b) the
   C6's key store, (c) neither — before C6L-3 and the FW
   track harden today's sketch into wire behaviour.
