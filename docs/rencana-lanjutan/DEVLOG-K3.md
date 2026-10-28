# DEVLOG-K3 — virtio-blk request robustness (late-completion cascade)

Track: board-less follow-up, item K3 (PLAN.md §K3).
Branch: `dev/k3`. Base: K2 merge (`2f2ffb6`, push 266).
Status: implemented, fault-injection proven, post-change
boot-count 30/30 clean; battery in progress (§7, filled when
the chain finishes).

## 1. Premise correction: the failure is NOT a probe race

PLAN.md §K3 describes a probe race: the SD probe loses, the boot
ends with `nblkdevs=1`, `fat32_mount()` fails, every FAT syscall
fails, and only SHELL `ls` catches it. That theory was never
proven — no failing boot's early log had ever been captured — and
the boot-count evidence gathered for this item disproves it as
the dominant class:

- Pre-change baseline, 30 boots (`scripts/verify-k3.py --tag pre`):
  **29 OK, 1 OTHER, 0 probe losses.** The single failure (boot 23)
  probed 2 devices, mounted `/sd`, passed the SD one-shot, then
  failed `ufs` (`ufs FAIL: fat_read/verify`) and shell `ls`.
- Two further pre-change 30-boot runs with per-request tracing
  (tags `diag`, `trace`, `geom`): 28/27 OK, the rest OTHER, still
  **0 probe losses and 0 signature misclassifications** in 120
  boots total.

The proven failure class is **runtime request loss AFTER a good
probe and a good mount**: the device processes early requests
fine, then a later request fails, and a later `ls` fails too.
The fix therefore targets the request path (plus probe /
signature / mount retries as defense-in-depth), and the
original probe-race premise is recorded here as corrected.

## 2. Root cause: one late completion poisons the next request

The evidence chain, from per-request traces of failing boots
(`~/workspace/logs-k3/{diag,trace,geom}-boot-*.log`):

1. A request times out (lost kick or slow device under host
   load): e.g. SD WRITE sector 100 (inside FAT1) completes
   nothing within 3M spins (`DIAG TIMEOUT ... used=3->3`).
   The request stays queued; the driver resyncs `used_idx` and
   returns -2.
2. The next request's poll exits on the late completion: its
   `used` advances by exactly 1 (the timed-out request's
   completion, in order), e.g. READ sector 100 returns `rc=0`
   on the WRITE's completion — with **stale `req_data`**
   (the previous sector's bytes disguised as the requested
   sector).
3. Worse, when a late completion lands *between* two requests
   (after the previous return, before the next request's setup),
   the next request's `req_status = 0xff` setup plus pre-kick
   cache clean **clobbers the late completion's status byte in
   RAM**; the next poll exits immediately on that completion
   and reads `st=255` (`DIAG STATUS ... st=255 used=4->5`).
   The used entry's `len` confirms the completion is genuine
   (513 = data + status for a read, 1 = status only for a
   write) — the device did its work; the driver misattributed
   it.
4. The affected request itself is still queued and completes
   during a later request's poll, so the corruption cascades
   one request at a time until the device happens to drain the
   queue before the guest's first poll read (which is why most
   boots recover and only some fail a visible operation).

Theories eliminated by the traces:

- Cache-line sharing: the post-poll invalidates are ordered
  after the idx advance, and under QEMU TCG cache ops are NOPs
  (host RAM is shared coherently).
- Struct/queue overlap: the geometry print shows per-device
  `vq_block` bases 0x4000 apart, `used` pointers 0x4000 apart,
  no overlap; the `desc[2]` address always equals the owning
  device's `&req_status`.
- Stale completion: every STATUS request shows a used-delta of
  exactly 1, never 2 — each failing poll exited on exactly one
  new completion.

The single design flaw: the old poll exited on **any** used-index
movement (`*uidx != used_idx`), so a request could return on
another request's completion. Everything else (single shared
request buffers, `req_status = 0xff` setup, cache cleans) only
turns that misattribution into visible corruption.

## 3. Design

### 3.1 Wait for your own completion: `used == avail`

New helper `blk_wait_complete()` (`mach3-port/src/ext/blk.c`)
replaces the old poll. It waits until the device's used index
equals the driver's avail index — i.e. **every** submitted
request, including this caller's, has completed — instead of
exiting on any movement. Consequences:

