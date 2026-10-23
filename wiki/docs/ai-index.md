# AI Agent Index

This page is the machine-friendly front door: one line per wiki
page, the **canonical source document** for each topic (the wiki
synthesizes; the source decides), and repo layout pointers.

> **Rules for agents working from this wiki**
>
> 1. **Code beats docs.** Where a page and the source tree disagree, the tree is the truth and the page is owed an update.
> 2. **Newest research wins.** Where sources conflict, later research supersedes — the worked example is the SoC corrections register ([`docs/riset-soc-rv1103/RINGKASAN.md`](../docs/riset-soc-rv1103/RINGKASAN.md) §3: clock 1.104 GHz, TF socket present, CAM connector routed, no VOP/decoder).
> 3. **Known staleness:** `docs/SYSCALL-ABI.md` is 28 syscalls behind the code (backlog #14). Never invent syscall rows; verify numbers in `kernel/kernel/arm/user.c`.
> 4. **Parked means parked:** secure boot (B5.4), the LLM API key, multi-user — decided by the owner; do not re-propose.
> 5. Project artifacts (docs, code, comments, commits) are in **English**.

## Coverage map

| Wiki page | Covers (one line) | Canonical source |
|---|---|---|
| [Home](index.md) | What QaonicOS is; status snapshot; page map | `docs/ARSITEKTUR-SISTEM.md` |
| [Architecture](architecture.md) | Layers, component map, boot flow, eight runtime flows | `docs/ARSITEKTUR-SISTEM.md` |
| [Kernel / Userspace Boundary](kernel-userspace-boundary.md) | Rules R1–R14, resident verdicts, decisions D-ARCH-1..4 | `docs/ARSITEKTUR.md` |
| [Hardware & SoC](hardware-soc.md) | RV1103 inventory digest, corrections, leverage order, key addresses | `docs/riset-soc-rv1103/RINGKASAN.md` (+ `BAGIAN-A/B/C`), `docs/hw-addrs.md` |
| [Boot & Resilience](boot-resilience.md) | Boot chain, bootstate/bootlog, safe mode, FND-1/2, FND-3 design, acceptance bar | `docs/rencana-fondasi-tanpa-board/PLAN.md` + `DEVLOG-FND1/FND2.md`; `docs/riset-out-of-box/RINGKASAN.md` |
| [Storage](storage.md) | littlefs backends, FAT32 `/sd`, NAND layout, data placement | `docs/rencana-storage-littlefs/PLAN-B1.md`; FND plan §FND-4 |
| [Networking & the C6 Link](networking-c6.md) | Guest stack as-is, wire protocol v1/v2, drifts, C6L phases, D-C6L-1..4 | `docs/riset-komunikasi-c6/RINGKASAN.md`; `docs/rencana-komunikasi-c6/PLAN.md` |
| [Display, UI & Face](display-ui-face.md) | Token arbitration, face renderer, uiapp, LVGL adoption + UI-1 gate | `docs/riset-ui-framework/RINGKASAN.md`; `docs/riset-ui-qaonicos/RINGKASAN.md` |
| [Qabot Agent Runtime](qabot-agent.md) | ReAct harness, policy gate, modes A/B/C, competitor digest | `docs/rencana-qabot-harness/PLAN.md`; `docs/riset-agent-bots/RINGKASAN.md` |
| [Onboarding & Security](onboarding-security.md) | D-ONB-1..7, ONB-1/ONB-2 as built, QSEC credstore, threat-relevant caveats | `docs/rencana-onboarding/PLAN.md` + `DEVLOG-ONB1-ONB2.md`; `docs/rencana-hardening/PLAN-B55.md` |
| [Power & Battery](power-battery.md) | Display-only "sleep" today, PMU ladder, SARADC IN1, SYS_POWER design, PWR-1..4 | `docs/riset-power-battery/RINGKASAN.md` |
| [Updates](updates.md) | A/B state machine, QUP1, −2/−3 codes, FND-5 delivery gap | `docs/rencana-update/PLAN-B4.md`; `docs/rencana-hardening/PLAN-B5.md` |
| [Syscall ABI](syscall-abi.md) | Calling convention; documented surface; extension numbers per track docs | `docs/SYSCALL-ABI.md` (**stale — see rule 3**); `docs/ARSITEKTUR.md` §2 |
| [Development](development.md) | Build, QEMU verification methodology, harness traps, working discipline | `docs/VERIFIKASI-GUI.md`; repo `README.md`; `scripts/` |
| [Tracks & Roadmap](tracks-roadmap.md) | Locked order FND → C6L → UI; proposed SOC/PWR/THR; hardware-gated; parked | `docs/ARSITEKTUR-SISTEM.md` §8; track plans |
| [Glossary](glossary.md) | Project terms with one-line definitions | topic pages above |

## Repo layout pointers

| Path | What an agent needs to know |
|---|---|
| `kernel/kernel/arm/` | The extension layer: `user.c` (dispatch, scheduler, arbiters), `bootstate.c`, `tcp.c`, `http.c`, `webui.c`, `credstore.c`, `lfs_port.c`, `psci.c`, `draft/` (uncompiled HW drivers) |
| `kernel/build-md.sh` | The build; new user programs go in **both** program loops + `uprogs[]` in `user.c` |
| `user/` | `face/`, `uiapp/`, `qabotd.c` + `qabot/`, `sh.c`, one-shots `u*.c`, `ulib/`, `uartproto/`, `tls/` (vendored mbedTLS) |
| `scripts/` | `run-qemu.sh`, `capture-gui.sh`, `verify-shell.sh`, `verify-*.py`, `soak-test.py`, `sign-update.py` |
| `docs/` | Deep documents: `ARSITEKTUR-SISTEM.md` (structure+flows), `ARSITEKTUR.md` (placement policy), `SYSCALL-ABI.md` (stale), `riset-*/` (research), `rencana-*/` (plans + dev logs) |
| `hw/dts/` | Vendor device trees (RV1103 inherits RV1106 definitions) |
| `wiki/` | This wiki: `mkdocs.yml` + `docs/*.md` sources; `site/` is build output (gitignored) |

## Last updated

| Page group | Stamp |
|---|---|
| All pages (initial comprehensive build) | 2026-10-23 |

Sources for this index: the repository tree itself, surveyed
2026-10-23.
