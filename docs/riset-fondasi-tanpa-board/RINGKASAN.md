# Research: Board-less kernel/OS foundation gaps

Date: 2026-10-23. Scope: RESEARCH ONLY — an audit of what in the
QaonicOS kernel/OS foundation is still absent, stubbed, or partial
**and can be implemented and verified without the physical board**.
The yardstick is the out-of-the-box acceptance bar
(`docs/riset-out-of-box/RINGKASAN.md` §4): 100/100 cold boots,
100 power cuts with the filesystem intact, failed update → previous
version boots, 3 consecutive crashes → safe mode (never a
reset-loop brick), first boot offline → the device tells the user
what to do.

Fase B (B0 software side – B5.5), ONB-1 and ONB-2 are done and are
NOT re-listed here. Hardware-blocked and owner-parked items are in
the appendix so the boundary is explicit.

## 1. Method + sources checked

Documents read in full: `docs/UTANG-TEKNIS.md`,
`docs/ROADMAP-FASE13-18.md`, `docs/SYSCALL-ABI.md`,
`docs/rencana-qabot-harness/PLAN-FONDASI.md`,
`docs/rencana-uji-hardware/RENCANA.md`,
`docs/riset-out-of-box/RINGKASAN.md`; skimmed for open ends:
`docs/rencana-bootchain/PLAN-B0.md`,
`.../rencana-storage-littlefs/PLAN-B1.md`,
`.../rencana-resilience/PLAN-B2.md`,
`.../rencana-provisioning/PLAN-B3.md`,
`.../rencana-update/PLAN-B4.md`,
`.../rencana-hardening/PLAN-B5.md` + `PLAN-B55.md` +
`DEVLOG-B55.md`, `.../rencana-onboarding/PLAN.md` +
`PLAN-ONB1-ONB2.md`.

Code cross-checked (every gap below cites a file that was read,
not just a doc claim): `kernel/kernel/arm/user.c` (syscall
dispatch, daemon scheduler, display/console arbiters, spawn),
`kernel/kernel/arm/bootstate.c` (bootlog event set),
`kernel/kernel/arm/lfs_port.c` (littlefs backend),
`kernel/kernel/arm/netmain.c` (net pump call sites),
`user/sh.c` (setup/update builtins), `user/uiapp/scr_power.c`
(power menu), `user/tls/tls_port.c` (entropy),
`user/ntp.c` (private DNS client), `.github/workflows/ci.yml`,
plus TODO/FIXME/stub greps over `kernel/kernel/arm/*.c` and
`user/**/*.c` (excluding `kernel/kernel/arm/draft/` — known
hardware drafts).

## 2. Ranked gaps

Effort: S ≈ a day or less, M ≈ a few days, L ≈ a week or more /
needs a design decision first.

| # | Gap | Layer | State | Effort |
|---|-----|-------|-------|--------|
| 1 | No watchdog / health supervisor | kernel | absent | M |
| 2 | No reset / poweroff syscall | kernel | absent | S–M |
| 3 | No resource reclamation when a daemon dies | kernel | absent | S |
| 4 | littlefs backend is RAM-only in QEMU (no persistence) | kernel/tests | partial | M |
| 5 | No update-package delivery path (real file → stage) | userspace | partial | S–M |
| 6 | Factory reset not implemented | userspace | absent | S–M |
| 7 | copyin/copyout validation is VA-range only | kernel | partial | M |
| 8 | Process model v1: single spawn, no argv, FAT-only | kernel | partial | M–L |
| 9 | Persistent logging covers boot events only | kernel | partial | M |
| 10 | SYS_STAT still QAON_UNKNOWN: cpu_pct, blk_used_sec | kernel | partial | S/M |
| 11 | CI ignores the working branch; verify battery not automated | tooling/tests | partial | M |
| 12 | No userspace IPC (syscalls 10–12, 23 reserved) | kernel | absent | L |
| 13 | TLS entropy source is timer + xorshift (weak) | userspace | partial | M |
| 14 | SYSCALL-ABI.md is 28 syscalls stale | docs | stale | S |

### 1. No watchdog / health supervisor — kernel, absent, M

The only trace of a watchdog in the tree is a reserved
reset-reason code (`BSR_WATCHDOG 3`, `bootstate.c:58`). There is
no watchdog driver and no software supervisor. This matters
because daemon scheduling is **cooperative**:
`sched_yield_switch()` runs only from the `SYS_YIELD` path
(`user.c:676`) and the exit path (`sched_exit_switch`,
`user.c:1509`), and `net_pump()` — the whole network stack,
including the web server — is called only from those two switch
points (`user.c:1496`, `user.c:1512`; `netmain.c:7`). A daemon
that hangs in a loop without yielding therefore freezes the GUI
**and** networking, and nothing ever notices or recovers; the
only remedy is a physical power cycle. B2's crash counter covers
boot-time crashes only — a runtime hang does not even increment
it. A kernel supervisor thread (per-daemon progress counters,
mark crash via the existing bootstate path, then reset) is fully
implementable and testable in QEMU with a hang-injection
one-shot, the same pattern as the existing `crash` builtin and
the B4 sabotage flag. Depends on #2 for its reset action.