- The last completion observed is always the caller's own,
  written after its own setup, so the status byte and the data
  buffer read after the wait are always its own.
- A late completion from an earlier request is simply waited
  out (it advances `used` toward `avail`, not past it).
- A genuinely dead device now fails loudly on every later
  request (each poll times out) instead of corrupting data
  silently. This trade-off is deliberate and documented here.

Wrap-around is safe: both indices advance by 1 per request and
are compared as `unsigned short`; at most a handful of requests
are ever outstanding, far below the 65536 wrap distance.

### 3.2 Lost-kick recovery: bounded multi-window polling

`blk_wait_complete()` polls up to `BLK_WAIT_ATTEMPTS` (3)
windows of `BLK_WAIT_SPINS` (3M spins) each, re-notifying once
halfway through each window and once at the start of each
further window. A lost kick is recovered by the re-notify; a
slow device is recovered by the extra windows; no request is
ever re-submitted, so the queue can never hold two copies of
one operation (writes of identical data would be harmless, but
duplicates are avoided entirely).

### 3.3 Defense-in-depth retries (bounded, cheap, quiet)

- Slot init (`blk_init`): up to 3 attempts per slot, with an
  uncalibrated ~ms-scale spin settle between attempts.
  `blkdev_init_one` is idempotent (STATUS reset first, queue
  rebuilt), so re-running it is safe. Retry lines print only
  when a retry actually happens; clean boots print nothing new.
- Signature read (`blkdev_is_fat32`): up to 3 attempts. This
  read decides the dev map (Fase 15: dev 0 = internal,
  dev 1 = SD, by FAT32 boot signature, never slot order);
  a transient loss must not misclassify the SD card.
- Mount (`machine_init` → `fat32_mount`): up to 3 attempts.
  Mount is idempotent (state reset, geometry re-read).
  No FAT semantics, no mount validations, and no daemon-table
  entry changed.

### 3.4 Worst-case added boot time

Normal boots pay **zero** added time: the first attempt
succeeds on every path, and the new poll condition costs no
extra spins. Added time accrues only when the device is
already failing:

- Per stuck request: up to 3 windows. The B2 note measured
  100M spins ≈ 100 s, i.e. 3M spins ≈ 3 s, so ≈ 9 s per stuck
  request in the worst case (observed TCG boots complete the
  windows faster; the bound is what is documented).
- Per failed slot init: 2 settle spins (sub-second).
- Per failed mount: up to 3 mount attempts, each bounded by
  its requests' windows — ≈ 27 s worst case for a genuinely
  dead SD before mount gives up, versus silent data corruption
  today. A dead device failing loudly after ≤ ~30 s is the
  intended behaviour.

## 4. What the fix does NOT cover

- A device that completes requests with a wrong status byte or
  corrupt data (device-side corruption, not misattribution).
- A device that never completes and never errors: every later
  request times out (loud failure, §3.1) rather than
  recovering; recovery from a wedged queue would need a queue
  reset, which virtio-mmio legacy does not provide cleanly.
- The one-shot UFS pattern test remains the canary for any
  residual data-path issue; it is not replaced.

## 5. Fault-injection proof (scratch builds, never shipped)

Both injections targeted the first SD request of a boot
(sector-0 signature read) and were fully reverted afterwards
(`git diff` verified free of `INJECT` before the clean build):

- **Inject-A (dropped kick):** the initial QNOTIFY write was
  skipped for the first SD request only; re-notifies stayed
  active. Result: 2/2 boots OK (mounted, `ls` ok); the request
  completed via the halfway re-notify. Proves lost-kick
  recovery inside a single request.
- **Inject-B (forced full timeout):** kick *and* all
  re-notifies were suppressed for the first SD request only,
  so it timed out after all 3 windows (log line
  `INJECT-B request timed out (expected)` fired on both
  boots). Result: 2/2 boots OK — the `blkdev_is_fat32` retry
  recovered with the correct device map, mount succeeded,
  `ls` ok. Proves timeout + retry recovery end-to-end.

The committed code contains no test hooks (`grep INJECT` = 0).

## 6. Boot-count evidence

