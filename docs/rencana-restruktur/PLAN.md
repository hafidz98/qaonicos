# Plan: repository restructure — folder layout and naming

Status: **PLAN ONLY** (2026-10-23). Nothing in this document has been
executed. The owner reviews, answers the D-RST decisions in §8, and
execution starts as the first worktree dev (`dev/restruktur`).

Baseline: branch `qabot/q2-tls-provider`, HEAD `afc73b6`
(FND-3 done). All counts below were measured on that tree.

---

## 1. The problem

The owner, reading the tree cold, sees duplication:

```
kernel/                 <- a folder named "kernel"
└── kernel/             <- containing another folder named "kernel"
    ├── arm/            <- "arm" here...
    └── mach/arm/       <- ...and "arm" again under "mach"
kernel/mach3-src/
└── kernel/             <- and a third "kernel" inside mach3-src
```

The contents are **not** duplicated — every level has a distinct role
(§2). What is duplicated is the *vocabulary*: three different things
are called `kernel`, two different things are called `arm`, and no
document maps the layout (the one README that tried,
`kernel/README.md`, is stale — it still claims the Mach sources are
*not* in this repo and live at `~/workspace/mach3-src`, which stopped
being true when the sources were vendored). This plan fixes the
names, not the code. **Zero behaviour change is the bar (§7).**

## 2. Current-state inventory

### 2.1 The kernel build root and everything under it

`kernel/` at repo root is **not** "the kernel". It is the build
workspace of the Mach 3 ARM port: build scripts, the linker script,
our port sources, the vendored upstream tree, the patch overlay, the
build generators, the porting reports, and the (gitignored) build
output. 1,255 of the repo's ~1,840 tracked files live under it.

