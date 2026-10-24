# DEVLOG-C6L4 — AUD streaming integrity + error semantics at simulated 921600

Phase: C6L-4 (fourth phase of the C6 link track).
Branch: `dev/c6l4` (worktree `~/workspace/qaonic_os-c6l4`).
Base: C6L-3 merge + EXIT-trap fix (`09847c6`).
Status: DONE, verified. No merge, no push (coordinator's job).

## 1. What was built

Three pieces, all in the userspace link layer (no kernel
change, no wire change — v1.1 and v1.2 stay frozen):

1. **Link state + counters + failure classification**
   (`user/uartproto/uartproto.c`, API in `uartproto.h`).
   Every command wrapper classifies its outcome exactly
   once. A peer ERR answer counts as a peer error and
   leaves the link state alone (the peer answered; the
   link is fine). Any transport failure marks the link
   DOWN with a reason (TIMEOUT / DEAD / CRC / OVERSIZE /
   MALFORMED) and resets the transport: backend close +
   byte staging cleared, queued inbound units kept.
   The next command lazily reopens the backend — that
   is the auto-recover. DOWN clears on the next
   successful command. Frame-send failure marks DOWN
   (DEAD) and closes the backend without clearing
   staging (no exchange is in flight).
2. **Retry policy** (`uproto_cmd_retry`,
   `uproto_kv_set_retry`, classifier
   `uproto_retry_policy`). Plain wrappers NEVER retry.
   The `_retry` variants retry at most ONCE, only
   after a TRANSPORT failure (never after a peer ERR),
   and only for idempotent commands: the allowlisted
   line commands (WIFI.SCAN/LIST/STATUS/DISC,
   BLE.STATUS/ON/OFF, TIME.GET, FIDO.LIST)
   unconditionally, and KV.SET only when the caller
   asserts `idem != 0` (same value re-written).
   WIFI.PASS, KV.SET with a changed value,
   TOOL.DISPATCH, VOICE.*, NET.WS.*, DISPATCH.RESULT,
   BLE.NAME, WIFI.CONNECT, and unknown tokens are
   NEVER retried. KV.GET has no retry variant: its
   length-prefixed body must be read by
   `uproto_kv_get`, never by the line reader.
   Placement justification: the policy lives in the
   link layer because the link layer is what sees
   transport failures and parses the command token;
   the caller only declares idempotence it actually
   knows (KV.SET same-value). Linkd (C6L-5) reuses
   the classifier, not a new policy.
3. **Bridge baud pacing** (`user/uartproto/bridge.c`,
   API in `link.h`). The bridge stands in for a UART,
   so it paces all bytes it sends to the configured
   baud budget (10 bits per byte, airtime ceiled so
   the budget is never exceeded). Default 921600 (the
   product baud); 115200 is the v1 profile that
   cannot carry voice. The pacer is a reservation
   clock: a send that arrives early yields until its
   budget frees (yielding pumps net_pump and feeds
   the watchdog). The mock backend is NOT paced
   (in-process, no UART). Pace waits / wait-ms are
   exposed for rate tests. Note: a chunk that the
   kernel send returns 0 for (window full) reserves
   its airtime again on retry — conservative (slower
   than budget, never over), and all link-layer
   writes are single-chunk in practice.

## 2. Streaming: measured numbers vs the 92 KB/s budget

Guest program `user/ulinkaud.c` (spawn-only,
`embed:ulinkaud`), peer `scripts/mock-c6.py` extended
with AUD sequence checking (payloads starting `ULS4`
+ u16 BE sequence are order-checked, `aud_seq_gap`
counts gaps; expected resets on VOICE.START) and
C6L-4 test keys (`__badlen__`, `__hugelen__`) and
the `--junk-after` fault flag.

Production-paced 10 s stream (320 x 1024 B frames,
16 kHz x 16 bit = 32 KB/s payload, 1031 B on the
wire per frame):

- Guest sent 320, peer `aud_ok` 320, VOICE.STOP 320,
  `aud_bad` 0, `aud_seq_gap` 0, `aud_outside` 0.
- Send-completion intervals: min 30 ms, max 40 ms,
  mean 31 ms, jitter events (> 40 ms) 0, against a
  nominal 31.25 ms production interval. Real-time
  pacing held for the full 10 s.
- Wire rate ≈ 32.2 KB/s against the 92,160 B/s
  budget: headroom 2.86x. The 115200-baud demo
  (32 frames back-to-back): wall 2890 ms, wire
  11,415 B/s against the 11,520 B/s budget, pace
  waits 34, pace wait-ms 2910 — i.e. the pacer, not
  the scheduler, set the rate, and 32 KB/s voice
  cannot fit in 11,520 B/s: v1 baud fails voice,
  as the plan predicted. (Per-frame pace airtime:
  12 ms at 921600, 90 ms at 115200, both ceiled.)

Full-run link counters (one guest run, all phases):
ok 23, peer_err 2 (ERR ARG + NOTFOUND), timeout 3
(silence x2 attempts + hugelen), dead 21 (3
truncated-response attempts + 16 post-kill frame
sends + STOP + TIME.GET open-fail), crc 0,
oversize 0, malformed 2 (junk line + "OK abc"),
resets 10, retries 2 (silence + drop), denied 2
(PASS + SET), frames sent 400, frame send fails 16
(400 + 16 = 416 = all frames attempted). Every
number reconciles; nothing is silently swallowed.

## 3. Error table: client-visible outcomes (all asserted)

| Fault | Client sees | Recovery | Asserted by |
|---|---|---|---|
| Peer silence (2 s, no bytes) | TIME.GET fails after 2 attempts (1 retry); LINK DOWN TIMEOUT | Reopen; next TIME.GET ok, LINK UP | P2 transcript: TIME.GET x2; P3 TIME.GET served |
| Truncated response (drop-after) | TIME.GET attempt fails (DEAD); retry succeeds | Auto-reset + retry; stream in sync | P4: 3 truncated RESPs; TIME.GET x4 (probe + attempt + retry + probe) |
| WIFI.PASS, response lost | -1, retry DENIED (denied counter) | PASS landed exactly once (LIST shows the network) | P4: WIFI.PASS REQ x1 |
| KV.SET changed value, response lost | -1, retry DENIED | SET landed exactly once (GET shows new value; kv_sets == 1) | P4: KV.SET REQ x1, kv_sets == 1 |
| Peer ERR (NOTFOUND) | -1, peer_err counter, NO retry, link stays UP | Next command normal | P4: KV.GET missing REQ x1 |
| Peer death mid-stream | In-flight STOP fails; LINK DOWN DEAD; TIME.GET fails | Reconnect loop; fresh 32-frame stream STOP == 32 | P5/P6: aud_ok == 32, gaps 0 |
| Garbage line in stream | TIME.GET fails; LINK DOWN MALFORMED | Auto-reset drops the stale peer OK behind the junk; next TIME.GET ok | P7: junk line in transcript |
| Over-long KV.SET line (len 5000) | ERR ARG; peer_err; link stays UP | Next command normal | P8: ERR ARG in transcript |
| Malformed length ("OK abc") | KV.GET fails; LINK DOWN MALFORMED | Reopen; TIME.GET ok | P8 markers |
| Over-long body ("OK 100000", no body) | KV.GET times out (2 s); LINK DOWN TIMEOUT | Reopen; TIME.GET ok | P8: __hugelen__ served |

Notes on the table:

- `--drop-after N` truncates from response N+1
  (the check runs before the served counter
  increments); the drop peer uses N=1 so the
  probe is resp 1 and the target is resp 2.
  `--junk-after N` injects after response N (the
  check runs after incrementing). The asymmetry is
  a peer implementation detail, documented here so
  the next phase does not misread it.
- The reader's frame-length oversize path
  (declared frame len > LINK_MAX_PAYLOAD) is
  classified (OVERSIZE) but has no dedicated
  reader probe in this phase: the link layer never
  emits > 1024 B, so only a malicious peer could
  trigger it, and the over-long-body class is
  covered by the peer-side BODY_MAX rejection
  (KV.SET 5000) and the hugelen timeout. The
  oversize counter reads 0 in the full run, and
  that is honest, not a gap in the run: no
  oversize frame arrived. A scripted oversize-EVT
  probe is a one-line peer addition if linkd
  wants it (see §6).
- One classification bug found while wiring the
  paths and fixed in this phase: the EVT-oversize
  branch of `next_resp_line` returned -1 without
  setting the oversize flag (its siblings in
  `poll_one` and `ws_recv` did), so an oversize
  EVT during a command wait would have classified
  as TIMEOUT. Now flagged. No test hit that
  branch before or after the fix; the fix is
  behavior-preserving for all existing tests.

## 4. Retry policy, restated for linkd (C6L-5)

Linkd reuses `uproto_retry_policy()` and the
`_retry` wrappers; it must not invent a second
policy. Consequences for linkd's design: linkd's
client stub exposes the plain wrappers (never
retry) for command execution, and the retry
variants only for the allowlisted reads; a
client that needs KV.SET-same-value retry passes
`idem` explicitly and owns the sameness claim.
The yield-or-park watchdog rule (DEVLOG-IPC5
§11) and the spawn-only rule for network programs
(C6L-2) both still hold: the guest test yields
in every wait, and the pacer yields while waiting
for budget.

## 5. Verification

- `scripts/verify-c6l4.py`: **C6L4-VERIFY: PASS
  (60/60)** three times: run1 (first run), run2
  (after the serial-dump harness addition),
  run3 (after the §3 flag fix rebuild).
  Run3 numbers reproduce run2 exactly except
  stream min interval (20 ms vs 30 ms — ms
  scheduling granularity, mean still 31 ms,
  jitter still 0). Pre-change evidence:
  against the C6L-3 kernel (`~/workspace/
  c6l4-base.elf`, saved before implementing):
  **FAIL (10/52)**, `spawn gagal`, exit 1 — the 10
  passes are boot regressions + vacuous checks,
  the same shape as C6L-2's pre-change run.
- Harness notes: each peer process gets its own
  transcript/stats files (per-phase assertions
  survive the restarts); the serial log is saved
  to `logs-c6l4/serial.log` (untracked scratch,
  not committed). The pre-change run originally
  crashed at QEMU launch because the base kernel
  copy lived in /tmp and /tmp was wiped by a VM
  replacement mid-task; the base copy now lives
  in `~/workspace/c6l4-base.elf` and the script's
  scratch lives under `logs-c6l4/` (same lesson
  as ever: /tmp is not storage).
- Battery: (filled after the battery run — see §7).

## 6. Limitations / honest gaps

- No real-time pacing against the RV1103 audio
  DMA (plan non-goal; that is H4). The stream
  here is guest-generated PCM through the link,
  not the kernel audio path: the plan's "from
  the kernel audio path" is approximated by the
  same VOICE.START/AUD/STOP contract the audio
  driver uses (uvoice), not by I2S capture.
- No STT endpoint (plan non-goal).
- Simulated baud only: the bridge paces sends,
  but slirp TCP underneath is unpaced; on
  hardware the UART itself enforces the budget
  (and adds its own framing/overrun behaviour,
  which QEMU cannot show).
- Reader oversize-frame probe: see §3 note.
- The pacer re-reserves airtime when a chunk
  send returns 0 (window full) and is retried:
  conservative (slower than budget, never over),
  not a correctness issue for single-chunk
  link-layer writes.

## 7. Battery (standalone, worktree kernel)

Full battery, worktree kernel `mach3-port/build/mach3.elf`,
run in order (Python verifies, then .sh scripts):

| Script | Result |
|---|---|
| verify-fnd1.py | PASS |
| verify-fnd2.py | PASS (21/21) |
| verify-fnd3.py | PASS (46/46) |
| verify-fnd4.py | PASS (36/36) |
| verify-fnd5.py | PASS (33/33) |
| verify-ipc2.py | PASS (34/34) |
| verify-ipc4.py | PASS (58/58) |
| verify-ipc5.py | PASS (20/20) |
| verify-onb1.py | PASS |
| verify-onb2.py | PASS |
| verify-c6l1.py | PASS (29/29) |
| verify-c6l2.py | PASS (36/36) |
| verify-c6l3.py | PASS (72/72) |
| verify-c6l4.py | PASS (60/60), also re-passed inside the battery |
| verify-shell.sh | PASS |
| verify-safemode.sh | PASS |
| verify-firstboot.sh | battery run FAIL (state); **standalone rerun PASS** |
| verify-update.sh | PASS |

All 15 Python verifies passed first-try in battery
order. The single battery-order failure is the known
FIRSTBOOT state precondition (documented in C6L-1
and C6L-3): ONB1 runs earlier in the battery and
provisions the disk, so boot 1 sees a provisioned
device ("banner missing", "UFIRST did not report
first-boot", "setup status should be NOT
provisioned"). The standalone rerun passed, and the
script's own boot 3 leaves the disk unprovisioned
again, so the shared dev disk ends healthy and
unprovisioned. No C6L-4 regression.

Headroom (objcopy-binary method, same as C6L-3):
binary 3,531,292 B, headroom **663,012 B** in the
4,194,304 B partition (C6L-3: 738,688 B; delta
75,676 B = ulinkaud image + link-layer state /
counters / retry / pacer code). No kernel change
in behaviour: the extension layer grew, the Mach
substrate did not (no new syscall, no ABI change).
