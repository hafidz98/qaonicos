# Agent Workflow — PC Dev Agent + GitHub Coordination

Status: living document (adopted 2026-10-24, owner decision).
This is the operating contract for development work done by a
coding agent running on the owner's PC, coordinated through
GitHub, with Muse as lead / manager / QA. It generalises the
arrangement that already worked for browser-relay into a
repo-wide rule set.

## Roles

| Role | Who | Owns |
|---|---|---|
| Owner | Hafidz | Decisions: plan approval, D-* items, priorities, releases. |
| Lead / Manager / QA | Muse (this agent) | Plans and task specs, assignment, review, verification, and **all merges** to the working branch. |
| Implementer | PC dev agent | Implementation on its own branch, PR, fixes per review. Never pushes to the working branch, never merges its own PR. |

## The loop (one task = one Issue = one branch = one PR)

1. **Spec first.** No plan section, no task. Every task starts
   as a GitHub Issue written by the lead, containing: context,
   the plan section it implements, scope in / scope out, and
   **measurable acceptance criteria** (named verify scripts
   and their PASS targets, or an equally checkable artifact).
2. **Branch.** The implementer works on `dev/<phase>` (from
   the working branch `qabot/q2-tls-provider`), in its own
   worktree on the PC. One writer per file at a time — the
   lead will not schedule two tasks that touch the same hot
   files concurrently.
3. **PR.** One PR per Issue, following the PR template:
   plan section cited, what changed, verification evidence,
   dev log written (project rule: research/plan first, dev
   logs always).
4. **QA at the lead.** The lead pulls the PR branch, rebuilds,
   and runs the **full regression battery in QEMU** on the
   VM (the proven harness lives there; QEMU on Windows is a
   time sink) plus GUI capture evidence where the change is
   visual. Review comments go on the PR; the implementer
   fixes on the same branch.
5. **Merge.** Only the lead merges (`--no-ff`, atomic per
   phase), pushes, and closes the Issue. Commits, comments
   and docs in English.

## Division of labour (what goes where)

**To the PC agent:**
- Hardware-phase work (it sits next to the board; the PC
  agent executes flash/serial steps locally under its Issue
  spec and attaches the logs/evidence to the PR — the lead
  reviews asynchronously. **No Tailscale/SSH bridge**
  (parked 2026-10-24, owner security call): the lead has no
  remote access to the PC. A live supervised session is an
  optional, ephemeral exception the owner enables per
  session and turns off afterwards).
- ESP32-C6 firmware (ESP-IDF is native on the PC).
- Windows-side tooling (e.g. the HW bridge runner).
- Parallel tracks whose files do not collide with the VM
  line.

**Stays on the VM line:**
- Plans, research, architecture documents.
- Integration and all QA (QEMU battery, soak).
- Anything touching shared hot files (`user.c` and peers)
  while another task holds them.

## Labels and milestones

Labels: `fnd`, `c6l`, `ui`, `hw`, `docs`, `agent-pc`
(executor), `lead` (executor). One milestone per track
(Foundations, C6 Link, UI, Hardware). The board view is
GitHub Issues/Projects filtered by these.

## Rules that bind the implementer

- English for commits, code comments, docs (standing owner
  rule). Chat with the owner is not the implementer's job.
- Follow the plan; where the plan is silent, the boundary
  rules in `docs/ARSITEKTUR.md` (esp. R14, the kernel
  admission rule) and the principles in
  `docs/ARSITEKTUR-SISTEM.md` decide placement.
- Test-first for kernel behaviour: a verify script that
  FAILs before and PASSes after, same discipline as
  FND-2/FND-3.
- Never commit credentials, keys, or `.env` files. The
  update-signing dev key in `scripts/update-img/` is a
  documented test key — never introduce another.
- If a task looks wrong against the plan, stop and comment
  on the Issue instead of improvising a different design.

## Pilot

The loop is proven end-to-end with one bounded task before
scaling: the HW bridge runner (see the pilot Issue).
