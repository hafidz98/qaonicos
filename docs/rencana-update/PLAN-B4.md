# B4 Plan: Update manager -- A/B slots, trial boot, auto-rollback

Date: 2026-10-20. Phase B4 of the out-of-the-box roadmap.
Goal: an update can never brick the device. A new image gets one
trial boot; if that boot does not confirm itself healthy, the
previous slot comes back automatically.

## QEMU scope
The slot/decision logic + persistence are real; the "slots" are
simulated (no second kernel image is booted in QEMU). On hardware
the same state machine drives the U-Boot boot-count/slot choice
and the NAND boot partitions (boot A/B) from PLAN-B0.

## State (extends the B2 boot state, checksummed)
- cur_version: version of the running slot.
- active_slot: 0=A, 1=B.
- upd_state: 0=idle, 1=staged (pending trial), 2=trial.
- pending_slot + pending_version for the staged update.

## Flow
1. `update stage <ver>` (shell): records the staged update,
   state=staged, event STAGE. (Image transfer itself is the
   transport's job: C6/OTA later; here the version + a manifest
   file on littlefs stand in for the image.)
2. Next boot (bootstate_init): staged -> active_slot flips to
   pending_slot, state=trial, event TRIAL.
3. Trial boot: if all one-shots pass, bootstate_healthy() also
   confirms the update: cur_version=pending_version, state=idle,
   event CONFIRM.
4. If the trial boot never confirms (crash/failing one-shots),
   the NEXT bootstate_init sees state=trial unconfirmed ->
   rollback: active_slot reverts, cur_version unchanged,
   state=idle, event ROLLBACK. The device is back on the old
   image without any user action.
5. Failure injection for testing: `update sabotage` arms a
   one-shot flag (bootstate flags bit1); user_launch_init then
   skips the healthy mark exactly once, simulating a bad update.

## Syscalls (SYS_BOOTSTATE subs)
- 6 = update info (cur_version, active_slot, upd_state,
  pending_version into a 4-word buffer)
- 7 = stage update (a1 = version)
- 8 = arm sabotage (test only)

## Tasks
- [x] B4.1 bootstate update fields + on-init transitions +
      confirm-in-healthy + sabotage flag.
- [x] B4.2 Syscall subs 6/7/8; sabotage hook in
      user_launch_init's healthy marking.
- [x] B4.3 Shell `update` builtin (status/stage/sabotage).
- [x] B4.4 scripts/verify-update.sh: stage v2 -> trial ->
      confirm; stage v3 + sabotage -> trial unconfirmed ->
      rollback to v2. Marker: UPDATE-VERIFY: PASS.

## Out of scope (later/hardware)
- Real image transfer + signature verification (B5), U-Boot
  slot switching on NAND, OTA download via C6.

## Verification results (2026-10-20)
- UPDATE-VERIFY: PASS (5-boot cycle: stage v2 -> trial ->
  confirm v2/B; stage v3 + sabotage -> trial unconfirmed ->
  rollback to v2/B; bootlog holds the ROLLBACK event ev=7).
- Regressions: SHELL-VERIFY, SAFEMODE-VERIFY, FIRSTBOOT-VERIFY
  all PASS.