| Path (today) | Tracked files | Role | Referenced by |
|---|---:|---|---|
| `kernel/` (root) | 1,255 | Port build workspace (everything below) | `cd kernel` in README ×2, CI, `scripts/run-qemu.sh`, `scripts/build-update-img.sh` |
| `kernel/build-md.sh`, `kernel/build-mi.sh` | 2 | The build: MD compile + link; MI compile. `build-md.sh` also builds and embeds every user program | 39 × `build-md.sh` in 23 files (docs, wiki, scripts) |
| `kernel/mach3.ld` | 1 | Linker script | `build-md.sh` |
| `kernel/build/` | 0 (gitignored) | Build output: objects, generated headers, `mach3.elf`, and the `inc/` symlink farm (`inc/machine`, `inc/mach/machine`) | `kernel/build/mach3.elf`: **21 occ / 19 files** — every `scripts/verify-*`, `scripts/run-qemu.sh`, CI, both READMEs |
| `kernel/mach3-src/` | 1,133 | **Pristine vendored CMU Mach 3.0 tree.** Its child `kernel/` is upstream's own source root (`kern/ vm/ ipc/ mach/ device/ ddb/` + MD dirs for i386/alpha/mips/… — upstream has **no** `arm/`; Mach 3 predates ARM support). Content must never change | Only the two build scripts name the wrapper (`SRC=.../mach3-src/kernel`); the *assembled* tree is referenced everywhere via include paths |
| `kernel/mi-overrides/` | 7 | Our patched copies of upstream MI files, in upstream-relative layout (`kern/` 4, `vm/` 1, `ddb/` 2). `build-mi.sh` substitutes an override for the pristine file of the same relative path, and puts the dir first in `-I` order so converted headers shadow upstream ones. This is what keeps `mach3-src` pristine | 6 occ / 4 files (both build scripts + 2 historical reports) |
| `kernel/kernel/` | 98 | **Our** port source root — the analogue of upstream's source root, in our tree | `kernel/kernel/arm`: **76 occ / 44 files** (living architecture docs, plans, wiki, `scripts/gen-splash.py`, `user/ulib/ulib.h`) |
| `kernel/kernel/arm/` | 90 | **Two roles in one folder.** (a) Mach machine-dependent (MD) glue: the files that satisfy Mach's `<machine/*.h>` contract and ARM platform bring-up. (b) The whole QaonicOS extension layer: syscall dispatch/scheduler (`user.c`), net stack + HTTP + web UI, littlefs port, bootstate, credstore, update verifier, drivers. Full per-file classification in Appendix A | The relative fragment `kernel/arm` is the most-referenced path string in the repo: **181 occ / 111 files** (substring count; includes the full-path count above, source header comments, and historical docs) |
| `kernel/kernel/arm/littlefs/` | 11 | Vendored littlefs v2.9.3 + our `shim/` headers; compiled by a dedicated loop in `build-md.sh` | `lfs_port.c`, `build-md.sh` |
| `kernel/kernel/arm/draft/` | 5 | Draft hardware drivers (SFC NAND, SPI0, ST7789) — **not compiled** | `docs/ARSITEKTUR.md` inventory |
| `kernel/kernel/mach/arm/` | 8 | Our public `<mach/machine/*.h>` headers (upstream naming: `mach/` is Mach's interface directory; `arm` here means the machine these interface headers describe) | `build-mi.sh` symlink `inc/mach/machine -> kernel/mach/arm`; 18 occ / 13 files for `kernel/mach/arm` |
| `kernel/tools/` | 3 | Host-side build generators (`gen_config.py`, `gen_mig_stubs.py`, `mi_sources.py`) | Both build scripts |
| `kernel/docs/` | 8 | Porting reports M1–M6 + `MI-PATCHES.md` + `RELEASE.md` — all dated 2026-09-28, all historical | `kernel/README.md`, `kernel/Makefile` |
| `kernel/Makefile`, `kernel/gen-skeleton.sh` | 2 | M1 scaffolding: a placeholder probe Makefile (points at the *external* `~/workspace/mach3-src`) and the one-off skeleton generator. Neither is used by the current build; both embed pre-vendoring path assumptions | Nothing living |
| `kernel/README.md` | 1 | Stale orientation doc (see §1) | — |

How the build **assembles** the tree today (this is the layout that
must survive the rename intact, §4.3):

| Logical view (what the compiler sees) | Physical source today |
|---|---|
| MI source root (`<kern/…>`, `<ipc/…>`, `<vm/…>`, `<mach/…>`, `<device/…>`, `<ddb/…>`) | `kernel/mach3-src/kernel/` (via `SRC` in both build scripts; per-file override substitution from `mi-overrides/`) |
| `<machine/*.h>` | symlink `build/inc/machine` → `kernel/kernel/arm/` |
| `<mach/machine/*.h>` | symlink `build/inc/mach/machine` → `kernel/kernel/mach/arm/` |
| MD + extension objects | glob: `kernel/kernel/arm/*.c`, `*.s`, plus `kernel/kernel/arm/littlefs/*.c` |
| Link | `mach3.ld` → `kernel/build/mach3.elf` |

### 2.2 The rest of the repo root

| Path | Tracked files | Role | Verdict preview (§4.4) |
|---|---:|---|---|
| `user/` | 328 | Userspace programs + `ulib/`, `face/`, `uiapp/`, `qabot/`, `tls/` (vendored mbedTLS, 222 files), `uartproto/`, `cfg/`. The name is accurate; it is also the most-referenced path in the repo (`user/`: 319 occ / 99 files, 58 of them inside `build-md.sh` alone) | **Keep** — accurate name, largest blast radius, zero gain |
| `scripts/` | 19 | Repo-level run/verify/update tooling (`run-qemu.sh`, the whole `verify-*` battery, `build-update-img.sh`, `gen-splash.py`, soak test, recorders) + `update-img/` inputs | **Keep** — accurate; contents get path fixes only |
| `tools/` | 6 | Host utilities (`mkfat32.py` — used by CI — `pem2c.py`, QEMU/VNC helpers, mock servers) | **Keep** — scope differs from `kernel/tools/`; the *inner* one is the one that gets renamed (§4.2) |
| `docs/` | 98 | All project documentation: standing references at top level, `rencana-*/` (plans), `riset-*/` (research), `devlog/`, `assets/` | **Keep**, including the Indonesian `rencana-`/`riset-` folder convention — it is consistent, owner-native, and renaming it would churn hundreds of citations for no structural gain |
| `wiki/` | 19 | MkDocs site (`mkdocs.yml` + `docs/` pages; built `site/` gitignored) | **Keep**; its citations to kernel paths are updated (§5) |
| `hw/dts/` | 21 | Vendor device trees — the hardware address reference | **Keep** — `hw/` is broad but honest; 9 occ / 7 files |
| `disk-images/` | 0 (gitignored) | Generated QEMU disk images | **Keep** — 36 occ / 18 files, all operational |
| `archive/` | 88 | Frozen pre-Mach kernel (phases 1–17) + early QEMU tests. Contains its own internal `kernel/` tree — frozen history, not a naming problem | **Keep, untouched** |
| `logs/` | 8 | Tracked soak-test result logs (historical evidence); the directory also collects untracked run artifacts locally | **Keep** |
| `.github/workflows/ci.yml` | 1 | CI: `cd kernel && ./build-md.sh`, boots `kernel/build/mach3.elf`. Triggers only on branch `core`, so it cannot be exercised from the working branch | Path fixes in the same change (§6) |
| (untracked) `crashtest/`, `push-*.sh`, `push-repair.lock`, loose `logs/*.ppm/png` | — | Local working artifacts, not part of the repo | **Out of scope** — a worktree execution never sees them; nothing is deleted |

### 2.3 Reference-count method

Counts are `git grep -F -c` over tracked text files at `afc73b6`,
excluding file *contents* under `kernel/mach3-src/` (upstream's
internal self-references are not ours to fix). Substring counts
overlap (e.g. `kernel/arm` also matches inside `kernel/kernel/arm`)
and are labelled as such; they are per-pattern blast radii, not a
partition. Headline: **the most-referenced path fragment being
renamed is `kernel/arm` (181 occ / 111 files)**; the most-referenced
*unambiguous operational* path is `kernel/build/mach3.elf`
(21 occ / 19 files) — every verification script consumes it, which
is why §7 gates on the full battery rather than on grep alone.

## 3. Naming principles

1. **One name = one thing.** No two directories in the repo share a
   basename unless one is a frozen archive or an upstream mirror.
   (`kernel` ×3 and `arm` ×2 are the violations being fixed.)
2. **A folder's name says its role, not its ancestry.** `mach3-src`
   says where the code came from; the reader needs to know what it
   *is* (pristine upstream — do not touch). Role names: `upstream/`,
   `overrides/`, `src/`, `md/`, `ext/`.
3. **Upstream's own names stay upstream's.** Inside the vendored
   tree, `kernel/`, `kern/`, `mach/` etc. are CMU's vocabulary and
   the content is frozen — the rename stops at the wrapper. The same
   respect applies to `user/tls/mbedtls/` and `archive/`.
4. **Mirror the assembled tree.** Where our source root plays the
   same role as upstream's source root, the parallel should be
   visible in the layout (§4.3), so a reader who learns one tree can
   navigate the other.
5. **Depth beats cleverness.** Prefer one extra honest level
   (`upstream/mach3/kernel`) over a compressed name that needs a
   paragraph to explain (`mach3-src`). No abbreviation that isn't
   already Mach vocabulary (`md`, `mi`, `ext` are; they are defined
   in the layout map).
6. **Renaming is for names that actively mislead.** Accurate names
   stay even when they are unfashionable (`user/`, `hw/`, `logs/`).
   Every rename must pay for its citation churn with clarity.

## 4. Target layout

### 4.1 The new build root

```
qaonic_os/
├── mach3-port/            # was kernel/ — the port's build workspace
│   ├── README.md          # NEW layout map (§6, step 6) — replaces the stale kernel/README.md
│   ├── build-mi.sh        # unchanged name (Mach vocabulary: MI = machine-independent)
│   ├── build-md.sh        # unchanged name (MD = machine-dependent)
│   ├── mach3.ld           # unchanged
│   ├── upstream/
│   │   └── mach3/         # was kernel/mach3-src/ — wrapper renamed, content byte-identical
│   │       └── kernel/    # upstream's own root; CMU's name, kept
│   ├── overrides/         # was kernel/mi-overrides/ — patched MI files, upstream-relative layout
│   │   ├── kern/  vm/  ddb/
│   ├── src/               # was kernel/kernel/ — our port source root
│   │   ├── md/
│   │   │   └── arm/       # MD glue (Appendix A) + mp/ intact + tests/ (self-tests)
│   │   ├── mach/
│   │   │   └── arm/       # <mach/machine/*.h> interface headers, intact
│   │   └── ext/           # extension layer (Appendix A) + draft/ + littlefs/ intact
│   ├── build-tools/       # was kernel/tools/ — host generators for this build
│   ├── history/           # was kernel/docs/ + the M1 scaffolding (§4.2)
│   └── build/             # generated, gitignored — was kernel/build/
├── user/                  # unchanged
├── scripts/               # unchanged (path contents updated)
├── tools/                 # unchanged
├── docs/                  # unchanged
├── wiki/                  # unchanged (citations updated)
├── hw/                    # unchanged
├── disk-images/           # unchanged
├── archive/               # unchanged
└── logs/                  # unchanged
```

### 4.2 Rename mapping (complete)

| # | Old path | New path | Rationale (one line) |
|---|---|---|---|
| R1 | `kernel/` | `mach3-port/` | The worst offender: the folder is the port's *workspace*, not the kernel; `mach3-port` is the name the project already uses for this work (standalone repo `~/workspace/mach3-port`, tag `mach3-port-v1.0`) |
| R2 | `kernel/mach3-src/` | `mach3-port/upstream/mach3/` | Names the role (pristine upstream, grouped under `upstream/` so a future vendored tree has an obvious home) and drops the ancestry word `-src`; only the wrapper moves — every byte inside is untouched |
| R3 | `kernel/mi-overrides/` | `mach3-port/overrides/` | `mi-` was meaningful only next to `mach3-src`; `overrides/` + upstream-relative children (`kern/ vm/ ddb/`) say exactly what the build does with it |
| R4 | `kernel/kernel/` | `mach3-port/src/` | Kills the `kernel/kernel` doubling; `src/` is the honest name for "our source root" and parallels upstream's root one level over |
| R5 | `kernel/kernel/arm/` (MD subset, 43 files + `mp/`) | `mach3-port/src/md/arm/` | The MD half of the split (§4.3): files that implement Mach's machine-dependent contract for ARM |
| R6 | `kernel/kernel/arm/{ipc_test.c,sched_test.c}` | `mach3-port/src/md/arm/tests/` | Bring-up self-tests belong with the MD they exercise, not in the flat compile glob's way |
| R7 | `kernel/kernel/arm/` (extension subset, 28 files + `draft/` + `littlefs/`) | `mach3-port/src/ext/` | The extension half of the split: product services that happen to live in the kernel (the `docs/ARSITEKTUR.md` extension layer), now visibly *not* Mach MD code |
| R8 | `kernel/kernel/mach/arm/` | `mach3-port/src/mach/arm/` | Moves with its source root; internal names are upstream's (`mach/` = interface dir) and stay |
| R9 | `kernel/tools/` | `mach3-port/build-tools/` | Distinguishes "generators for this build" from the repo-level `tools/` (host utilities) |
| R10 | `kernel/docs/` | `mach3-port/history/` | Ends the `docs/` vs `kernel/docs/` ambiguity; these are dated porting reports, i.e. history that belongs to the port |
| R11 | `kernel/Makefile`, `kernel/gen-skeleton.sh` | `mach3-port/history/` (contents unchanged) | M1 scaffolding out of the operational root; both embed pre-vendoring paths and are kept as artifacts, clearly labelled historical in the new README — retiring beats deleting (they document how the port started) |
| R12 | `kernel/build/` | `mach3-port/build/` | Generated output follows its workspace; `.gitignore` updated in the same commit |
| R13 | `kernel/README.md` | `mach3-port/README.md` | Same path modulo R1, but **rewritten** as the layout map (§6 step 6) — the old content is stale enough that preserving it would preserve misinformation; its history remains in git |

Not renamed, deliberately: `build-mi.sh` / `build-md.sh` (Mach
vocabulary, 39 citations, the names are fine once the layout map
defines MI/MD), `mach3.ld`, everything in §2.2 marked Keep, and all
file *basenames* inside the moved trees (this is a folder
restructure, not a file-renaming pass — `user.c` stays `user.c`).

### 4.3 The `arm/` split — verdict: split, two ways

**Recommendation: split `kernel/kernel/arm/` into `src/md/arm/` and
`src/ext/`, per the per-file table in Appendix A.**

Why not keep one folder with a README explaining the two roles:
the folder is the extension layer's home *and* the MD contract's
home, and those have different rules — MD files are bound by
upstream's `<machine/*.h>` interface and mirror upstream's `mips/`
and `alpha/` MD dirs, while extension files are product code bound
by `docs/ARSITEKTUR.md`'s placement rules (and are the prime
candidates to one day leave the kernel entirely, per D-ARCH-1).
A reader — human or agent — currently cannot tell which rulebook a
file follows without opening it. The split makes the rulebook a
property of the path.

