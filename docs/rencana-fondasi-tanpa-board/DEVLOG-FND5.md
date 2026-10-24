# DEVLOG FND-5 — Update-package delivery path

Phase: FND-5 of `docs/rencana-fondasi-tanpa-board/PLAN.md`.
Branch: `dev/fnd5`. Spec: PLAN §FND-5; placement per
`docs/ARSITEKTUR.md` §4.5 (updverify = KEEP: "the shell
reads the package *file* (userspace act), the kernel
verifies and stages (kernel invariant)").

**This phase closes the FND track: FND-1 (reset/poweroff),
FND-2 (dead-daemon reclamation), FND-3 (watchdog/health
supervisor), FND-4 (persistent littlefs in QEMU) and
FND-5 (this delivery path) are all implemented, verified
test-first, and green in the full regression battery.**

## Problem

The B4/B5.3 update pipeline could only be fed two ways:
`update stage <ver>` (syscall 98 sub 7 — stages a version
*number*, no package content at all) and the `uupdsig`
one-shot, whose signed packages are compiled into the
kernel image at build time. No real package file produced
by `scripts/sign-update.py` could enter the pipeline:
there was no path from a file on a filesystem to the
kernel's verify+stage. FND-5 is that path — the product
delivery route the compile-time embed was standing in for.

## Design as built

One file changed: `user/sh.c`. **No kernel change, no new
syscall, no new sub.**

### The ABI question, answered against the code

The plan text says stagepkg "reads a package file ... in
chunks into the kernel package buffer and calls the
existing verify+stage path (syscall 98 sub 9 semantics)".
Refinement recorded: the hand-off is a **single atomic
sub-9 call**, not append/finalise subs, because the
existing surface already expresses the delivery and the
chunked alternative cannot:

- Sub 9 already takes the whole package in one call
  (user buffer + length, cap 8192) and does copy → verify
  → stage atomically. Nothing about a file source changes
  what the kernel needs.
- There is nothing to stream *with*: both filesystem read
  surfaces are whole-file syscalls — `SYS_LFS_READ` (96)
  and `SYS_FAT_READ` (54) fill a caller buffer up to a max
  and return the byte count. No offset/seek read API
  exists anywhere in the ABI, so a chunked shell loop over
  the file is not expressible without also adding file
  streaming syscalls — a much larger ABI to move ≤ 8 KB.
  (The transfers *are* chunked where chunking exists:
  `fat32_read_file` walks the chain 512 B at a time and
  littlefs reads block-wise, both inside the kernel.)
- Append subs would add cross-syscall accumulation state
  to the kernel (offset, partial buffer, owner) plus an
  FND-2-style reclamation question when a caller dies
  mid-delivery — to protect an 8 KB buffer the caller can
  already hold. The one-shot shape §4.5 praises ("verify,
  then stage, or refuse") is preserved exactly.

`sh.bin` grows 42,872 → 53,573 bytes (the 8,193-byte
staging buffer + ~1.5 KB code), under sh's 65,536 embed
cap with ~12 KB to spare.

### stagepkg behaviour

`update stagepkg <path>`:

| Path root | Reader | Path handed to the reader |
|---|---|---|
| `/data/<name>` | `SYS_LFS_READ` (96), after the shell's usual on-demand mount | bare littlefs name (`path + 6`) |
| `/sd/...` | `SYS_FAT_READ` (54) via ulib `sys_fat_read` | the path as given (`split_path` requires the `/sd` prefix) |

**The /sd-vs-/data finding: both are supported**, with
the two readers above — the shell already addressed both
filesystems for `cat`/`ls`, and stagepkg reuses exactly
those routes. Anything else (no argument, a bare name, the
memory FS, `/data` with no file part) is a usage error:
update packages are delivered on the card or in /data,
never from compile-time content.

Error discipline (the shell never stages a partial
package):

- The read asks for **cap + 1 = 8,193 bytes**. Both
  readers truncate at the requested max and return the
  count, so the return value separates all cases:
  `< 0` → *cannot read* (missing file, directory, I/O
  error); `0` → *empty*; `> 8192` → *too large* (shell
  refuses; the kernel never sees a truncated package);
  otherwise the complete file, handed to sub 9 in one
  call.
- Kernel verdicts pass through with the same wording as
  the existing `update stage` path: `-3` → "rejected (bad
  signature or format)", `-2` → "rejected (downgrade)",
  `-1` → "stagepkg failed (busy)". On success the shell
  prints the byte count and the staged version (read back
  via sub 6) followed by the familiar "staged (reboot to
  trial)".
