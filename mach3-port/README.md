# mach3-port/ — layout map

This directory is the **build workspace of the Mach 3 ARM port**:
the scripts that build QaonicOS's kernel, our port sources, the
vendored upstream tree, and the patch overlay. It is not "the
kernel folder" — it is the port's workshop, and this map is the
authoritative guide to what lives where. (Layout established by
the 2026-10-24 restructure; mapping from the old layout:
`docs/rencana-restruktur/PLAN.md` §4.2.)

```
mach3-port/
├── README.md          # this layout map
├── build-md.sh        # THE build: MI + user programs + MD/ext, links build/mach3.elf
├── build-mi.sh        # MI-only compile pass (called by build-md.sh)
├── mach3.ld           # linker script
├── upstream/
│   └── mach3/         # PRISTINE vendored CMU Mach 3.0 — do not edit, ever
│       └── kernel/    # upstream's own source root (kern/ vm/ ipc/ mach/ device/ ddb/
│                      #   + MD dirs for i386/alpha/mips/...; upstream has no arm/)
├── overrides/         # our patched copies of upstream MI files, upstream-relative
│   ├── kern/  vm/  ddb/   # build-mi.sh substitutes these for the pristine file
│                      #   of the same relative path — this is what keeps
│                      #   upstream/ pristine
├── src/               # OUR port source root (parallels upstream/mach3/kernel/)
│   ├── md/
│   │   └── arm/       # Mach machine-dependent (MD) contract for ARM:
│   │       ├── tests/ #   bring-up self-tests (ipc_test.c, sched_test.c)
│   │       └── mp/    #   multiprocessor header (UP build; kept for upstream parity)
│   ├── mach/
│   │   └── arm/       # our <mach/machine/*.h> public interface headers
│   └── ext/           # the QaonicOS EXTENSION layer: product services that
│       ├── draft/     #   live in the kernel (syscall dispatch/scheduler in
│       └── littlefs/  #   user.c, net stack, HTTP/web UI, bootstate, credstore,
│                      #   update verifier, drivers). draft/ = uncompiled HW
│                      #   driver drafts; littlefs/ = vendored littlefs + shim
├── build-tools/       # host-side generators for this build (gen_config.py,
│                      #   gen_mig_stubs.py, mi_sources.py) — NOT repo-level tools/
├── history/           # dated M1–M6 porting reports, MI-PATCHES, RELEASE, and the
│                      #   M1 scaffolding (Makefile, gen-skeleton.sh). Historical:
│                      #   paths cited inside are the layout of their time
└── build/             # generated output (gitignored): objects, gen/, inc/
                       #   symlink farm, mach3.elf
```

## Naming principles (one paragraph)

One name = one thing; a folder's name says its **role**, not its
ancestry (`upstream/`, `overrides/`, `src/`, `md/`, `ext/`);
upstream's own names stay upstream's (inside `upstream/mach3/`,
`kernel/`, `kern/`, `mach/` are CMU's vocabulary and the content
is frozen); where our source root plays the same role as
upstream's, the layout mirrors it so a reader who learns one tree
can navigate the other. Vocabulary: **MI** = machine-independent
(Mach's core), **MD** = machine-dependent (the port's contract
with Mach), **ext** = the QaonicOS extension layer (product code
that lives in the kernel, governed by `docs/ARSITEKTUR.md`).

## How the build assembles the tree

| Logical view (what the compiler sees) | Physical source |
|---|---|
| MI source root (`<kern/…>`, `<ipc/…>`, `<vm/…>`, `<mach/…>`, `<device/…>`, `<ddb/…>`) | `upstream/mach3/kernel/` (per-file substitution from `overrides/`) |
| `<machine/*.h>` | symlink `build/inc/machine` → `src/md/arm/` |
| `<mach/machine/*.h>` | symlink `build/inc/mach/machine` → `src/mach/arm/` |
| MD objects | `src/md/arm/*.c`, `*.s`, `src/md/arm/tests/*.c` |
| Extension objects | `src/ext/*.c`, `src/ext/littlefs/*.c` |
| Link | `mach3.ld` → `build/mach3.elf` |

Dependency rule: extension code may include MD headers
(`<machine/*.h>`); MD code never includes extension headers.

## Where does a new file go?

- Implements Mach's MD contract for ARM (trap/pmap/clock/… glue,
  `<machine/*.h>` headers)? → `src/md/arm/`
- Is a `<mach/machine/*.h>` interface header? → `src/mach/arm/`
- Is product logic/services in the kernel (a syscall service, a
  driver for a product feature, the web UI, …)? → `src/ext/`
  (and it must pass the kernel admission rule, `docs/ARSITEKTUR.md` R14)
- Patches an upstream MI file? → copy it to the same relative
  path under `overrides/` and patch it there; never edit
  `upstream/`.
- A host-side generator for this build? → `build-tools/`
  (repo-level host utilities live in the repo's `tools/` instead)
- A userspace program? → repo-level `user/`, never in here.

## Build

```sh
source ~/workspace/toolchain/env.sh   # portable toolchain (or any clang/lld)
cd mach3-port && ./build-md.sh        # -> build/mach3.elf, "MD: 42 ok, 0 failed"
```

## Warnings

- **`build/` holds absolute symlinks.** A stale `build/` copied
  or left over from another checkout points at that checkout's
  sources. Delete `build/` before building in a fresh location.
- **This workspace is not the standalone `~/workspace/mach3-port`
  repo.** That is a separate, frozen checkout (the tagged
  `mach3-port-v1.0` release artifact) with the pre-restructure
  internal layout. Nothing here syncs to it.
- **Documents dated before the 2026-10-24 restructure cite the
  pre-restructure layout** (`kernel/`, `kernel/kernel/arm/`,
  `kernel/mach3-src/`, …). The mapping is
  `docs/rencana-restruktur/PLAN.md` §4.2; historical texts are
  intentionally left as written.