Why two ways and not three (no separate `drivers/`): several
extension files are drivers (`blk.c`, `gpio.c`, `pl330.c`,
`audio.c`, `gpu.c`), but they are drivers *for the extension
layer's services*, several are QEMU-virtio devices that exist only
to serve the product, and a third bucket would force judgement
calls (is `uart.c` a driver or MD? is `gic.c`?) that the two-way
split avoids: the test is not "what does it talk to" but "whose
contract does it implement — Mach's MD interface, or QaonicOS's
product". By that test `uart.c` (early console, required by
`arm_init`) and `gic.c` (interrupt glue the trap path depends on)
are MD; `blk.c` (storage service for bootstate/FAT/syscalls) is
extension. Appendix A applies the test file by file.

Build consequences (mechanics, §6): the `<machine/*.h>` symlink
retargets to `src/md/arm/`; extension sources compile from `src/ext/`
with the same include roots (extension code includes
`<machine/*.h>` and `<mach/machine/*.h>`, never the reverse — MD
files include no extension headers today, verified by grep, so the
split creates no include cycle); quoted includes inside the
extension set (`"ramfs.h"`, `"webui.h"`, `"credstore.h"`,
`"littlefs/lfs.h"`, `"splash_text.h"`) all stay within `ext/` and
resolve unchanged. `littlefs/` and `draft/` move intact as
subdirectories of `ext/` (littlefs stays next to its port file;
separating vendored provenance into a `third_party/` is D-RST-6,
recommended *no* for this pass).