### 2. No reset / poweroff syscall — kernel, absent, S–M

There is no way for software to reboot or power off the device.
`user/uiapp/scr_power.c` says it in its header: Restart needs a
reset/PSCI driver that does not exist, so the menu omits it —
even though the face design (D-GUI) specifies a Power menu with
Shutdown/Restart/Sleep Now. "Shutdown" today is a black screen
plus an infinite `sys_yield()` loop (`scr_power.c`, `pwr_event`):
the CPU keeps running. The B4 flow also dead-ends here — the
shell prints `update: staged (reboot to trial)` (`user/sh.c`,
`bi_update`) but no software reboot exists to reach the trial.
QEMU `virt` offers PSCI/system-reset mechanisms, so a
`SYS_RESET`/`SYS_POWEROFF` pair is implementable and testable
board-less; on RV1103 the same syscall maps to the PMU/WDT reset
later. Unlocks #1's recovery action and the update trial flow.

### 3. No resource reclamation when a daemon dies — kernel, absent, S

Fault isolation works (a faulting daemon dies alone,
`user_fault`, `user.c:1603`), but `sched_exit_switch`
(`user.c:1509`) only marks the daemon slot dead — it never
releases what the daemon held. The display token
(`dpy_holder`, `user.c:463`) stays with a dead uiapp forever, so
the face can never render again; the console owner
(`console_owner`, `user.c:452`, set at `user.c:1138`) is likewise
never cleared if `sh` dies. (The spawn slot *is* reclaimed —
proof the pattern exists.) Result: a whole class of field
failures of the form "screen/console dead until power cycle",
on a device whose selling point is recovering by itself. The
fix is small: on exit/fault, if the dying daemon holds the
token or the console, hand them back.

### 4. littlefs backend is RAM-only in QEMU — kernel/tests, partial, M

`lfs_port.c:4`: "Backend: RAM block device (1 MB = 256 blocks x
4096 B)". Everything in `/data` — provisioning config, device
name/timezone, the ONB-2 QR secret, the B5.5 encrypted
credential store — evaporates on every reboot in emulation;
only the bootstate sector (on the blk device) persists. PLAN-B3
already noted this limit. Consequences: the ONB/B3/B5.5 flows
cannot be regression-tested *across* reboots without a board,
and every verify run starts from an empty `/data`. A persistent
backend (a region of `mach3-blk.img`, mirroring the HW draft's
littlefs region split in `draft/sfc_nand.c`) is board-less work
and is the precondition for soak-style persistence tests and
for testing #6 properly.

### 5. No update-package delivery path — userspace, partial, S–M

The signed-package machinery is complete (RSA-2048 verify in
`updverify.c`, stage via syscall 98 sub 9) but its only caller
is the `uupdsig` one-shot with a **compile-time embedded**
package. The shell's `update stage <ver>` (`user/sh.c`,
`bi_update`) only flips bootstate metadata (sub 7); no command
feeds a real package *file* from `/sd` or `/data` into sub 9.
So B4/B5.3 cannot be exercised end-to-end with an actual file
produced by `scripts/sign-update.py` — the exact artifact a
release would ship. A `stagepkg <path>` path
(read file → sub 9) closes the loop board-less.

### 6. Factory reset not implemented — userspace, absent, S–M

D-ONB-4 defines tier-2 recovery as a full factory reset (PIN,
credentials, config wiped). Nothing implements the wipe:
`setup reset` only clears the provisioned flag — PLAN-B3
(`PLAN-B3.md:28`) itself calls it "factory-reset prep". There is
no code path that erases `cfg-pin`, the `cfg-*` files and the
credstore together. The C6/BLE *trigger* is ONB-4 (hardware),
but the OS-side wipe plus shell/web entry points are board-less,
and forgotten-PIN recovery is part of the out-of-box story.
Proper testing wants #4 (persistent `/data`).

### 7. copyin/copyout validation is VA-range only — kernel, partial, M