- Trial-boot / confirm / rollback semantics are
  untouched B4/B5.3 behaviour; stagepkg only changes how
  the package arrives.

Help text updated: `update [stage <ver>|stagepkg
<path>|cancel|sabotage]`.

## Test placement tooling (host-side)

The verify script must put *binary* package files on both
filesystems; neither the host (no mtools, no FAT library)
nor the guest (shell `put` is text, ≤ 255 B) had a writer.
Two contained tools, both stdlib/existing-code only:

- **FAT**: a ~70-line adder inside `verify-fnd5.py` —
  parse the BPB of a fresh `tools/mkfat32.py` image,
  allocate a free cluster chain, write the data, add a
  root dirent, update both FAT copies.
- **littlefs**: `scripts/lfsimg.c` — the **vendored
  littlefs** (`mach3-port/src/ext/littlefs/`) compiled for
  the host with the kernel port's exact `lfs_config`
  (read/prog/cache/lookahead 256, block 4096 × 256,
  block_cycles 500), block device = the blk image's
  FND-4 tail region, prog with the BLK backend's AND
  semantics. A volume written by lfsimg is mounted by the
  guest as-is (proven by the byte-exact round-trip check
  and the /data happy path). If the region does not mount
  (blank), lfsimg formats it — the guest's own first-mount
  behaviour. This tool is reusable by later phases (e.g.
  factory-reset verification).

## Verification — scripts/verify-fnd5.py

Test-first: written and run before the shell change.

- **Pre-change: FAIL 15/33.** All 18 failures are exactly
  the stagepkg assertions (usage/missing/empty/oversized/
  tampered/downgrade/truncated, both happy paths, trial,
  confirm, post-update downgrade). Host placement, the
  probe boot and the legacy sub-7 regression all passed —
  the failure is isolated to the missing feature.
- **Post-change: PASS 33/33.** Probe boot reads the
  current version (the script is version-agnostic, like
  verify-update.sh); packages are built with
  `sign-update.py` + the dev key (good = V+1, old = V
  validly signed, tampered = good with a flipped payload
  byte, truncated = first 200 B, oversized = 9,000 B of
  junk, empty = 0 B). Boot 1 runs the sub-7 regression
  (stage + cancel), then every negative with `update`
  proving **state stays idle at V after each**, then the
  /data happy path (staged, `pending_ver=V+1`), cancel,
  and the /sd happy path left staged. Boot 2: TRIAL +
  CONFIRMED banners; running V+1 on the flipped slot,
  idle. Boot 3: the same package file is now refused as a
  downgrade — the delivery path end to end. Cleanup
  removes the /data test file (lfsimg) and recreates the
  SD image fresh; the disk ends healthy, idle, at V+1
  (unprovisioned — FND-4's closing `setup reset` state is
  not touched by this flow).

## Regression battery (standalone, this worktree)

| Script | Result |
|---|---|
| verify-shell.sh | PASS |
| verify-safemode.sh | PASS |
| verify-firstboot.sh | PASS |
| verify-update.sh | PASS |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS |
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS (21/21) |
| verify-fnd3.py | PASS (46/46) |
| verify-fnd4.py | PASS (36/36) |
| verify-fnd5.py | PASS (33/33, run before and after the rest of the battery) |

Build: `MD: 42 ok, 0 failed`, `LINK OK`. No battery
script needed hygiene adjustments for FND-5 (its own
script scrubs/seeds everything it depends on).

## Honest limitations

- **QEMU-only proof.** On hardware the same files arrive
  by the future delivery media (USB gadget / C6 link /
  webui upload — plan non-goals for this phase) and
  stagepkg consumes them unchanged, but no hardware ran.
- **The 8 KB cap stays** (plan non-goal: a flash-layout
  question). A real kernel image does not fit in 8 KB, so
  on hardware stagepkg will refuse real images as
  "too large" until the cap/layout decision is made; in
  the QEMU A/B model the staged payload is version
  metadata, exactly as B4/B5.3 established — stagepkg
  changes the package's *route*, not what a trial boot
  executes.
- The dev key (`scripts/update-img/dev-key.pem`) remains
  a committed test key, as in B5.3; nothing in FND-5
  changes the key story.
- stagepkg is a dev-shell surface. A product delivery UX
  (progress, resume, network fetch) belongs to the C6 /
  webui tracks, per the plan's non-goals.
