# DEVLOG FND-4 — Persistent littlefs in QEMU

Phase: FND-4 of `docs/rencana-fondasi-tanpa-board/PLAN.md`.
Branch: `dev/fnd4`. Spec: PLAN §FND-4; placement per
`docs/ARSITEKTUR.md` (littlefs = KEEP in kernel, R1/R5).

## Problem

In QEMU the littlefs backend was a 1 MB RAM array, so every
config file — device name, timezone, PIN hash, setup secret,
credstore secrets — evaporated on every QEMU restart. The
whole onboarding / credential regression story (ONB-1/2,
B5.5) could only ever be tested inside a single boot, and
FIRSTBOOT's boot-2 name check had to be skipped with a
"backend is RAM" comment.

## Design as built

All changes are in `mach3-port/src/ext/lfs_port.c`; nothing
above the port changed shape.

### Two backends behind the existing callbacks

The `lfsbd_read/prog/erase/sync` callbacks configured into
`lfs_cfg` are now dispatchers over two backends, selected
by `lfsbd_kind`:

- **RAM** — the original B1 backend, byte for byte: 1 MB
  array, NAND prog semantics, power-cut simulator
  (`lfsbd_cut_after`). Unchanged behaviour.
- **BLK** — a reserved region of blk dev 0 (FND-4).

### Reserved region

Same littlefs geometry as the RAM backend (256 blocks ×
4096 B = 1 MB), tail-anchored exactly the way bootstate
anchors its own reservations:

```
sector (512 B)            owner
---------------------------------------------------------
0 .. base-1               (head: unused by this layout)
base .. base+2047         littlefs region  (2048 sectors)
total-18 .. total-3       bootlog ring     (16 sectors)
total-2                   bootstate state sector
total-1                   blk selftest scratch
base = total - 18 - 2048
```

On the standard 16 MiB image (32768 sectors): the region
is **sectors 30702..32749** (1 MiB), the bootlog ring is
32750..32765, state 32766, scratch 32767.

Collision argument: the region's end (exclusive) is
`total-18`, which is exactly the bootlog ring's start;
bootstate touches only `[total-18, total)`; the blk
selftest touches only `total-1`. A full grep audit of
dev-0 writers (`blk_write_sector` callers) finds exactly
three: `blk_selftest`, `bootstate`, and now `lfs_port` —
no other writer exists (FAT32 uses the separate dev-1
`sd_*` API). The region therefore cannot collide with
any existing reservation, on any image size, because all
four reservations are computed from the same tail anchor.

The layout mirrors the NAND draft's "littlefs lives in a
reserved span" shape (`draft/sfc_nand.c`: NAND blocks
320..999); the hardware geometry differs (128 KB blocks)
but the swap stays a backend change under the same glue.

### BLK backend semantics

- **read**: per-sector reads into the caller's buffer.
- **prog**: read-modify-write per sector, AND-ing the new
  data in — littlefs never observes a 0→1 transition
  without an erase, exactly the NAND rule the RAM backend
  enforces and littlefs is designed around.
- **erase**: write 0xFF across the block's 8 sectors.
- **sync**: no-op (virtio-blk writes complete
  synchronously).
- All region I/O goes through 3-attempt retry wrappers —
  bootstate's B2.8 lesson (blk requests can time out
  transiently under host CPU contention) applies here
  too. A hard I/O failure returns `LFS_ERR_IO` (-5).

### Backend selection

Boot-time, in `lfs_port_mount()`: /data defaults to BLK.
If `blk_total_sectors()` is too small to hold
region + tail (+ 64 sectors of slack) the port prints a
warning and falls back to RAM, so a no-disk boot still
gets a working /data. Every mount prints one line naming
the backend (and the sector range for BLK).

### Self-test isolation (B1 behaviour preserved)

`lfs_run_test()` pins the RAM backend before it formats
anything, so the B1 power-cut self-test can never erase
the real /data — previously it formatted the same RAM
disk /data later mounted. If /data is mounted when the
test is invoked, the test unmounts it first; its state is
on blk and the next `lfs_port_mount()` remounts it. The
ULFS one-shot result is unchanged (PASS mask 0x1f, RAM
backend, cut simulation intact).

## Behaviour deltas worth knowing

1. **The self-test's canary no longer leaks into /data.**
   `lfs_run_test()` stage 4 writes `hello.txt`; pre-FND-4
   that file was visible in the product `ls /data`
   afterwards (test and product shared one disk). It now
   stays on the test's private RAM disk. One battery
   script depended on the leak — see hygiene list.
2. **Config now genuinely persists**, including across
   unclean QEMU kills, by littlefs's own atomicity
   (copy-on-write metadata pairs): a cut mid-write leaves
   old-or-new, never garbage. Proven by verify-fnd4
   boots 3/4.

