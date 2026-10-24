# Glossary

Project terms, defined once. Where a term has a precise source
definition, the page that owns the topic is linked.

| Term | Meaning |
|---|---|
| **Daemon** | A persistent userspace program in the fixed set: `face` (progid 0), `uiapp` (1), `qabotd` (2), `sh` (3). Daemons are embedded in the kernel image and launched by the kernel; they are *not* Mach IPC servers. |
| **One-shot** | A userspace program that runs to completion: boot selftests, `ufirst`, feature probes (`u*.c`). One-shots double as the regression battery, and their PASS is the boot healthy-mark input. |
| **Progid** | The daemon/program index used by kernel arbiters (display token, console owner, face-request ownership). `0`=face, `1`=uiapp, `2`=qabotd, `3`=sh. |
| **Extension layer** | The hand-written product layer inside the kernel (`mach3-port/src/ext/`): syscall dispatch, cooperative scheduler, arbiters, and resident services (net stack, HTTP, littlefs, bootstate, credstore). Sits on the frozen Mach 3 core. |
| **Net pump** | `net_pump()` — one iteration of the whole network server (RX + TCP timers + one HTTP request), called only from the scheduler's two switch points. If daemons stop yielding, the pump starves. |
| **Cooperative scheduling** | Daemons switch only at `SYS_YIELD` and at exit. No preemption; quantum preemption (THR-3) is gated on a measured jitter need. |
| **Display token** | The kernel-held single-owner right to draw and receive input events. Face holds it by default; uiapp receives it via GRANT and returns it via RELEASE. See [Display, UI & Face](display-ui-face.md). |
| **Console owner** | The daemon that owns console input (sh, via `SYS_CONSOLE_TAKEOVER`). Reclaimed to −1 when its owner dies (FND-2). |
| **Bootstate** | The persistent boot-resilience record (magic "BOOT") in a raw tail sector: boot/fail counts, last reason, provisioned + GUI-mode flags, update slot state. Syscall 98. |
| **Bootlog** | The 16-record persistent event ring below the bootstate sector (TRIAL, ROLLBACK, SAFEMODE, software reset, …). Syscall 99; `bootlog` in the shell. |
| **Healthy mark** | The bootstate write that zeroes `fail_count` and confirms a trial update — *earned* when all boot one-shots pass, never automatic. |
| **Safe mode** | Boot state entered at `fail_count ≥ 3`: shell only, a recovery surface. One healthy boot clears it. |
| **Sabotage flag** | Bootstate flags bit1: suppresses exactly one healthy mark. The test hook that proves update rollback works. |
| **QUP1** | The signed update package format: `"QUP1" | ver | len | RSA-2048 signature | payload`. See [Updates](updates.md). |
| **QSEC** | The credential-store file format: `"QSEC" | ver | nonce | AES-256-GCM ciphertext | tag` inside a `cfg-*` littlefs file. |
| **Embed (daemon images)** | Userspace programs are linked *into* the kernel image at build time (`uprogs[]`); the boot partition therefore budgets kernel + all programs together (~287 KB headroom). |
| **TODO(HW) seam** | The standing pattern for hardware-only behaviour: a named seam in code with a simulated source on the QEMU side, so callers are testable board-less (e.g. the `psci.c` PMU/WDT seam, the simulated `SYS_POWER` source). |
| **Simulated source** | A scriptable stand-in for a hardware fact (voltage curve, entropy, mock C6) that lets a subsystem's full stack be verified in QEMU. |
| **Mock C6 / mock-c6.py** | The simulated ESP32-C6 peer. Today: an in-process mock (`mock_comcu.c`) behind the uartproto library. From C6L-2: a Python host peer that is the *executable spec* for the real firmware. |
| **linkd** | The planned C6L-5 daemon: single owner of the link transport (D-C6L-4), so the single guest TCP connection is never contended. |
| **Face** | Qabot's visual embodiment and the device's home screen: a procedural C renderer (eyes / waveform / sphere states). Also owns first-boot QR setup and (by design) power policy. |
| **Policy gate** | The harness's pre-execution check on every tool call: ALLOW / CONFIRM (physical or web confirmation) / BLOCK. The agent safety boundary. |
| **Modes A / B / C** | Qabot connectivity: A = device calls an LLM API directly; B = device serves, a cloud harness (Jivix) is the client; C = hybrid. See [Qabot Agent Runtime](qabot-agent.md). |
| **Slirp** | QEMU's user-mode networking (host at 10.0.2.2); the guest stack's dev environment — and the proven source of the TLS keep-alive flakiness, not a stack bug. |
| **stagepkg** | The planned FND-5 shell builtin: `update stagepkg <path>` — read a real package file into the verified staging path. |
| **SYS_STAT / `qaon_stat`** | The statistics struct behind syscall 57 (uptime, memory, block, net). `cpu_pct` and `blk_used_sec` are honestly `QAON_UNKNOWN` today; there is no battery field. |

Sources: the topic pages linked above and
[`docs/ARSITEKTUR-SISTEM.md`](../docs/ARSITEKTUR-SISTEM.md).
