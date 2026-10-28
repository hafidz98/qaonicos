# DEVLOG-K5 — Live radio-state wiring for the status-bar icons

## 1. The gap

The status bar shows Material Symbols wifi/BT glyphs (UI-8),
but the only call site, `uilvgl_set_radio_icons()` in
`user/uiapp/lv_port.c`, was called with `(0, 0)` from the
shell's `build_status_bar()`: icons always OFF. The real
radio state lives in linkd (`uproto` WIFI.STATUS /
BLE.STATUS — the same calls the wifi/ble screens make).
UI-7/UI-8 deferred the wiring because a blocking RPC in
the menu-build path would stall menu appearance whenever
linkd is troubled. K5 wires it without paying that cost.

## 2. Poll/cache design (the core of this item)

Owner: uiapp (the LVGL shell), the only writer of the icon
labels after build. No new daemon, no new thread, no new
IPC op, no linkd change, no protocol change.

- Cache: two static ints in `user/uiapp/lv_shell.c`,
  `radio_wifi_on` / `radio_bt_on`, initialised to OFF
  (the honest QEMU baseline) at menu build.
- Poller: one LVGL timer, `radio_timer`, period 5000 ms,
  created in `build_status_bar()` next to `clock_timer`
  and deleted at shell end next to it. The UI-1 smoke
  screen never builds a status bar, so it stays
  poll-free.
- Menu-build path never RPCs: build only creates the
  timer and paints OFF. The first poll fires one full
  period after build, so the menu renders long before
  any poll can stall the loop. Menu appearance costs
  exactly zero RPCs even with linkd dead — by
  construction, not by timing luck.
- Poll steps (timer callback = shell-loop context):
  1. STATUS gate: a raw `uipc_rpc_user("link", ...)`
     STATUS RPC (3 s timeout, no wire exchange —
     linkd answers from internal counters). Linkd
     absent (rc < 0) or transport DOWN → both icons
     OFF, no wire RPCs at all. This is the QEMU
     default and the failure case alike, and it costs
     no wire stall.
  2. Only if transport UP: WIFI.STATUS + BLE.STATUS
     via a new short-timeout stub call (below).
     Parse: wifi ON iff the reply starts "CONNECTED";
     BT ON iff the reply starts "ON" ("ON NONE" vs
     "OFF NONE" — the first two bytes discriminate).
- Short timeouts: the stock `uproto_cmd()` carries the
  30 s `LINKD_RPC_CMD_MS`. A background poll must not
  stall the UI loop for 30 s per RPC when the peer
  hangs mid-exchange, so linkcli gains `uproto_cmd_t()`
  (same path, same parsing, caller-supplied timeout;
  the poll passes 3000 ms). Worst-case UI stall per
  poll is then 2 x 3 s, and only while the transport
  claims UP but the peer hangs — a state where the
  wifi/ble screens hang identically today, so K5
  introduces no new failure class, only a periodic
  one. The poll parks in IPC receive (a scheduler
  switch point, progress-stamped), never spins, so
  the FND-3 watchdog stays fed.
- Logging: change-only (`RADIO: wifi=1 bt=0`). A line
  per poll would spam the serial console and pollute
  other scripts' markers. NOTE: the prefix is NOT
  `UILV5:` — verify-ui5.py asserts no `UILV5:`
  marker survives (the removed name editor's
  namespace).

## 3. Failure semantics (explicit)

| Condition | Wifi icon | BT icon |
|---|---|---|
| linkd absent (lookup/RPC fails) | OFF | OFF |
| transport DOWN (STATUS gate) | OFF | OFF |
| poll RPC timeout / malformed reply | that icon OFF | other icon keeps its fresh value |
| peer DISCONNECTED / OFF | OFF | OFF |
| peer CONNECTED / ON | ON | ON |

Per-icon independence: a wifi-only failure must not
kill the BT icon (and vice versa); only the
linkd-absent / transport-DOWN cases force both OFF,
because there the gate proves nothing is known at all.

Rationale for fail -> OFF over stale ON: an icon
asserts live state. Showing ON when the poll cannot
confirm it would claim a radio that may be gone; OFF
is the safe default and matches the QEMU baseline
(no peer -> OFF, honestly). A stale ON is a lie
about live state; a transient OFF during a peer
outage self-corrects on the next 5 s poll.

## 4. What changed

- `user/uiapp/lv_shell.c`: poll timer + cache +
  STATUS gate + parsers + lifecycle (create/delete
  next to `clock_timer`); stale "later work" comments
  updated.
- `user/uiapp/lv_port.c`: comment only (the call
  site is unchanged; the poll is now its live caller).
- `user/uartproto/linkcli.c` + `uartproto.h`:
  `uproto_cmd_t()` (linkd-stub only; the direct layer
  does not implement it — no consumer there needs it).