## Verification — scripts/verify-fnd4.py

Test-first: written before the kernel change and run
against the unmodified build first.

- **Pre-change: FAIL 24/34.** The 10 failures were exactly
  the persistence assertions: boot-2 provisioned+name/tz,
  PIN persisted, marker content, cfg-apikey prefix,
  login-page-not-create-form, /settings key present, and
  all four boot-4 post-cut survival checks. Every in-boot
  check passed, isolating the failure to persistence.
- **Post-change: PASS 36/36.** Boots: A (hygiene
  `setup reset`), 1 (provision: PIN create flow, POST
  /settings name/tz/API key, marker file, QSEC blob
  captured, `setup done`, clean `poweroff` → QEMU rc 0),
  2 (fresh QEMU process, same image: boot reason 4 +
  "software poweroff" bootlog record, provisioned flag,
  name/tz/PIN read back, marker exact, blob evidence,
  login with the boot-1 PIN, key present on /settings,
  ULFS + UCREDSTORE PASS), 3 (second marker committed,
  then a put-stream SIGKILLed mid-flight), 4 (FS mounts
  and lists, both committed markers intact, provisioning
  + name intact, `bootctl` shows fail_count back to 0,
  bootlog parses; ends with `setup reset`).

One evidence note: the cfg-apikey blob cannot be scraped
byte-for-byte from outside — shell `cat` is a text command
(binary truncates at the first NUL), and B5.5 deliberately
exposes no plaintext read. The script therefore compares
the blob's **visible prefix** (magic + version + nonce
head; the nonce is random per write, so equal prefixes
mean the same sealed bytes) and adds a functional
whole-entry proof: in boot 2, clearing the key through
/settings flips it from present to absent, and re-setting
restores presence — the entry boot 2 operates on is the
one boot 1 sealed.

## Regression battery (standalone, this worktree)

| Script | Result |
|---|---|
| verify-shell.sh | PASS (after hygiene fix H1) |
| verify-safemode.sh | PASS |
| verify-firstboot.sh | PASS (assertion strengthened, H3) |
| verify-update.sh | PASS |
| verify-onb1.py | PASS (after hygiene fix H2) |
| verify-onb2.py | PASS (unchanged) |
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS (21/21) |
| verify-fnd3.py | PASS (46/46) |
| verify-fnd4.py | PASS (36/36) |
| soak (100 boots + 30 cuts) | PASS (fails=0; prior log archived as `logs/soak-results-2026-10-24-prefnd4.log`) |

### Script hygiene adjustments (assertions never weakened)

- **H1 — verify-shell.sh:** the `ls /data` check expected
  `hello.txt`, which was the self-test canary leaking into
  /data (delta 1 above). The script now seeds its own
  `note.txt` first and lists for it — same assertion
  strength (a real file visible in a real listing).
- **H2 — verify-onb1.py:** the flow starts from "device
  has no PIN yet"; with persistence, a `cfg-pin` left by
  any earlier run turns the create form into a login form
  and cascades (observed: 12 failing checks). The script
  now scrubs the littlefs region on the image to 0xFF
  before booting (host-side, constants mirroring
  lfs_port.c — the same helper verify-fnd4 uses). No
  assertion changed.
- **H3 — verify-firstboot.sh:** the boot-2 name check that
  was skipped with a "littlefs is RAM in QEMU" comment is
  now performed (the header already promised it): boot-2
  `setup` must show the saved name `myqaon`. The stale
  comment was replaced. This is a strengthening, per
  PLAN §FND-4.

verify-fnd4.py itself scrubs the region before its boot A
and leaves the disk unprovisioned (`setup reset`) at the
end, so the battery can run in any order.

## Honest limitations

- Geometry is frozen at 1 MB (256 × 4096 B) to keep the
  two backends interchangeable; growing /data is a
  separate decision (the NAND region is ~85 MB, so the
  hardware port will revisit geometry anyway — as a
  backend constant, not a glue change).
- BLK prog is read-modify-write per sector: more virtio
  traffic than a RAM store. Irrelevant in QEMU; the NAND
  backend programs natively.
- The RAM fallback branch is compiled and reasoned but
  not boot-tested with the blk device removed — with no
  dev 0 the bootstate path has no disk either, so that
  configuration is a robustness floor for the port, not
  a supported boot mode.
- Mid-write cuts are simulated at the QEMU-process level
  (SIGKILL mid-stream) plus littlefs's own guarantees;
  true virtio write-tearing inside a single sector write
  is not modelled (the host page cache makes sector
  writes effectively atomic here).