The assembled tree after the rename (principle 4 — same logical
view, new physical addresses):

| Logical view | Physical source after rename |
|---|---|
| MI source root | `mach3-port/upstream/mach3/kernel/` (+ per-file substitution from `mach3-port/overrides/`) |
| `<machine/*.h>` | symlink `mach3-port/build/inc/machine` → `mach3-port/src/md/arm/` |
| `<mach/machine/*.h>` | symlink `mach3-port/build/inc/mach/machine` → `mach3-port/src/mach/arm/` |
| MD objects | `mach3-port/src/md/arm/*.c`, `*.s`, `tests/*.c` |
| Extension objects | `mach3-port/src/ext/*.c`, `ext/littlefs/*.c` |
| Link | `mach3-port/mach3.ld` → `mach3-port/build/mach3.elf` |

### 4.4 Everything else: positions

- `user/`, `scripts/`, `tools/`, `docs/`, `wiki/`, `hw/`,
  `disk-images/`, `archive/`, `logs/`: **keep** (§2.2). Their names
  do not mislead; `scripts/` and `tools/` contents receive path
  fixes, not renames.
- `docs/` internal taxonomy (`rencana-*`, `riset-*`, Indonesian
  names): **keep**. It is a consistent, deliberate convention, not
  an accident of growth.