- `user/uradio.c` (NEW, QA-only Tier A): the verify
  driver. The shell passes no arguments to spawned
  programs, so the command arrives via a littlefs
  marker `radio-cmd` written by `put` and read back
  with `cat` (the C6L-5 marker pattern). uradio
  splits it on the first space, issues it through
  linkd (the SAME path the poll uses), prints one
  `URADIO: <line> -> <resp|ERR>` line, clears the
  marker, exits 0 (missing marker -> `URADIO:
  NOMARKER`, exit 1).
- `mach3-port/build-md.sh`: uradio build section
  (QA=1 guard, links `uartproto_linkcli.o`) + the
  Phase C image list (the second program loop —
  missing this was the first build failure: the
  kernel link reported `uradio_img` undefined).
- `mach3-port/src/ext/user.c`: uradio extern +
  `embed:uradio` hooks-table entry, both under
  `QA_TIER_A`. Production images omit uradio
  entirely (`run embed:uradio` fails there by
  design).
- `scripts/mock-c6.py`: `--wifi-on SSID` /
  `--ble-on` initial-state flags (test tooling only;
  the pre-change run boots the peer already ON so
  the old kernel's always-OFF icons fail against
  live state without any driver).

## 5. Discrepancy found (not fixed here, on purpose)

LINKD-CONTRACT's STATUS prose says state is
"0 DOWN / 1 UP". The normative header says the
opposite: `UPROTO_LINK_UP = 0`, `UPROTO_LINK_DOWN
= 1` (`user/uartproto/uartproto.h:249-250`), and
linkd's `handle_status()` writes
`uproto_link_state()` verbatim, the shell prints
"UP" when word 1 == 0, and verify-c6l5 expects
"link: UP" on a live peer. All code agrees on
UP = 0; only the contract prose disagrees. K5's
gate follows the header + the shell + the battery
evidence, and notes this here rather than editing
a frozen contract unilaterally — the coordinator
may take the one-line contract fix separately.

## 6. Verify evidence (scripts/verify-k5.py)

Test-first: the same script FAILs on the pre-change
kernel and PASSes on the K5 kernel (both logged in
`~/workspace/logs-k5/`).

- Pre-change (main-tree PROD kernel at 271, peer
  ON/ON): FAIL 19/31. All 8 drive checks fail (no
  uradio on the base image -> `spawn gagal`), and
  all three ON phases fail with icons frozen at
  the OFF values (wifi-gray=120, bt-gray=77, cell
  diffs 0/0). The OFF phases pass trivially (the
  old kernel is always OFF) -- the script's verdict
  still FAILs, as it must.
- Post-change (dev/k5 QA kernel): PASS 31/31
  (verify-k5-post6.log). Every icon phase passes
  on cell pixel-diff against the same run's OFF
  reference dump: ON/ON wifi-diff=83 bt-diff=63,
  ON/OFF 83/0, OFF/ON 0/63, OFF/OFF 0/0, peer-dead
  0/0. Absolute counts for the record: wifi ON
  gray=101 vs OFF 120; BT ON gray=77 vs OFF 77
  (same total -- see section 9); battery stays
  78 + interior empty throughout.
- Menu timing (grant -> UILV2:start): peer up
  0.2 s, peer dead 0.2-0.4 s (budget 25 s; UI-5
  harness steps use 32 s). Menu appearance is
  provably unaffected by linkd state, as designed.
- The peer transcript confirms the poll issues
  both STATUS commands every 5 s period, and the
  RADIO change lines confirm the cache values
  (wifi=1 bt=1, wifi=1 bt=0, wifi=0 bt=1,
  wifi=0 bt=0) track the driven states.

## 7. Battery (split PROD / QA=1)

Logs: `~/workspace/logs-k5/battery-prod/` and
`battery-qa/` (full log per leg; runner
`~/workspace/logs-k5/run-battery.sh`, resume-safe
via status.txt). PROD image for the PROD chain,
QA=1 image for the QA chain (both frozen under
`~/workspace/logs-k5/` as k5-prod.elf / k5-qa.elf).

PROD (all PASS; UI3 needed one rerun, see note):

| leg | result |
| --- | --- |
| K1 | 7/7 |
| UI5 | 73/73 |
| UI4 | 73/73 |
| UI3 | chain 77/78 -> rerun 78/78 |
| UI2 | 38/38 |
| UI1 | 25/25 |
| FND1 | PASS |
| FND2 | 21/21 |
| FND3 | 46/46 |
| FND4 | 36/36 |
| FND5 | 33/33 |
| ONB1 | PASS |
| ONB2 | PASS |
| SHELL | PASS |
| SAFEMODE | PASS |

QA=1 (all PASS after adjudication, see notes):

| leg | result |
| --- | --- |
| K5 | 31/31 |
| K2 | 21/21 |
| C6L1 | 29/29 |
| C6L2 | 36/36 |
| C6L3 | 72/72 |
| C6L4 | 60/60 |
| C6L5 | chain 53/56 -> rerun 55/56 -> rerun 56/56 |
| IPC2 | chain 28/34 -> rerun 34/34 |
| IPC4 | chain 36/58 -> rerun 58/58 |
| IPC5 | 20/20 |

