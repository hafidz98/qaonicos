# B2 Plan: Resilience — boot counter, safe mode, persistent boot log

Date: 2026-10-20. Phase B2 of the out-of-the-box roadmap.
Goal: a failed boot can never loop forever or brick the device;
every boot leaves a persistent, inspectable record.

## Design
- **State sector**: second-to-last sector of blk dev 0 (the last is
  the blk selftest scratch). Struct: magic, boot_count, fail_count,
  last_reason, checksum. On hardware this moves to a NAND env
  partition; the logic is identical.
- **Boot flow**: `bootstate_init()` (in user_launch_init, blk ready):
  read state -> boot_count++, fail_count++ -> persist -> if
  fail_count >= 3, safe mode is armed for this boot.
- **Healthy mark**: one-shot `uhealthy` runs last in the one-shot
  sequence; user_launch_init marks healthy only if all one-shots
  passed (fails == 0). Healthy = fail_count reset to 0.
- **Safe mode**: daemons face/uiapp/qabotd are skipped; shell only.
  Banner printed at boot. Recovery = one healthy boot.
- **Persistent boot log**: ring of 16 records x 512B in the sectors
  just below the state sector. Events: BOOT, HEALTHY, SAFEMODE,
  CRASHMARK. Shell builtin `bootlog` prints the history.
- **Reset reason**: state.last_reason (0 unknown, 1 power,
  2 crash/panic, 3 watchdog). Panic path notes reason best-effort.
- **Shell**: `bootctl` shows state; `bootctl crash` arms a simulated
  crash (fail_count=2 persisted, so next boot enters safe mode).

## Syscalls
- 98 SYS_BOOTSTATE: a0=sub: 0=info, 1=healthy, 2=crash-sim.
- 99 SYS_BOOTLOG: a0=index -> record; a0=0xFFFFFFFF -> count.

## Tasks
- [x] B2.1 bootstate.c (state sector + log ring + init/healthy).
- [x] B2.2 Safe-mode daemon gating + banner.
- [x] B2.3 uhealthy one-shot + healthy gating on one-shot fails.
- [x] B2.4 Shell builtins bootctl/bootlog + syscalls 98/99.
- [x] B2.5 Panic-path reason note (best effort).
- [x] B2.6 Multi-boot verification script (3 boots: crash -> safe
      mode -> healthy -> normal). Marker: SAFEMODE-VERIFY: PASS.

## Out of scope (hardware)
- Real RV1103 watchdog timer (register base needs TRM).
- Reset-reason register read (PMU/GRF) on silicon.

## Verification results (2026-10-20)
- SAFEMODE-VERIFY: PASS x2 (crash -> safe mode -> healthy -> normal;
  bootlog contains the SAFEMODE event record).
- SHELL-VERIFY: PASS 9/9 (no regressions).
- Stdio boots x3: UBLOGTEST PASS (16/16 records), ULFS PASS, no panic.

## Bugs found & fixed along the way
1. blk_request completion wait: 100M-spin limit ~= 100 s (looked like
   a hang) and desynced the queue after timeout. Now 3M spins,
   re-notify halfway, resync used_idx on timeout (blk.c).
2. bootstate_log_get struct copy to a possibly unaligned user
   buffer -> alignment abort in syscall path. Now byte copy.
3. Shell bi_bootlog read a char[64] buffer as unsigned* -> data
   abort 0xdab7 killed the shell when the buffer landed unaligned.
   Now a static unsigned int[16].
4. bs_log/bs_persist cleared only sizeof(pointer)=4 bytes of the
   shared sector buffer after the static-buffer refactor -> stale
   note tails ("bootone-shots passed"). Now sizeof(bs_sector_buf).
   (Lesson: sizeof on a pointer is silent; grep after refactors.)

## B2.7 addendum (2026-10-20): torn boot-log records
Found by the 100-boot / 30-power-cut soak test: cold boots
100/100 PASS, but the post-cut check failed -- one boot-log
record was torn by a power cut landing mid-sector-write, and
with no per-record checksum it failed reads permanently.
Fix: bs_rec gains a checksum (note shrinks 40 -> 36 bytes);
readers (bootstate_log_count/get) validate magic + checksum +
expected seq and SKIP torn records; the ring overwrites them
within 16 boots (self-healing). UBLOGTEST now also asserts
strictly increasing seq across returned records.
The state sector already had this property (checksum + reset
fallback) and survived all 30 cuts intact (boot #199 history
continuous).

## Also found by the soak regression (2026-10-20)
uprog_udesc[] was sized UPROG_MAX=16 but indexed up to
NUPROGS+NDAEMON_IMAGES; once NUPROGS reached 20 the daemon
udesc writes corrupted scheduler statics and the shell daemon
failed to register (no qaon> prompt). Sized structurally now
(user.c). Latent since the program count passed 16; the soak
caught it because it boots the CURRENT build 100 times.

## B2.8 addendum (2026-10-20): transient blk timeouts
Soak run 2: 98/100 cold boots PASS, final integrity check PASS
(B2.7 holds), but boots 23 and 35 lost UBLOGTEST: count came
back 0. Reproduced and captured: bootstate_init's first state
read hit a transient blk timeout (host CPU contention makes
the spin-based completion wait expire early), which silently
disabled bootstate for that whole boot. Fix: all bootstate
I/O goes through bs_read/bs_write with 3 attempts, and an
unreadable state sector now prints a WARN instead of failing
silently. Validation: 40/40 boots PASS under the same load;
full soak re-run follows.