- The stale `~/workspace/mach3-src` external clone and the
  standalone `~/workspace/mach3-port` repo: **not part of this
  restructure**. They are separate checkouts with their own
  histories; §7 lists them as a risk to check, not a thing to move.

## 5. What does NOT move, and which documents are living

### 5.1 Hard non-movers

- **Every byte under `mach3-src/`** (the future
  `upstream/mach3/`): the wrapper directory is renamed by `git mv`;
  no file content, name, or internal path inside it changes. This
  is verifiable after execution: `git diff` on the rename commit
  must show 100% similarity for all 1,133 files.
- **`archive/` internals**: frozen; its internal `kernel/` is a
  historical artifact, not a duplicate to fix.
- **Historical documents' contents** (§5.2): they record the paths
  of their time. Rewriting them would falsify the record — a
  dev log that says `kernel/kernel/arm/user.c` is *correct* about
  the tree it was written against.

### 5.2 Living vs historical documents

**Living** — updated to the new paths during execution (§6 step 6),
because a reader follows them to do work today:

| Document | Why living |
|---|---|
| `README.md`, `README.id.md` | The front door: structure diagram, build/run commands, license note |
| `mach3-port/README.md` (new) | The layout map itself (§6) |
| `docs/ARSITEKTUR.md` | Standing reference; its §2 inventory cites `kernel/kernel/arm/` per subsystem |
| `docs/ARSITEKTUR-SISTEM.md` | Standing reference; component map + flows cite the extension layer's path |
| `docs/UTANG-TEKNIS.md` | Standing debt register (path mentions are filename-level; audit still covers it) |
| `docs/SYSCALL-ABI.md` | Standing ABI contract (mentions `psci.c` by name; audit covers it) |
| `docs/hw-addrs.md` | Standing hardware reference (no repo kernel paths today; audit covers it) |
| `docs/VERIFIKASI-GUI.md` | Standing verification methodology; cites scripts and build outputs |
| `docs/rencana-fondasi-tanpa-board/PLAN.md` | Track still executing (FND-4/FND-5 remain) |
| `docs/rencana-komunikasi-c6/PLAN.md` | Track approved, not yet executed — its path citations are instructions, not history |
| The UI/LVGL plan set (`docs/riset-ui-framework/RINGKASAN.md` path citations that function as build instructions, `docs/rencana-*` UI material as applicable) | Approved next track; same reasoning as C6L |
| All `wiki/docs/*.md` + `wiki/README.md` | The wiki is the readable layer over the *current* tree; every citation must resolve |
| Source-file header comments in moved living code | A file that moves and still names its old path in its own header is a self-mislabel; path strings in comments of moved files are updated (comment-only change) |

**Historical** — contents untouched, wherever they sit:

- All `DEVLOG*.md` (including FND-1..3), `docs/devlog/`
- All dated research: `docs/riset-*/RINGKASAN.md` and
  `BAGIAN-*.md` (except UI build-instruction citations above),
  `docs/ringkasan-a4-ntp-persist/`
- `docs/MIGRASI-MACH3.md`, `docs/ANALISIS-PORTING.md`,
  `docs/ROADMAP-FASE13-18.md` (dated migration/porting records)
- `mach3-port/history/` contents (M1–M6, `MI-PATCHES`, `RELEASE`,
  scaffolding)
- Tracked `logs/` soak results