Adjudication notes (standing flake rule: every
chain failure re-ran standalone before verdict):

- UI3 chain 77/78: single failure "shell sets
  timezone" -- the known tz-set timeout class.
  Standalone rerun 78/78. Not a regression.
- IPC2/IPC4 chain failures: identical root, all
  downstream of "web login creates PIN + session"
  failing. Root cause found by probe (not K5):
  the SHARED disk's `cfg-pin` held a foreign hash
  (`3ed0cb79...`, matching neither 246810 nor
  1234 nor 5678 nor the ONB1 wrong-PIN probes).
  `setup reset` clears provisioning only, NOT
  cfg-pin, so the foreign PIN survived every
  leg's teardown. No K5 battery leg writes
  cfg-pin (verified by grep over k5/k2/c6l1-5/
  ipc5), and K5's code never touches it, so the
  contamination predates this battery. Fix
  applied during diagnosis: `setup pin 246810`
  on the shared disk (the documented standard
  PIN every script expects), after which IPC2
  reran 34/34 and IPC4 58/58. The foreign PIN's
  original setter was not identified from battery
  logs; the coordinator may want a standing
  "restore PIN 246810" step in battery runners.
- C6L5 chain 53/56, rerun 55/56, rerun 56/56:
  three runs, three DIFFERENT single failing
  checks (ulinktools dispatches, ulink5d markers,
  qabotd get_info transcript) -- the documented
  death-window serial-under-load flake signature,
  not a consistent regression. Final rerun green.
- The first QA chain run died mid-IPC4 when the
  VM was replaced (~hourly replacement); the
  resume-safe runner re-ran only the unfinished
  legs. Not a product signal.

## 8. Honest limitations

- Icon lag: state changes surface at most one poll
  period (5 s) + RPC time after the peer changes.
  Radio state changes on human timescales; a shorter
  period would only buy UI stalls.
- Hung-peer UI freeze: bounded by 2 x 3 s per poll
  (see section 2), only while transport claims UP
  but the peer hangs mid-exchange.
- The poll does not use EVT: no peer EVT exists for
  radio-state changes, and adding one would be a
  protocol change (out of scope; wire frozen).
- `uproto_cmd_t()` exists in the linkd stub only.
- The contract prose discrepancy (section 5) is
  documented, not fixed, here.
- Production headroom: 1,986,992 B by the
  objcopy-binary method (K5 PROD binary 2,207,312
  B vs main-271 2,205,812 B measured identically,
  +1,500 B; uiapp.bin +1,220 B). The 25 KB drop
  sometimes quoted against UI-8's 2,012,504 B is a
  stale-baseline artifact (UI-8 predates K3+K4);
  the like-for-like K5 delta is +1,500 B.

## 9. Measurement lessons (harness, not product)

Three script bugs were found by the measurement
runs and fixed in verify-k5.py; none was a product
bug:

1. BT ON/OFF by total gray count is wrong: the
   bluetooth ON glyph and the OFF glyph both total
   77 gray pixels (different row shapes, same
   total). ON/OFF discrimination is by cell
   pixel-diff against a same-run OFF reference
   dump (wifi 83, bt 63 pixels change on ON).
2. `SYS_LFS_WRITE` returns 0/-1, not a byte count
   (the shell `put` convention): uradio's clear
   check was `!= 1` and cried wolf on every drive.
3. The menu idle-exits after 30 s without keys,
   and a poke arriving in an already-expired loop
   turn cannot save it (`now` is sampled at loop
   top, before the handler). Phase dumps therefore
   self-heal: a dump that is not a menu (light
   bar pixels < 1000 -- face's dark top band
   scores ~0, a menu scores ~4378) re-grants and
   re-dumps once.
4. A dark-pixel count does NOT discriminate menu
   from face (face's dark background scores higher
   than the menu's light bar) -- the reason for the
   light-bar test in (3).
5. `sys_display_get_event()` drains raw serial
   bytes through `translate_key` when the console
   is unowned ('m' -> EV_MENU, 's' -> EV_DOWN,
   ...). Harness lines containing 'm' (`radio-cmd`,
   `setup name`, `key menu`) sent during unowned
   windows become phantom keys: this caused the
   spontaneous re-grants and one mangled `setup`
   line in the measurement runs. It is
   pre-existing kernel/harness behavior (the
   drain + translate_key predate K5; K5 touches
   neither), and the script tolerates it with
   probe-style retries. It belongs to the K6
   console/input area if anyone wants to chase it.
6. `run_spawn` must poll for its marker AND
   `spawn gagal` concurrently: waiting the full
   60 s timeout before noticing a fast spawn
   failure burned 3 minutes per drive on the
   pre-change run.
