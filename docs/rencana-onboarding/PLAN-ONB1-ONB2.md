# ONB-1 + ONB-2 Implementation Plan (QEMU scope)

Date: 2026-10-22. Implements the QEMU-testable phases of
`PLAN.md` (onboarding & settings surfaces). Decisions D-ONB-1..6
and D-GUI-1..4 from PLAN.md govern. Board/C6 phases (ONB-3/4/5)
are out of scope here.

## Findings that shape the design (code survey 2026-10-22)

- `kernel/kernel/arm/http.c` is GET-only; the TCP layer
  (`tcp.c`) accumulates at most **1024 bytes** and treats the
  request as complete at the header terminator — POST bodies
  are never received today.
- Response path: `resp[8704]`, page body buffer 8192 — new pages
  must stay compact.
- Config is littlefs files, writable from the kernel via
  `lfs_port_read/write` (the shell's `setup` uses the same files
  through syscalls: `cfg-name`, `cfg-tz`).
- The kernel already has a self-contained SHA-256 (updverify.c).
- The shell's console read is **non-blocking** (`sys_read_console`
  returns <0 when empty; `sh_readline` yields and retries) — a
  web-command queue can be polled from that loop without
  restructuring the shell.
- Shell output funnels through `sh_puts`/`sh_putu` (fd 1), so
  output capture hooks two functions.

## ONB-1 — web settings + PIN gate + web shell

### Transport (tcp.c)
- Request buffer 1024 → 3072 bytes.
- Request completeness: after the header terminator, if a
  `Content-Length` header is present, keep accumulating until
  the body is complete (capped by the buffer). GET behaviour
  unchanged.

### New kernel module `webui.c`
Owns all ONB-1 state so `http.c` stays a router:
- **PIN**: stored in littlefs `cfg-pin` as hex
  SHA-256("qaon-pin-v1:" || pin). Baseline for ONB-1; B5.5
  (credential store) hardens storage later. Verify = recompute.
- **Session**: one active session. Token = first 32 hex chars
  of SHA-256(pin_hash || boot_count || login_ticks). Cookie
  `qaon_sess`, HttpOnly, Path=/. Idle expiry 30 min (checked
  per request against arm_timer_ticks).
- **Web shell bridge**: state machine IDLE → PENDING →
  RUNNING → DONE; `cmd[160]`, `out[4096]` (truncation marked).
  Enabled flag (default OFF per D-ONB-5) with 15 min idle
  auto-off, evaluated per request.
- **Syscall 103 `SYS_WEBUI`** (for userspace):
  sub 0 = take pending command (sh polls; copies cmd, → len/-1),
  sub 1 = post result (sh sends captured output),
  sub 2 = set PIN from a trusted local path (shell `setup pin`),
  sub 3 = PIN present? (1/0).

### Routes (http.c)
- `GET /`, `GET /metrics` — unchanged, public (monitor only;
  D-ONB-3 gates settings + shell, not the monitor).
- `GET /login` — no PIN on device → create-PIN form (this is
  the QEMU stand-in for the onboarding Stage-3 PIN creation;
  on hardware it happens inside the BLE/SoftAP session). PIN
  present → login form.
- `POST /login` — verify or create; Set-Cookie; 303 → /settings.
- `GET /settings` — session required (else 303 → /login).
  Fields: device name (`cfg-name`), timezone (`cfg-tz`), API
  key (`cfg-apikey`; empty field = unchanged, separate
  "clear key" checkbox = delete — the field never echoes the
  key, only its set/empty state), boot mode (bootstate GUI-mode
  flag, `bootstate_set_gui_mode`), web-shell enable checkbox.
- `POST /settings` — session required; saves; 303 → /settings.
- `GET /shell` — session + shell-enabled required (else 403
  page explaining the toggle). Shows last result + command form.
- `POST /shell` — session + enabled; submits the command to
  the bridge and polls for the result up to ~3 s (bounded spin
  on timer ticks; netstack latency is acceptable at this
  scale). Timeout → the result can still be read via GET /shell.

### Shell side (sh.c, ulib)
- `sh_readline`: when the console is empty, poll SYS_WEBUI
  sub 0; a queued command becomes the input line, capture ON.
- Capture: `sh_puts`/`sh_putu` append to a 4 KB buffer while
  capture is on. After dispatch, the main loop posts the
  buffer via sub 1 and turns capture off. **v1 limit**: output
  of spawned child programs writes straight to the console and
  is not captured (builtins are).
- `setup pin <pin>` — sets the device PIN from the serial
  shell (dev/recovery path, mirrors `setup name/tz`).

## ONB-2 — face setup screen + QR

### QR encoder (`user/face/qr.c`, new)
- Byte mode, ECC level **M**, automatic version 2..6 (payload
  ≤ 120 chars). Reed-Solomon over GF(256) with generated
  exp/log tables; interleaving per the spec's block table for
  versions 2..6-M; all 8 masks scored with the standard
  penalty rules; format info written accordingly.
- Verification without a camera: render the module matrix and
  diff it module-by-module against a reference QR generated
  on the host for the same payload (python `qrcode` lib).

### Face behaviour (user/face/main.c)
- At start, face reads the provisioned flag (syscall 98 sub 0).
  Unprovisioned → **setup screen**: QR centred on the 240×240
  display, one line "SCAN UNTUK SETUP" under it (font5x7, the
  lowercase fold keeps it readable).
- Payload: `QAON1:<devid>:<secret>:<ssid>` where devid = the
  net MAC in hex, ssid = `QaonicOS-Setup`, secret = 8 hex chars
  derived from a per-boot nonce kept in `cfg-secret` (created
  on first need). On hardware the C6 session uses the same
  payload (ONB-3/4); in QEMU it is display-only.
- Poll the flag every ~1 s. When it flips to provisioned →
  **finale** (D-GUI-3): eyes + status text inviting the first
  menu press ("TEKAN TOMBOL UNTUK MENU"); after the first
  EV_MENU, normal idle resumes and a small persistent menu
  hint (three bars, bottom corner) is drawn on the idle face.

## Verification

- ONB-1: QEMU boot with `hostfwd=tcp::18080-:80`; curl script:
  dashboard 200 → /login create PIN → cookie → /settings 200
  → POST settings (name/tz) → read back → /shell 403 while
  disabled → enable → POST `bootctl` → output contains the
  boot state → wrong PIN rejected → settings without cookie
  redirects to /login. Plus the standard verify suite.
- ONB-2: boot with a cleared provisioned flag → capture-gui
  screenshot shows the QR screen; module-matrix diff vs the
  host reference = 0 mismatches; `setup done` from the shell →
  face flips to the finale/eyes; screenshot + shell transcript
  attached per `docs/VERIFIKASI-GUI.md`.
- Regression: verify-shell, verify-safemode, verify-firstboot,
  verify-update, uupdsig boot markers — all PASS.

## Out of scope / known limits

- No TLS on the web UI (LAN + PIN gate; recorded risk, same as
  the existing monitor server).
- Session/token entropy is timer-grade until the RV1103 TRNG
  lands (tracked in the TLS entropy debt).
- Web shell captures builtin output only (see above).