For future readers: the new `mach3-port/README.md` layout map and
`wiki/docs/development.md` each carry one line — *"Documents dated
before the restructure cite the pre-restructure layout; the mapping
is `docs/rencana-restruktur/PLAN.md` §4.2."* That line is the
bridge; the historical texts themselves stay as written.

## 6. Mechanics

Execution is the **first worktree dev** under the standing rule
(worktree + branch per dev, atomic merge). No step of this plan is
executed in the main tree.

```sh
# from the main checkout, at the then-current HEAD of qabot/q2-tls-provider
git worktree add ~/workspace/wt-restruktur -b dev/restruktur
cd ~/workspace/wt-restruktur
```

Order of operations (each numbered step = one commit on
`dev/restruktur`; the branch is allowed to be broken between steps
2–3 — nobody else builds it):

0. **Baseline capture** (in the *main* tree, before branching):
   clean build via `kernel/build-md.sh` — record the MI pass count,
   the `MD: N ok, 0 failed` count, and `llvm-objdump -h` section
   sizes of `kernel/build/mach3.elf`; run the §7 battery once and
   record PASS lines; `mkdocs build --strict`. These numbers are
   the comparison target, recorded in the execution dev log
   (`docs/rencana-restruktur/DEVLOG.md`, created by execution).
1. **Pure renames, part 1** — `git mv` for R1–R4, R8–R12 (whole
   directories). No content edits. (R1 is implicit: the children
   move into `mach3-port/`.)
2. **Pure renames, part 2** — the `arm/` split (R5–R7) by Appendix
   A, `git mv` per file/subdirectory.
3. **Build-script fixes** — `build-mi.sh`, `build-md.sh`:
   `SRC`/`OVR` variables, the two `inc/` symlink targets, the MD
   compile globs (now three source roots), the littlefs loop path,
   `tools/` → `build-tools/` invocations, `mach3.ld` path,
   `../user` (unchanged — the worktree root keeps `user/` as a
   sibling). Also `scripts/gen-splash.py` (its output path is built
   from parts: `os.path.join(ROOT, "kernel", "kernel", "arm", …)` —
   grep for split-string forms, not just literals) and path strings
   in moved files' header comments. **Gate A: clean build from a
   tree with no `build/` dir; counts and section sizes match
   baseline.**
4. **Script and CI fixes** — every `scripts/*` consumer of
   `kernel/build/mach3.elf` / `disk-images/` (disk-images does not
   move, but the ELF path does), `.github/workflows/ci.yml`
   (`cd mach3-port`, ELF path; YAML syntax-checked locally since CI
   itself only triggers on `core`), `.gitignore`
   (`kernel/build/` → `mach3-port/build/`), `scripts/build-update-img.sh`
   (its three ELF references + its `cd`). **Gate B: the full §7
   battery, standalone.**
5. **Living-docs fixes** — §5.2's living list, both READMEs' structure
   diagrams and build commands, and the new `mach3-port/README.md`
   layout map (the map from §4.1, the naming principles in one
   paragraph, the assembled-tree table from §4.3, the historical
   scaffolding warning, and the pre-restructure citation note from
   §5.2). Wiki pages updated in the same commit. **Gate C:
   `mkdocs build --strict` + the §7 grep audit.**
6. **Merge** — coordinator reviews the branch as a whole, merges
   `dev/restruktur` into `qabot/q2-tls-provider` as a **single
   `--no-ff` merge commit**, pushes via
   `~/workspace/push-qaonicos.sh`, then removes the worktree and
   the branch. The execution dev log is the last commit on the
   branch, so the merge carries its own evidence.

## 7. Verification gate

All of the following must pass **in the worktree** before the merge
is proposed. Zero behaviour change is the bar: same counts, same
sizes, same PASS lines as the §6 step-0 baseline.

1. **Clean build**: from the worktree with `mach3-port/build/`
   deleted, `cd mach3-port && ./build-md.sh` →
   `MD: N ok, 0 failed` with N equal to baseline, `LINK OK`, and
   `llvm-objdump -h` section sizes identical to baseline. (Byte-
   identity of the ELF is *not* demanded — embedded build paths
   may legitimately differ; section sizes may not.)
2. **Full regression battery, each standalone** (the battery's
   known shared-disk state interactions mean back-to-back runs are
   not the evidence; standalone runs are):
   `scripts/verify-shell.sh`, `scripts/verify-safemode.sh`,
   `scripts/verify-firstboot.sh`, `scripts/verify-update.sh`,
   `scripts/verify-onb1.py`, `scripts/verify-onb2.py`,
   `scripts/verify-fnd1.py`, `scripts/verify-fnd2.py`,
   `scripts/verify-fnd3.py` — every one PASS, matching baseline.
