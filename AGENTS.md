# QaonicOS — instructions for coding agents

You are working in the QaonicOS repository (hafidz98/qaonicos):
a Mach 3-based appliance OS for the Luckfox Pico Mini B
(RV1103). If you are a PC-side dev agent, your lead / QA is
Muse (a separate agent); the owner is Hafidz.

## Read first (in this order)

1. `docs/AGENT-WORKFLOW.md` — your contract: roles, the
   Issue → branch → PR → QA → merge loop, and the rules
   below in full. It wins over this file on any conflict.
2. `docs/ARSITEKTUR-SISTEM.md` §1–2 — what this OS is and
   its binding design principles.
3. `wiki/docs/ai-index.md` — the fastest map of every topic
   and its canonical source document.
4. Your assigned GitHub Issue — the spec you implement.

## Hard rules

- Work only on your assigned Issue, on branch `dev/<phase>`,
  in your own git worktree. Never push to
  `qabot/q2-tls-provider`; never merge your own PR.
- English for commits, code comments, documentation.
- Plan first: if no plan section covers your task, comment
  on the Issue and stop. If the spec conflicts with the
  plan, comment and stop — do not improvise a design.
- Kernel placement follows `docs/ARSITEKTUR.md` (rule R14:
  a new kernel resident must be justified; default is
  userspace).
- Test-first for kernel behaviour: a verify script that
  FAILs before your change and PASSes after (see
  `scripts/verify-fnd2.py`, `scripts/verify-fnd3.py`).
- Never commit credentials, API keys, tokens, or `.env`
  files.
- The owner's PC has ~5 GB free storage: do NOT install
  toolchains/QEMU/ESP-IDF or build the kernel locally.
  Push your branch — GitHub Actions builds it (CI runs on
  `dev/**` pushes and PRs); link the green run in your PR.
  Keep exactly one clone; use worktrees, not extra clones.
- Write a dev log for your phase
  (`docs/<track>/DEVLOG-<phase>.md`), as the existing
  tracks do.

## Layout (post-restructure)

- `mach3-port/` — the Mach 3 port build root
  (`build-md.sh`); see `mach3-port/README.md` for the full
  map. `mach3-port/upstream/` is pristine vendored source:
  NEVER edit it. Our kernel code: `mach3-port/src/md/`
  (Mach machine-dependent) + `mach3-port/src/ext/`
  (QaonicOS extension layer); patches to upstream live in
  `mach3-port/overrides/`.
- `user/` — userspace daemons and one-shot programs.
- `scripts/` — build/verify tooling. `docs/` — plans,
  research, dev logs. `wiki/` — the MkDocs wiki.

## Verification

The QEMU regression battery runs on the lead's VM, not on
this PC: your job is correct code, a dev log, and the
evidence your Issue's acceptance criteria name (command
output, logs, dry-runs). Say in your PR exactly what you
ran and what you could not run.