`user_range_ok()` (`user.c:577`) checks a user pointer only
against the fixed window `[0x100000, 0x130000)`.
`docs/SYSCALL-ABI.md` admits this honestly ("cek range VA, bukan
walk page table"). The window contains unmapped gaps (between
the end of a program's code pages and the stack page, etc.); a
buggy or hostile program that passes a pointer into a gap makes
the *kernel* take the fault mid-copy — undermining the fault
isolation that `user_fault` otherwise guarantees. Fix by
validating against the caller's pmap or by making kernel copies
fault-recoverable so a bad pointer returns -1. Board-less,
testable with a deliberately bad-pointer one-shot.

### 8. Process model v1 — kernel, partial, M–L

`SYS_SPAWN` (`user.c:1036` ff.) supports exactly **one** active
spawn (a second returns -1), a 64 KB image cap, **no argv**, and
loads the image only from FAT32 (`fat32_mounted()` required) or
the `embed:uls` test hook. In the product configuration there is
no FAT32 — the store is littlefs — so spawn is unusable on the
real device layout, and any future app/tool extensibility for
Qabot sits on this. Generalising (spawn from `/data`, argv via a
shared page, more than one child) is board-less design + code.

### 9. Persistent logging covers boot events only — kernel, partial, M

The bootlog ring (16 slots, `bootstate.c`) records lifecycle
events only (BOOT, HEALTHY, SAFEMODE, CRASHMARK, PROVISIONED,
PROV_RESET, ROLLBACK, UPD_STAGE/TRIAL/CONFIRM/CANCEL). Runtime
diagnostics — e.g. `sched: '%s' FAULT 0x%x (diisolasi)` — go to
the UART and are lost on a headless product device. Out-of-box
P0 #4 was "reset-reason capture **+ persistent logging**"; the
reason half is done, the general half is not. A small persistent
runtime log (ring in littlefs or spare blk sectors, exported via
the shell like `bootlog`) is board-less.

### 10. SYS_STAT unknowns — kernel, partial, S (blk) / M (cpu)

`SYS_STAT` still hard-codes `cpu_pct = QAON_UNKNOWN` and
`blk_used_sec = QAON_UNKNOWN` (`user.c:745`, `user.c:751`); the
comments even still claim "belum ada FS di disk", stale since
Fase D/B1. The Monitor screen and qfetch therefore display
"unknown" for CPU on a device meant to look finished.
`blk_used_sec` is now computable from littlefs/FAT accounting;
`cpu_pct` needs idle-time accounting in the scheduler.

### 11. CI / test automation — tooling, partial, M

`.github/workflows/ci.yml` triggers only on push/PR to branch
`core`, while all current work lands on `qabot/q2-tls-provider`
(140 commits) — the active branch has **no CI at all**. CI also
checks only 7 boot self-test markers; the real regression
battery (`scripts/verify-shell.sh`, `verify-safemode.sh`,
`verify-firstboot.sh`, `verify-update.sh`, `verify-onb1.py`,
`verify-onb2.py`, `scripts/soak-test.py`) runs only by hand, and
is known to flake when run back-to-back over shared disk state.
One local runner (fresh images per suite) + CI on the working
branch closes the hole that let e.g. the littlefs allocator leak
survive until ONB exposed it.

### 12. No userspace IPC — kernel, absent, L

`docs/SYSCALL-ABI.md` reserves 10–12 (`SYS_SEND`/`SYS_RECV`/
`SYS_RPC`) and 23 as "belum"; no such defines exist in `user.c`.
Programs coordinate through ramfs sentinel files, shared kernel
state (the face-request slot behind syscall 78), and the
web-shell bridge state machine. Mach ports exist underneath but
there is no user facade. Mode B tool dispatch and the C6 link
handler will need a real channel; listed for completeness —
this one wants a design decision before code.

### 13. TLS entropy is timer + xorshift — userspace, partial, M

`user/tls/tls_port.c:330` labels its own `mbedtls_hardware_poll`
"LEMAH (weak): timer + xorshift. Test only." UTANG-TEKNIS §4
assigns the real fix to the RV1103 TRNG (hardware). A board-less
mitigation exists — a kernel entropy-pool syscall fed by timer
jitter, network RX timing and boot count, or a QEMU virtio-rng
backend — which improves dev/test realism. It must not be
oversold: it is not production entropy, and the doc trail should
keep saying so.

### 14. SYSCALL-ABI.md is 28 syscalls stale — docs, stale, S

The ABI doc's tables end at syscall 77. `user.c` carries 67
`SYS_*` defines up to 105: 78 (face poll), 81 (console takeover),
82–88 (audio), 89–92 (PL330), 93–97 (littlefs), 98 (bootstate),
99 (bootlog), 100–101 (time quality/sync), 102 (GUI start),
103 (web UI), 104 (credential store), 105 (event inject) are all
undocumented there. This doc is the first thing any new consumer
— the C6 protocol work, a second coding agent — reads.

## 3. Shortlist: the 5 highest-leverage items

1. **Watchdog / health supervisor (#1).** The acceptance bar
   promises recovery, but today that promise covers boot only. A
   single hung daemon freezes the face *and* the network stack
   (both advance only at cooperative switch points) with no
   detection and no way back short of pulling the plug.
2. **Reset / poweroff syscall (#2).** Small, and it unlocks
   three things at once: the watchdog's recovery action, the B4
   update-trial reboot, and the Restart item the Power menu spec
   already promises. Fully testable in QEMU via PSCI.
3. **Resource reclamation on daemon death (#3).** The smallest
   fix on this list with an outsized effect: it deletes the
   "dead screen / dead console until power cycle" failure class
   that fault isolation alone leaves behind.
4. **Persistent littlefs backend in QEMU (#4).** Test
   infrastructure, not product code — but it is the precondition
   for regression-testing provisioning, credentials and factory
   reset *across reboots* without a board, which is where the
   out-of-box claim will actually be proven or lost.
5. **Update-package delivery path (#5).** B4/B5.3 built a
   verified, signed update pipeline that no real file can enter.
   One shell path (file → syscall 98 sub 9) makes the whole
   update story exercisable end-to-end today, using the existing
   `scripts/sign-update.py` output.

## 4. Checked and already covered (not gaps)

- **PLAN-FONDASI F4 repo hygiene** — done: both research docs are
  tracked (`git ls-files` shows `BEDAH-DEVIN-ARCHITECTURE.md`,
  `VALIDASI-TRANSCEND-RAILS.md`) and the `origin` remote exists.
- **UTANG-TEKNIS §5 residual** ("shell `ls`/`cat` still can't
  read the filesystem") — stale note: `user/sh.c` `ls`/`cat`
  handle both `/sd` (FAT32) and `/data` (littlefs) since B1.5.
- **F2 wall-clock bug** — resolved (UTANG-TEKNIS §2): the reported
  bug never existed; the `tls_time_get()` hardcode was removed;
  `UTIMESET` regression runs every boot.
- **F1 TCP keep-alive** — closed as an environment (slirp)
  limitation with `Connection: close` as the official dev/test
  pattern; guest RX logic verified correct. Backlog by decision,
  not an open defect.
- **DNS** — a private A-record client exists inside
  `user/ntp.c` (App A4); NTP's own need is covered. (A general
  resolver is absent but nothing board-less currently requires
  one beyond NTP.)
- **Reset-reason capture** — done in B2: bootstate records the
  reason (power/crash/watchdog codes) and the panic note
  (B2.7). Only the watchdog *source* (#1) and runtime logging
  (#9) are missing.
- **Fault isolation** — done: `user_fault` kills only the
  faulting daemon (App A2). Recovery after isolation is #3;
  hang (not fault) detection is #1.
- **Spawn wait/exit codes** — `SYS_SPAWN_WAIT` (80) works (Q9);
  the limits are #8, not absence.

## 5. Appendix: out of scope for board-less work

### Hardware-blocked (need the Luckfox Pico Mini B / C6)

- B0.4 flash test of `update-final.img` on the real board.
- SPI NAND (SFC) driver bring-up — draft in
  `kernel/kernel/arm/draft/sfc_nand.c`; littlefs region split
  already designed there.
- SPI display (ST7789) bring-up — drafts `draft/spi0.c`,
  `draft/spi_lcd.h`, `draft/st7789.h`; H1.3.
- ONB-3 (C6 BLE provisioning), ONB-4 (C6 SoftAP captive portal),
  ONB-5 (Android setup wizard).
- ESP32-C6 firmware FW-1..FW-3 and the H2 link test suite
  (921600 baud integrity, WIFI.*, NET.HTTP, VOICE, TOOL.DISPATCH).
- OTP-wrapped credential-store device key (B5.5 follow-up; the
  seam `credstore_device_key()` is ready).
- PIN login throttle counters are RAM-only; persistence follows
  the on-device littlefs (and board-less, item #4).
- RV1103 TRNG driver for production TLS entropy (UTANG-TEKNIS
  §4); audio CRU clock values (TODO(H4) in `audio.c`); PL330
  DMAGO execution (structure done, hardware step remains).
- Hardware test suites H1–H4 (`docs/rencana-uji-hardware/`).

### Environment-blocked (sandbox, not the board)

- NTP end-to-end: the sandbox blocks UDP, so F2's last checkbox
  stays open until H1.1. Note the `ntp` daemon is currently
  commented out of `daemon_images[]` (`user.c:1954`,
  "Nonaktif sementara Q2b") and must be re-enabled for that test.

### Parked by owner decision (do not start)

- B5.4 secure boot — DEFERRED 2026-10-21 (`PLAN-B5.md` §B5.4).
- LLM API key / Qabot Mode A live test — skipped 2026-10-21.
- Multi-user / multiple PINs — D-ONB-7, deferred 2026-10-21.