3. **Wiki**: `cd wiki && mkdocs build --strict` clean.
4. **Grep audit**: no file in the living set (§5.2) and no
   operational file (build scripts, `scripts/`, CI, `.gitignore`,
   moved source comments) references an old path. Patterns:
   `kernel/kernel`, `kernel/mach3-src`, `kernel/mi-overrides`,
   `kernel/build/`, `cd kernel`, `"kernel", "kernel"` (split-string
   forms), `mach3/kernel/arm` (the even older form used in source
   header comments). **Declared exceptions** (expected hits, listed
   in the dev log): everything under `archive/`, the historical
   documents of §5.2, the contents of `mach3-port/history/`, the
   contents of `upstream/mach3/`, and this plan itself.
5. **Pristine check**: the rename commits show 100% similarity for
   every file under the old `mach3-src/` (`git log --follow` /
   rename detection on the merge), and
   `git diff <baseline> -- upstream/mach3` contains no content
   change — renames only.

## 8. Risks and rollback

| Risk | Where it hides | Mitigation |
|---|---|---|
| Path literals grep can't see | Compiled artifacts in a stale `build/` — its `inc/` symlinks are **absolute** and would silently point at the old location | Execution happens in a fresh worktree (no `build/` exists); Gate A builds from clean. Anyone with a stale main-tree `build/` deletes it before their next build — noted in the new README |
| Split-string / constructed paths | `scripts/gen-splash.py` builds its output path from `os.path.join` parts; other scripts may do the same | Audit patterns (§7.4) include the split forms; the battery (Gate B) is the real net — a wrong path fails loudly there |
| The external push script | `~/workspace/push-qaonicos.sh` | **Checked for this plan**: it assumes only the repo root (`cd ~/workspace/qaonic_os`) and the branch name; the push map is keyed by branch, not by path. No change needed |
| `~/workspace/mach3-port` (standalone repo) | A separate checkout whose root has the same internal layout names (`kernel/`, `mi-overrides/`, `build-md.sh`) — a tired reader could confuse the two, and this plan renames only the in-repo copy | Named explicitly in the new README's map ("the standalone `~/workspace/mach3-port` repo is a separate, frozen checkout — this workspace is not it"). Renaming *it* is out of scope (D-RST-7) |
| The external `~/workspace/mach3-src` clone | `kernel/Makefile`'s probe target references it; it is historical scaffolding | It moves to `history/` unchanged (R11); nothing operational references the external clone |
| Agent/operator memory | Saved project memory and habits cite `kernel/build-md.sh`, `kernel/kernel/arm/user.c` etc. | Not grep-able in-repo. The coordinator updates the standing memory notes in the same session as the merge, and the wiki's Development page + AI Index (updated in step 5) become the findable truth |
| CI never exercised | `ci.yml` triggers only on `core`; a path typo there would surface only at the next `core` merge | YAML parse + a manual trace of every path in the file against the new tree during step 4; flagged in the dev log as *checked, not run* |
| Rename-detection failure | If any moved file is also edited in the rename commit, git may record delete+add and lose `--follow` continuity | Steps 1–2 are pure `git mv` commits, edits are separate commits (§6) — this ordering is a requirement, not a preference |

**Rollback.** The restructure lands as one `--no-ff` merge commit
on `qabot/q2-tls-provider`. Rolling back is one operation —
`git revert -m 1 <merge>` — followed by the standard push; no
partial states exist because no step was merged separately. If the
merge has not happened yet, rollback is deleting the worktree and
branch: the main tree was never touched.

## 9. Owner decisions (D-RST)

| ID | Decision | Options | Recommendation |
|---|---|---|---|
| D-RST-1 | Name of the outer build root (R1) | (a) `mach3-port/` (b) `mach3/` (c) keep `kernel/` | **(a)** — matches the project's existing name for this work; (b) is shorter but reads as "the Mach 3 sources", which is `upstream/`'s job |
| D-RST-2 | Upstream wrapper shape (R2) | (a) `upstream/mach3/` (b) `mach3-upstream/` (flat) | **(a)** — the `upstream/` parent gives future vendored trees a home and groups provenance in one place; (b) is one level shallower |
| D-RST-3 | Our source root name (R4) | (a) `src/` (b) `port/` (c) keep `kernel/` under the new outer name | **(a)** — (b) repeats the parent (`mach3-port/port/`); (c) keeps a `kernel/` that still needs explaining |
| D-RST-4 | The `arm/` split (R5–R7) | (a) two-way split `md/arm/` + `ext/` (b) keep one folder + README sub-structure (c) three-way with a separate `drivers/` | **(a)** — §4.3; (c) forces contract judgements the two-way test avoids |
| D-RST-5 | M1 scaffolding (`Makefile`, `gen-skeleton.sh`) (R11) | (a) move unchanged to `history/` (b) leave at the port root (c) delete | **(a)** — out of the operational root, nothing destroyed |
| D-RST-6 | littlefs placement | (a) keep at `ext/littlefs/` next to its port (b) move to `src/third_party/littlefs/` | **(a)** for this pass — provenance separation is real but orthogonal; revisit when a second vendored library lands in the kernel |
| D-RST-7 | The standalone `~/workspace/mach3-port` repo | (a) leave frozen, document the distinction (b) restructure it identically later (c) archive it formally | **(a)** now; it is a tagged release artifact (`mach3-port-v1.0`), and touching it is a separate decision |

