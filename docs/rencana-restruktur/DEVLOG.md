# Dev log: repository restructure — execution record

Date: 2026-10-24. Branch `dev/restruktur` (worktree
`~/workspace/qaonic_os-rst`), from `qabot/q2-tls-provider` @
`07c18a9`. Spec: `docs/rencana-restruktur/PLAN.md` (owner
approved all D-RST recommendations). **Not merged by the
executor** — the coordinator merges `--no-ff` and pushes.

## Commits (in order)

| Commit | Content |
|---|---|
| `da6327c` | Pure renames part 1 (R1–R4, R8–R12): 1,255 files, 0 insertions, 0 deletions |
| `390e4b2` | Pure renames part 2 (R5–R7, the `arm/` split per Appendix A): 90 files, 0/0 |
| `190c540` | Build-script + generator path fixes (build-mi.sh, build-md.sh, gen-splash.py, header comments in moved sources) |
| `bc447e3` | md/ext source roots added to the MD include path (plan gap #1) |
| `79f9c39` | Script, CI and `.gitignore` path fixes |
| `5d6d72f` | verify-*.py derive ROOT from script location (plan gap #2) |
| `cd80081` | Living-docs path updates + `mach3-port/README.md` layout map |
| (this log) | Execution record |

Rename mapping applied exactly as PLAN.md §4.2 (R1–R13),
with Appendix A's per-file classification verified against
the tree before moving (73 flat files + `mp/mp.h` + `draft/`
5 + `littlefs/` 11 = 90 ✓).

## Plan gaps found, and how they were resolved

1. **Cross-tree quoted include (PLAN.md §4.3).** The plan's
   quoted-include analysis covered ext-internal quotes only.
   Exactly one cross-tree quoted include exists in the whole
   split — `src/ext/audio.c` quotes `"machine_routines.h"`
   (in `src/md/arm/`) — verified by scanning both trees.
   Pre-split it resolved via the shared directory; post-split
   the first Gate A build failed on it (`MD: 41 ok, 1
   failed`). Resolution, consistent with the plan's
   dependency rule (ext → md, never the reverse): both
   source roots became include roots in `build-md.sh`
   (`-I src/md/arm -I src/ext`). No source edit.
2. **Hardcoded ROOT in the python battery (PLAN.md §7.4).**
   `verify-onb1.py`, `verify-onb2.py`, `verify-fnd1.py`,
   `verify-fnd2.py`, `verify-fnd3.py` hardcoded
   `ROOT = "/home/hatch/workspace/qaonic_os"` (the main
   tree). In the worktree they therefore tried to boot
   `<main>/mach3-port/build/mach3.elf` — nonexistent — and
   every QEMU launch failed instantly (the `.sh` scripts,
   which derive ROOT from their own location, worked).
   The plan's audit patterns did not include the absolute
   repo path. Resolution: all five now derive ROOT from
   `__file__`, matching the shell scripts — a path-reference
   fix only, and a precondition for the worktree workflow
   itself. (Side effect worth knowing: before this fix, a
   worktree battery run would have silently tested the
   *main tree's* binaries and disk images, or failed as
   here — never the worktree's own.)
3. **`.rodata` size vs PLAN.md §7.1.** The gate asks for
   section sizes identical to baseline. All loadable
   sections are identical **except `.rodata`**, whose delta
   is entirely `__FILE__` path spellings (mbedTLS assert
   strings embedded in the user-program images): 56 path
   strings in both ELFs. Full accounting, closed-form:
   baseline `.rodata` = 0x296564; control build of the
   restructured tree from a path whose build-root prefix
   has the *same length* as the baseline's = 0x29661c
   (+0xB8, the per-file suffix effect: `upstream/mach3/
   kernel/` vs `mach3-src/kernel/`, `src/ext/` vs
   `kernel/arm/`, …); the actual worktree build =
   0x2967dc (+0x278 = 0xB8 + 56 × 8, the 8-char-longer
   worktree prefix × 56 strings). Content proof: after
   normalizing old/new path prefixes, **all 3,916 text
   strings ≥ 8 chars in `.rodata` are identical** between
   baseline and restructured builds; the path strings'
   byte totals (4,081 vs 4,270 incl. NULs, Δ=189 ≈ 184 +
   alignment) account for the section delta. This is the
   "embedded build paths may legitimately differ" case the
   plan anticipated; it is recorded here as a measured,
   explained deviation from the gate's literal wording —
   not a behaviour change. `.text` (0x666c0), `.data`
   (0x1040), `.bss` (0x404788), `.text.boot`, `.ARM.exidx`,
   `.symtab`, `.strtab` sizes are all identical.

## Verification gate results

### Baseline (main tree @ 07c18a9, before branching)

- Clean build: `MI compile: 94 passed, 0 failed` ·
  `MD: 42 ok, 0 failed` · `LINK OK`.
- Sections: `.text.boot 0x20, .text 0x666c0, .ARM.exidx
  0x10, .rodata 0x296564, .data 0x1040, .bss 0x404788`.
- Battery, each standalone: SHELL PASS · SAFEMODE PASS ·
  FIRSTBOOT PASS · UPDATE PASS · ONB1 PASS · ONB2 PASS ·
  FND1 PASS · FND2 PASS (21/21) · FND3 PASS (46/46).
- Wiki: `mkdocs build --strict` rc=0.

### Gate A — clean build in the worktree

`MI 94/0 · MD 42/0 · LINK OK`; section sizes as analysed
above (all identical except `.rodata`, path strings only).

### Gate B — full battery in the worktree

First attempt (before plan gap #2 was fixed): `.sh` scripts
PASS (SAFEMODE, FIRSTBOOT, UPDATE); SHELL FAIL on one check
(`ls` → '/sd:' missing) while a parallel control build was
saturating the host CPU — and the guest log shows /sd
mounted and used by boot one-shots, i.e. a host-contention
timing flake, not a layout effect: the same script PASSes
in the rerun sequence and standalone on virgin images.
All five python scripts FAIL (hardcoded ROOT, gap #2).

Rerun (after the ROOT fix), each standalone in sequence:
SHELL PASS · SAFEMODE PASS · FIRSTBOOT PASS · UPDATE PASS
· ONB1 PASS · **ONB2 FAIL** (`finale shows the eyes again`,
`finale log line` — the known in-sequence finale flakiness
after ONB1) · FND1 PASS · FND2 PASS (21/21) · FND3 PASS
(46/46). ONB2 standalone rerun immediately after: **PASS**
(4/4, both finale checks green).

Net Gate B: **9/9 PASS**, with two first-attempt flakes
(shell under parallel-build contention; onb2 finale
in-sequence) each resolved by a standalone PASS — the same
evidence standard as the baseline's. Disk images for the
worktree were created fresh (CI-style: `truncate` +
`mkfat32.py create`); the verify scripts do not create
them.

### Gate C — wiki + grep audit

- `cd wiki && mkdocs build --strict` rc=0, no warnings.
- Audit (PLAN.md §7.4 patterns, widened with
  `kernel/build-md`, `kernel/tools/`, `kernel/mach3.ld`):
  no living or operational file references an old path.
  Remaining hits are exactly the declared exceptions:
  `archive/`, `mach3-port/upstream/`, `mach3-port/history/`,
  the historical documents of §5.2 (dev logs, dated
  research, MIGRASI/ANALISIS/ROADMAP), this plan — plus
  two *intentional* bridge notes that quote the old layout
  in order to map it: `mach3-port/README.md` and
  `wiki/docs/development.md` (§5.2 requires the latter).

### Pristine check (PLAN.md §7.5)

- Tree-object hash of the upstream tree is **identical**
  before and after: `git rev-parse 07c18a9:kernel/mach3-src`
  = `git rev-parse HEAD:mach3-port/upstream/mach3` =
  `574c09d1407270b19c05c07d10ee6eb37e47d66d`. Every byte,
  name and mode of all 1,133 files untouched.
- Rename detection across the branch: 0 upstream files
  renamed at less than 100% similarity (1,181 files total
  renamed at 100%).

### CI

`.github/workflows/ci.yml` updated (`cd mach3-port`, ELF
path), YAML parse-checked, every path traced against the
new tree. **Checked, not run** — CI triggers only on
branch `core` (PLAN.md §8).

## Deviations from the plan (summary)

- One build-flag addition beyond pure path substitution
  (gap #1, include roots) and one script-robustness fix
  beyond path substitution (gap #2, ROOT derivation) —
  both forced by evidence, both path-level, no logic or
  behaviour change.
- `docs/riset-ui-framework/RINGKASAN.md`: both of its path
  citations were updated (line 9 is a methods/provenance
  citation, line 451 a source pointer). The file sits in
  the plan's living set for the UI track; updating both
  keeps the audit unambiguous. Recorded as a judgement
  call.
- Gate §7.1's ".rodata identical" wording: see gap #3 —
  content-identical modulo `__FILE__` path spellings,
  proven by normalized string census.