`scripts/verify-k3.py`: fresh COPIES of both disk images per
boot, serial socket + logfile transcript from byte 0 (no `-S`
pause, so timing is unperturbed), `ls` after the `qaon>`
prompt, classes OK / PROBE_LOSS / SIG_LOSS / MOUNT_READ_LOSS /
OTHER. Exit 0 iff zero probe/mount/`ls` failures.

Pre-change (kernel at K2 merge, `2f2ffb6`):

| Tag | Boots | OK | OTHER | Probe loss |
|-----|-------|----|-------|------------|
| pre | 30 | 29 | 1 (boot 23: `ufs FAIL`, then `ls` failed) | 0 |
| diag | 30 | 28 | 2 | 0 |
| trace | 30 | 28 | 2 | 0 |
| geom | 30 | 27 | 3 | 0 |
| **Total** | **120** | **112** | **8** | **0** |

Post-change (fixed kernel):

| Tag | Boots | OK | Failures |
|-----|-------|----|----------|
| post | 30 | 30 | 0 |

The fixed kernel's 30/30 clean run covers the same failure
window the pre-change runs measured (120 boots, 8 OTHER
failures, all runtime request loss). The fault-injection
proofs (§5) carry the mechanism evidence; the boot count
carries the statistical evidence.

## 7. Battery (split PROD / QA)

PROD image legs (fixed kernel, production build):

| Leg | Result |
|-----|--------|
| K1 | PASS 7/7 |
| UI5 | PASS 73/73 |
| UI4 | PASS 73/73 |
| UI3 | PASS 78/78 (incl. teardown restores shared PIN 246810) |
| UI2 | PASS 38/38 |
| UI1 | PASS 25/25 |
| FND1 | PASS |
| FND2 | PASS 21/21 |
| FND3 | PASS 46/46 |
| FND4 | PASS 36/36 |
| FND5 | PASS 33/33 |
| ONB1 | PASS |
| ONB2 | PASS |
| SHELL | PASS |
| SAFEMODE | PASS |

QA=1 image legs (Tier A test programs are QA-only):

| Leg | Result |
|-----|--------|
| K2 | PASS 21/21 |
| C6L1 | PASS 29/29 |
| C6L2 | PASS 36/36 |
| C6L3 | PASS 72/72 |
| C6L4 | PASS 60/60 |
| C6L5 | PASS 56/56 |
| IPC2 | PASS 34/34 |
| IPC4 | PASS 58/58 |
| IPC5 | PASS 20/20 |

Notes:

- K2 was first attempted on the PROD image and failed at
  `ulinktools started` — not a regression: `verify-k2.py`'s
  docstring requires the QA=1 image (ulinktools is QA-only).
  On QA it passes 21/21. The aborted PROD attempt was killed
  and its QEMU + mock peer cleaned up (no leftovers).
- C6L5 reports 56/56 from its own verdict line; the K3 task
  text listed 55/55 (carried from an older count). The script
  verdict is authoritative; the one-check difference is a
  stale expectation in the task text, not a failure.
- ONB1/ONB2 run after UI3 in the chain and log in with PIN
  246810, confirming the shared disk PIN state after UI3's
  teardown restore. No QEMU or mock peer left running after
  either chain (verified by exact-name `pgrep`; the one
  apparent peer hit was a self-match of the checking shell
  command itself).
- Full per-leg logs: `~/workspace/logs-k3/battery-prod/` and
  `~/workspace/logs-k3/battery-qa/` (scratch, not committed).

## 8. Files changed

- `mach3-port/src/ext/blk.c`: `blk_wait_complete()` (used ==
  avail, 3 windows x 3M spins, re-notify), `blk_request` uses
  it, slot-init retry (3x), signature-read retry (3x).
- `mach3-port/src/md/arm/machdep.c`: mount retry (3x).
- `scripts/verify-k3.py`: boot-count harness (classes +
  transcripts under `~/workspace/logs-k3/`); docstring updated
  with the corrected diagnosis.
- `docs/rencana-lanjutan/DEVLOG-K3.md`: this file.

Constraints honoured: upstream untouched; FAT32-signature dev
map (never slot order); FAT semantics, mount validations, and
daemon table unchanged; linkd untouched (94% embed cap);
no test hooks in committed code.
