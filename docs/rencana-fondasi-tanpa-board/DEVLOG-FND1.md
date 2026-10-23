# Dev Log: FND-1 — Reset / poweroff syscall

Date: 2026-10-23. Spec: `docs/rencana-fondasi-tanpa-board/
PLAN.md` section FND-1 (audit item #2). Status: implemented
and verified in QEMU; not yet committed (coordinator review
pending).

## What was built

- **Kernel — syscalls 106/107** (`kernel/kernel/arm/user.c`):
  `SYS_RESET` = 106, `SYS_POWEROFF` = 107 (next free numbers
  after `SYS_EVENT_INJECT` 105, confirmed against the
  dispatch). Both handlers first record the reset reason in
  bootstate, print a kernel line ("sys: SYS_RESET /
  SYS_POWEROFF (software request)"), then invoke PSCI. A
  return from PSCI means the platform refused; the syscall
  then returns -1 to userspace.
- **Bootstate reason 4 = software request**
  (`kernel/kernel/arm/bootstate.c`): new `BSR_SOFTWARE` (4)
  alongside 0 unknown / 1 power / 2 crash / 3 watchdog, new
  boot-log event `BLE_RESET` (12), and
  `bootstate_note_software_reset(uptime_ms, poweroff)` which
  persists `last_reason = 4` and logs ev=12 with the note
  "software reset" or "software poweroff" BEFORE the
  platform acts, so the next boot tells the truth through
  three independent paths: the kernel boot line
  (`reason=4`), `bootctl` (`last_reason=4`), and the
  `bootlog` record itself. The shell's bootctl legend now
  documents code 4.
- **PSCI driver** (`kernel/kernel/arm/psci.c`, new):
  `psci_system_reset()` / `psci_system_off()` wrapping
  SYSTEM_RESET (0x84000009) / SYSTEM_OFF (0x84000008). The
  RV1103 mapping (PMU software reset / watchdog fallback,
  PMU power-down) sits behind the same two prototypes as a
  clearly marked TODO(HW) seam.
- **ulib** (`user/ulib/ulib.c`, `ulib.h`): `sys_reset()` /
  `sys_poweroff()`, following the `sys_event_inject`
  pattern. Contract: on success they never return.
- **Shell** (`user/sh.c`): builtins `reboot` and
  `poweroff` (print a request line, call the wrapper, print
  "failed (platform refused)" only if it returns) + one help
  line each. The sh image stayed under the 65536 embed cap
  (build green, no cap change needed).
- **uiapp Power screen** (`user/uiapp/scr_power.c`): menu
  is now the D-GUI order **Shutdown / Restart / Sleep Now /
  Back**. Shutdown flushes a "SHUTTING DOWN" screen and
  calls `SYS_POWEROFF` for real (replacing the black-screen
  infinite yield loop); Restart flushes "RESTARTING" and
  calls `SYS_RESET`. The yield-forever loop remains only as
  the fallback if a power syscall ever returns (platform
  refusal). Sleep Now keeps the existing display-sleep path
  (`SYS_DISPLAY_SLEEP(1)` + token release).
- **Docs**: `docs/SYSCALL-ABI.md` gained only the two new
  rows (106/107) in a new "Syscall power (FND-1)" section;
  the 28-stale backfill stays backlog per plan.
- **Verification script**: `scripts/verify-fnd1.py` (see
  below).

## PSCI conduit finding: HVC (evidence)

The virt machine's firmware advertises the conduit itself.
Dumping the DTB of the toolchain QEMU 8.2.2
(`-M virt -cpu cortex-a7 -machine dumpdtb`) shows a `psci`
node (compatible "arm,psci-1.0", "arm,psci-0.2", "arm,psci")
with a `method` property; the literal string `hvc\0`
appears in the DTB (offset 272) and `smc\0` appears
nowhere in it. Runtime confirmation (verify-fnd1.py, below):
with HVC, `reboot` produced a genuine second boot and
`poweroff` terminated QEMU with exit code 0. SMC was
therefore not needed.

One build surprise: the kernel compiles with
`-march=armv7-a` (no `+virt`), so clang's assembler rejects
`hvc` ("instruction requires: virtualization-extensions").
Fixed locally in `psci.c` with an `.arch_extension virt`
directive inside the asm block (verified the emitted
encoding is `e1400070` = HVC #0); no global build flags
were touched.

## Verification

Build: `MD: 42 ok, 0 failed`, `LINK OK`.

`scripts/verify-fnd1.py` — **FND1-VERIFY: PASS**, all 16
checks:

- boot reaches qaon> prompt — PASS
- reboot: second boot on same disk — PASS
  (boot_count 730 -> 731)
- reboot: boot_count incremented by 1 — PASS
- reboot: kernel boot line reason=4 — PASS
  (`bootstate: boot #731 fail=1 reason=4 safe=0`)
- reboot: kernel logged SYS_RESET — PASS
- second boot reaches qaon> prompt — PASS
- bootctl shows last_reason=4 — PASS
- bootlog has ev=12 'software reset' for the requesting
  boot — PASS (`boot #730 ev=12 fail=0 up=1600ms
  software reset`; the same log also shows an earlier
  `software poweroff` record, so the two notes are
  distinguished correctly)
- Power screen: screendump saved (`logs/fnd1-power.png`),
  SHUTDOWN / RESTART / SLEEP NOW / BACK label bands all
  rendered (glyph pixels 128 / 111 / 127 / 63) — PASS x5;
  the frame was also inspected visually (status bar +
  POWER title + 4 items, SHUTDOWN selected)
- poweroff phase (separate boot): prompt reached, QEMU
  exits on its own, exit code 0, kernel logged
  SYS_POWEROFF — PASS x4

Regression battery (each run standalone):

- SHELL-VERIFY: PASS
- SAFEMODE-VERIFY: FAIL on the first run ("boot3: still in
  safe mode after healthy boot" — the safe-mode boot's
  healthy mark evidently flaked), PASS on the immediate
  standalone rerun with the textbook sequence (#737
  healthy -> #738 safe_mode=1, fail_count recovered to 0 ->
  #739 safe_mode=0). This is the previously documented
  shared-disk flake for this script, not an FND-1 change:
  nothing in FND-1 touches the fail_count/safe-mode path.
- FIRSTBOOT-VERIFY: FAIL on the first run (boot1 still
  provisioned from the preceding test's end state), PASS
  on the standalone rerun — same documented state flake.
- UPDATE-VERIFY: PASS
- ONB1 (verify-onb1.py): all checks PASS (20 PASS lines),
  ONB1-VERIFY: PASS
- ONB2 (verify-onb2.py): ONB2-VERIFY: PASS (4/4) — after
  repairing the environment: the VM was replaced mid-task,
  wiping pip packages (`zxingcpp` missing) and leaving a
  stale QEMU holding the disk lock; both were fixed
  (reinstall + kill by PID) and the clean rerun passed.

## Surprises / notes

- The VM replacement mid-task (exec service restart) cost
  one smoke-test run; all durable state is in the repo, so
  recovery was just a rebuild (not needed — the ELF
  survived) plus the pip reinstall above.
- `fail_count` needs no special handling on a software
  reboot: the shell only exists after the boot's healthy
  mark, so the counter is 0 when the reason is persisted
  and the next boot's increment is cleared by its own
  healthy mark — observed live (boot #731 fail=1 at init,
  back to 0 after one-shots).
- A staged-but-untrialled update + `reboot` still rolls
  back on the next boot by design (B4); FND-1 does not
  change update semantics.
