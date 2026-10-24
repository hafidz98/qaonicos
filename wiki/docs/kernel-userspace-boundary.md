# Kernel / Userspace Boundary

For most of its life the kernel/userspace split in QaonicOS grew
organically, one phase at a time. This page is the digest of the
policy document that now governs it —
[`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) — what may live in the
kernel, what today's kernel residents are doing there, and where
planned work will land.

> **Key facts**
>
> - Mach 3 is kept for **VM only**: real tasks, real PL0 isolation; Mach threads are created but never scheduled, and Mach IPC is unused in the product path.
> - Everything product-shaped in the kernel sits in the **extension layer** (`mach3-port/src/ext/`, centred on the 2,270-line `user.c`).
> - "IPC" in QaonicOS means **shared kernel globals behind a syscall surface** (display token, web-shell bridge, bootstate record) — there is no message passing.
> - The ABI is **poll-shaped**: no blocking syscalls, by design.
> - Consequence that drives everything: a userspace fault kills one program; a kernel-resident fault is a kernel fault — and every resident also spends the ~287 KB boot-partition headroom.

## Placement rules R1–R14 (digest)

### Must live in kernel

| Rule | Content |
|---|---|
| R1 | **Traps, scheduling, memory** — context switching, trap frames, pmaps exist nowhere else. |
| R2 | **Hardware arbitration** — a resource contended by ≥2 programs gets exactly one kernel-owned arbiter (token/owner/queue). Programs hold policy; the kernel holds the fact of ownership. |
| R3 | **Non-skippable safety invariants** — a check whose bypass breaks a product promise (no unsigned update staged; the credential key never leaves the store; a locked-out PIN stays locked out) is enforced in kernel, atomically with the action it gates. |
| R4 | **Hardware-only facts** — data only registers can produce (ticks, ADC samples, entropy, counters) is collected in kernel and published as facts; interpretation is not the kernel's job. |
| R5 | **The pre-userspace path** — whatever must work before any program runs (boot state, crash recording, program loading, boot splash) is kernel by construction. |

### Must not live in kernel

| Rule | Content |
|---|---|
| R6 | **No policy** — thresholds, escalation ladders, retries, UX behaviour: userspace. (One principled exception: policy that must run when userspace cannot — FND-3's hang escalation — small, fixed, justified under R14.) |
| R7 | **No parsing of untrusted input** — network/storage/link bytes are parsed in userspace. The kernel may do fixed-size, length-checked framing/demux — never grammars. |
| R8 | **No application logic, no presentation** — no HTML, no screens, no human formatting in the kernel. |
| R9 | **No waiting** — kernel code never blocks on an external event; the poll-shaped ABI is the house style. |

### Grey-zone criteria + admission

| Rule | Content |
|---|---|
| R10 | **Atomicity shape** — if correctness needs atomic check-and-act across programs, the kernel may hold the *state* (only the state); policy stays in userspace. |
| R11 | **Blast radius** — between equal placements, choose the one whose worst bug is a dead program, not a dead device. |
| R12 | **QEMU testability** — a kernel resident not exercisable in QEMU must ship with a simulated source or a one-shot harness. |
| R13 | **The image budget** — every resident spends the shared ~287 KB headroom; size is an architectural argument. |
| R14 | **Kernel admission rule** — a new kernel resident's plan must state: (a) which of R1–R5 requires it; (b) why a syscall surface over a userspace owner fails; (c) the untrusted-input surface vs R7; (d) its QEMU test story vs R12; (e) its size vs R13. Weak answers ⇒ the default is userspace. |

## Verdicts on today's kernel residents

| Resident | Verdict | Why (short) |
|---|---|---|
| HTTP server + web UI (`http.c`, `webui.c`, ~1,250 lines) | **MOVE CANDIDATE** | Parses untrusted input and generates HTML in kernel context, inside the scheduler switch path (violates R7/R8; worst blast radius, R11). Moving needs a server-socket syscall surface + an `httpd` daemon; the PIN-gate *verdict* (hash compare + throttle) stays kernel under R3. Until it moves: fixed buffers only, no new routes/parsers without review, no new kernel-side web features. |
| TCP/IP stack | **KEEP** (driver + stack); MOVE WHEN TOUCHED for new transports/second consumers | One server + one client consumer, welded to the polled pump mechanics. The trigger to revisit is a second concurrent consumer — C6L's bridge arbitration (D-C6L-4) is exactly that pressure. |
| littlefs | **KEEP** | The kernel itself is a client (credstore, provisioning config) before daemons exist (R5); a filesystem server would need IPC that doesn't exist. Clients are already syscall-mediated (93–97). |
| Credential store + AES-GCM | **KEEP** | Textbook R3: key material never crosses the boundary; the file format is fixed-size records (no grammar, R7 satisfied). Moving it would create the exposure it exists to prevent. |
| Update verification (`updverify`) | **KEEP** | Verify-and-stage are one atomic syscall (98 sub 9; bad signature → −3). Userspace verification would make the invariant depend on every caller behaving. |
| Boot state + boot log | **KEEP** | R5 in its purest form: written by the panic path, read before userspace exists. |
| Arbiters + event queue + face mailbox | **KEEP** | R2's reference implementation: a few words of state each; all policy already in userspace. |
| Audio driver | **KEEP** the register/FIFO/DMA layer | The voice *protocol* (framing, sessions) already lives in userspace — correct. |
| Time state | **KEEP** as-is | Kernel holds the fact; the "what counts as synced" rule is enforced at the syscall gate; the NTP client is a userspace program. The model split for stateful facts. |
| SYS_STAT aggregation | **KEEP** | Pure fact assembly (R4); presentation already lives outside (the dashboard being the known exception). |
| Spawn / lifecycle, FAT32 + ramfs, boot splash | **KEEP** | R1 / kernel-internal consumers / R5 respectively. |
| uartproto + mock, TLS | **Correctly placed** (userspace) | A link protocol is peer-input parsing + policy; TLS is the model citizen: large, parse-heavy, isolated failure. |

## Owner decisions (open)

| # | Decision | Document's recommendation |
|---|---|---|
| D-ARCH-1 | The HTTP server + web UI's fate: (a) keep + harden forever, (b) planned move to a userspace `httpd`, (c) move-when-touched | **(b)** — gated on the server-socket surface, sequenced with D-ARCH-3, after C6L. Cost ≈ one phase (M). |
| D-ARCH-2 | Adopt R14 as binding for phase plans, or advisory only | **Binding.** |
| D-ARCH-3 | The netstack's long-term home: (a) kernel indefinitely, (b) split at the transport (driver+IP stay; transports gain a shared socket surface), (c) full `netd` daemon | **(b)** — the smallest step that ends the single-consumer era cleanly. |
| D-ARCH-4 | Userspace IPC: build a minimal message IPC, or formally abandon it | **Abandon formally** — per-service syscall surfaces are the house pattern that has actually shipped. |

## Forward placement (digest)

The full 16-row table is in the source (§5). The shape to
remember: CRU/TRNG/crypto/RGA2/SFC/SPI/SARADC/SDMMC/USB drivers are
kernel *register layers* behind narrow syscall surfaces; linkd,
gadget class logic, power policy, LVGL and camera orchestration
are userspace. THR-1's FPU context is kernel with **no** new
surface. Factory reset splits into a kernel erase primitive (R3)
and a userspace flow (R6).

Sources:
[`docs/ARSITEKTUR.md`](../docs/ARSITEKTUR.md) (full text, §1–§6),
[`docs/riset-threading-compute/RINGKASAN.md`](../docs/riset-threading-compute/RINGKASAN.md) (§1, execution model),
[`docs/riset-fondasi-tanpa-board/RINGKASAN.md`](../docs/riset-fondasi-tanpa-board/RINGKASAN.md).
