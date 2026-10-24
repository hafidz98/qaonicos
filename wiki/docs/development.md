# Development

How QaonicOS is built, run and — most importantly — *proven*.
The project's working rule is: research + a written plan first,
then implementation with a dev log, and no claim without a
verification artifact.

> **Key facts**
>
> - **Build:** `cd mach3-port && ./build-md.sh` → per-file OK summary + `LINK OK` → `mach3-port/build/mach3.elf`. The portable toolchain lives outside the repo (`~/workspace/toolchain/env.sh` must be sourced; bare QEMU fails without its module dir).
> - **GUI verification is official methodology**, not a nicety: any face/uiapp/sh/display change ships with a QEMU screenshot + a shell transcript (`docs/VERIFIKASI-GUI.md`).
> - **Serial sockets must be drained continuously** by a reader thread in any harness — guest printf is polling-mode, and an undrained socket stalls the guest CPU inside printf, mimicking a network hang.
> - Kill leftover QEMU by exact name only (`pgrep -x qemu-system-arm`) before reruns — leftovers hold the disk-image lock and serial socket and cause phantom failures.
> - All project artifacts — docs, code comments, commit messages — are in **English**. Commits/pushes for routine work need no separate confirmation; versioned releases do.

## Build

The toolchain (clang/lld + QEMU) is a portable tree that survives
VM replacement; source its `env.sh` before building. Daemon
images are **embedded into the kernel image**, so adding a user
program means touching all of:

1. the program source under `user/`,
2. **both** program loops in `mach3-port/build-md.sh`,
3. the extern declaration + a `uprogs[]` entry in `user.c`.

Kernel sources are globbed by the build from
`mach3-port/src/md/arm/` (incl. `tests/`), `mach3-port/src/ext/`
and `mach3-port/src/ext/littlefs/`. The shell one-shot embed cap
is 65,536 bytes.

## Run + verify in QEMU

| Tool | What it proves |
|---|---|
| `scripts/run-qemu.sh` | Boots the system (slirp networking; web UI forwarded to host port 18080 — curl from the VM needs `--noproxy "*"`) |
| `scripts/verify-shell.sh` | Interactive serial session against the `qaon>` prompt (per `docs/VERIFIKASI-GUI.md`) |
| `scripts/capture-gui.sh` | Screendump via the QEMU monitor — the display really rendered, not just "code ran" |
| `scripts/verify-*.py` / `verify-*.sh` | Per-feature batteries: ONB1/ONB2, UPDATE, SAFEMODE, FIRSTBOOT, FND1 (16/16), FND2 — each prints a PASS/FAIL line with a denominator |
| `scripts/soak-test.py` | 100 cold boots + power-cut (SIGKILL) cycles; archive `logs/soak-results.log` before re-running |
| One-shot selftests (`user/u*.c`) | In-guest feature probes whose PASS lines double as the boot healthy-mark input |

Traps worth knowing before you debug a "hang": the shared dev
disk persists across runs, so killed boots increment `fail_count`
and can trip safe mode mid-battery (recover with clean boots to
the healthy mark); background processes that must survive a shell
session need the double-fork daemon pattern; `/tmp` is a small
tmpfs — extract big archives under the workspace.

Methodology source:
[`docs/VERIFIKASI-GUI.md`](../docs/VERIFIKASI-GUI.md).

## Working discipline

- **Research → plan → implement → dev log.** Every new
  phase/track starts with a research doc (`docs/riset-*/`) and a
  plan (`docs/rencana-*/PLAN.md`); implementation leaves a
  `DEVLOG-*.md` in the same folder. Long docs (> ~1,000 lines)
  are split per sub-section with the main file as index.
- **Test-first where it counts:** FND-2's battery was written to
  fail on the pre-fix tree before the fix landed.
- **Say FAIL when it's FAIL.** Flakes are documented as flakes
  (e.g. the back-to-back battery safe-mode trips), never dressed
  up as passes.
- **Background work is registered** in the notification watchdog
  (`~/workspace/notifikasi/tugas.json`) with a marker file the
  task touches on completion, so finishes are reported even when
  nobody is watching.

## Repo layout (pointers)

| Path | What |
|---|---|
| `mach3-port/` | Mach 3 port build workspace — layout map in `mach3-port/README.md`; MD glue in `src/md/arm/`, extension layer in `src/ext/`, pristine upstream in `upstream/mach3/` |
| `user/` | Daemons, one-shots, `ulib`, `uartproto`, vendored `tls` |
| `scripts/` | Build/run/verify tooling, `sign-update.py`, QEMU harnesses |
| `docs/` | The deep documents this wiki synthesizes |
| `hw/`, `~/workspace/b0-sdk/` | Vendor DTS, boot-chain SDK artifacts |
| `wiki/` | This wiki (sources in `wiki/docs/`, built site gitignored) |

> Documents dated before the 2026-10-24 restructure cite the
> pre-restructure layout (`kernel/`, `kernel/kernel/arm/`,
> `kernel/mach3-src/`, …); the mapping is
> `docs/rencana-restruktur/PLAN.md` §4.2.

Sources:
[`docs/VERIFIKASI-GUI.md`](../docs/VERIFIKASI-GUI.md),
repo `README.md`, `scripts/` (as listed).