---

## Appendix A: `kernel/kernel/arm/` — per-file classification

Test applied (§4.3): *whose contract does the file implement —
Mach's MD interface, or QaonicOS's product?* Subdirectories move
intact. Counts: MD 43 files + `mp/mp.h` + 2 tests = 46; extension
28 files + `draft/` (5) + `littlefs/` (11) = 44; total 90 ✓.

**→ `src/md/arm/`** (Mach MD contract + ARM platform glue):

`aeabi.c`, `arm_init.c`, `asm.h`, `asm_linkage.h`, `ast.h`,
`ast_types.h`, `clock.c`, `context.s`, `copy.s`, `cpu.h`,
`cpu_number.h`, `db_machdep.h`, `db_stubs.c`, `db_trace.h`,
`devices.c`, `fpu.c`, `gic.c`, `kttd_machdep.h`, `locore.s`,
`lock.h`, `mach_param.h`, `machdep.c`, `machine_routines.h`,
`machspl.h`, `mig_stubs.c`, `pcb.c`, `pmap.c`, `pmap.h`, `pte.h`,
`regdef.h`, `sched_param.h`, `setjmp.h`, `thread.h`,
`time_stamp.h`, `timer.h`, `trap.c`, `trap_frame.h`, `uart.c`,
`uprog.s`, `userasm.s`, `vm_param.h`, `vm_tuning.h`, `xpr.h`,
and `mp/mp.h` (subdir intact).

**→ `src/md/arm/tests/`**: `ipc_test.c`, `sched_test.c`.

**→ `src/ext/`** (QaonicOS extension layer):

`aesgcm.c`, `aesgcm.h`, `audio.c`, `blk.c`, `bootstate.c`,
`credstore.c`, `credstore.h`, `fat32.c`, `gpio.c`, `gpu.c`,
`http.c`, `lfs_port.c`, `net.c`, `netmain.c`, `netstack.c`,
`pl330.c`, `pl330.h`, `psci.c`, `ramfs.c`, `ramfs.h`,
`splash_text.h`, `tcp.c`, `updkey.h`, `updverify.c`, `updverify.h`,
`user.c`, `webui.c`, `webui.h`, and the subdirectories `draft/`
and `littlefs/` (intact).

Borderline calls, recorded so execution doesn't re-litigate them:
`uart.c` → MD (early console; `arm_init` depends on it before any
product service exists). `gic.c`, `clock.c` → MD (trap/clock path
glue). `psci.c` → ext (its own header calls it a service *for the
extension layer*; it exists because FND-1 syscalls needed it).
`blk.c` → ext (its consumers are bootstate, FAT32 and the block
syscalls — product services all). `devices.c` → MD (BSD `config`
tables the MI links against; content is a link requirement, not a
service).

## Appendix B: reference counts (method in §2.3)

| Pattern | Occurrences | Files | Note |
|---|---:|---:|---|
| `user/` | 319 | 99 | Not renamed — shown for scale |
| `docs/` | 284 | 71 | Not renamed |
| `kernel/arm` (substring) | 181 | 111 | Most-referenced renamed fragment; overlaps the next row |
| `kernel/kernel/arm` | 76 | 44 | Full path to the split folder |
| `scripts/` | 64 | 33 | Contents fixed, folder kept |
| `build-md.sh` (bare) | 39 | 23 | Name kept |
| `disk-images/` | 36 | 18 | Folder kept |
| `kernel/build/` | 23 | 21 | Moves with R12 |
| `mach3-src` (bare) | 22 | 10 | Wrapper renamed by R2 |
| `kernel/build/mach3.elf` | 21 | 19 | The operational path every verify script consumes |
| `kernel/mach/arm` | 18 | 13 | Moves with R8 |
| `kernel/build-md.sh` | 12 | 10 | Path changes via R1 only |
| `hw/dts` | 9 | 7 | Kept |
| `cd kernel` | 7 | 7 | Becomes `cd mach3-port` |
| `mi-overrides` | 6 | 4 | Renamed by R3 |
| `kernel/mach3-src` | 2 | 2 | Only the two build scripts name the wrapper in full |
