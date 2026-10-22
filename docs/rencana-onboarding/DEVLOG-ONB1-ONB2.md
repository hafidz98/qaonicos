# Dev log — ONB-1 (web settings, PIN, web shell) + ONB-2 (face QR setup screen)

Date: 2026-10-22. Plan: `PLAN-ONB1-ONB2.md` (same directory).
Decisions implemented: D-ONB-3 (single device PIN gates the web
surfaces), D-ONB-5 (web shell off by default), D-GUI-3 (hands-on
finale + persistent menu hint).

## What shipped

### ONB-1 — web UI with PIN gate (kernel `webui.c`, `http.c`, `tcp.c`)

- PIN: stored as SHA-256("qaon-pin-v1:" ‖ pin) hex in littlefs
  `cfg-pin`; 4–16 chars. First `POST /login` creates it; later
  posts verify. One admin session: cookie `qaon_sess`, token =
  SHA-256(pin_hash ‖ boot_count ‖ now_ms), 30 min idle expiry.
- `GET/POST /settings` (session-gated): device name, timezone,
  API key (empty field = unchanged, explicit clear checkbox),
  dev/user GUI mode (bootstate bit2), web-shell enable toggle.
  Values live in littlefs (`cfg-name`, `cfg-tz`, `cfg-apikey`)
  and are the same files the serial `setup` command reads —
  cross-checked both directions in verification.
- Web shell: `GET/POST /shell`, bridged to the serial shell via
  new syscall 103 (`SYS_WEBUI`): the kernel holds a pending
  command + captured output; `sh` polls for web commands in its
  readline loop and captures builtin output through `sh_puts`.
  Default OFF (D-ONB-5); 15 min idle auto-off.
- TCP: request buffer 1024 → 3072; `req_complete()` now honours
  `Content-Length` (POST bodies split across segments work).
- Shell: `setup pin <pin>` + PIN state in `setup` status. The
  `sh` embed cap rose 32768 → 65536 (it had silently outgrown
  the old cap while accumulating B2–B4/Boot-UX/ONB-1 builtins).

### ONB-2 — face setup screen (`user/face/qr.c`, `face.c`, `main.c`)

- New from-scratch QR encoder: byte mode, ECC level M, versions
  1–6 auto, GF(256) Reed–Solomon, block interleaving, all 8
  masks scored with the standard penalty rules; format info is
  computed (BCH) rather than tabled.
- Unprovisioned boot: face shows a white card with the QR of
  `QAON1:<secret>:QaonicOS-Setup` + "SCAN UNTUK SETUP". The
  secret is 32 hex chars kept in `cfg-secret` (per-boot on
  QEMU's RAM littlefs; persistent on hardware). NOTE: the plan
  payload also carried a MAC device id; no syscall exposes the
  MAC today, so the secret doubles as the device identity in
  this scope. ONB-3 (C6 session) splits the two properly.
- The setup screen never blanks (power FSM guard). Face polls
  the provisioned flag at 1 Hz; on flip it runs the D-GUI-3
  finale: eyes + "TEKAN TOMBOL UNTUK MENU" until the first
  EV_MENU, then plain idle with the persistent three-bars menu
  hint (also shown on later provisioned boots).

## Bugs found and fixed on the way (all pre-existing)

1. **littlefs allocator**: the port's `malloc` was a 4096-byte
   bump pool with a no-op `free` — every file open leaked its
   256-byte cache, so after ~16 opens per boot all file I/O
   failed (this is why the PIN "disappeared" after a few web
   requests). Replaced with a first-fit free-list allocator
   with coalescing over a 32 KB static pool.
2. **Pool alignment**: the pool is a plain `unsigned char[]`;
   in one build it landed on an odd BSS address and the
   allocator's `strd` header store took an alignment fault
   (panic in the ULFS self-test, `dfar` = pool base + 1). The
   pool is now `__attribute__((aligned(8)))`. Rule: any static
   byte arena that will hold typed headers must be explicitly
   aligned — BSS placement luck changes with any edit.
3. **TCP server wedge**: the single-connection HTTP server sat
   in FIN_SENT after answering; a new SYN was ignored (same
   tuple: counted as activity, refreshing the 120 s timeout —
   permanent wedge; different tuple: ignored until timeout).
   Browsers/curl close politely and never hit it; Python's
   http.client does. A SYN now takes over a lingering FIN_SENT
   connection, and a peer FIN in FIN_SENT returns the server
   to LISTEN.
4. **Mount-on-demand**: `lfs_run_test` leaves the filesystem
   unmounted by design; `lfs_port_read/write/list` returned -1
   when unmounted, so kernel-side config writes silently
   depended on some other client having mounted first. They
   now mount on demand (`lfs_port_mount`, format-on-first-use
   as before). This one masqueraded as an ONB-1 bug for hours;
   it worked in early tests only because a one-shot happened
   to leave the FS mounted.

QR debugging note: the encoder's data stream, ECC and placement
were verified byte-exact against an independent implementation;
the one real QR bug was in mask selection — the best candidate
was copied over the pristine unmasked matrix, so later masks
were applied on top of earlier ones. Fixed with separate
base/best buffers.

## Verification

- `scripts/verify-onb1.py` — 14/14: dashboard, settings gate
  (303 without cookie), create-PIN login + cookie, settings
  save → serial `setup` cross-check, shell enable → web
  `bootctl` output rendered, wrong PIN rejected, serial shell
  still healthy. **ONB1-VERIFY: PASS**
- `scripts/verify-onb2.py` — 4/4: face prints the setup
  payload; the QR decoded from the actual 240×240 framebuffer
  (ZXing) equals that payload exactly; after `setup done` the
  finale screen shows the eyes + prompt text + hint (4,460
  eye-coloured pixels in the eye band). **ONB2-VERIFY: PASS**
- QR encoder alone (host harness): ZXing decodes V1–V6 test
  payloads including the exact production payload.
- Regression: SHELL / SAFEMODE / FIRSTBOOT / UPDATE all PASS
  on the final build, run in sequence. (One transient
  FIRSTBOOT failure was observed once during back-to-back
  script chaining — shared disk state and killed-boot fail
  counts; it passes standalone and in the repeated sequence.
  Not reproduced; noted, not hidden.)
- Test-harness lesson baked into both verify scripts: the
  serial socket must be drained continuously from a thread.
  The guest's console printf is polling-mode; an undrained
  socket back-pressures QEMU's chardev and stalls the guest
  CPU inside printf, which looks exactly like a network hang.

## Known limits (unchanged from the plan)

- Web UI is plain HTTP on the LAN (no TLS on this path).
- Session token entropy is timer-grade until the RV1103 TRNG
  is wired (hardware step).
- Web shell captures builtin output only; a foreground program
  that owns the display token is out of its reach.
- ONB-3/4/5 (C6 BLE provisioning, SoftAP portal, Android
  wizard) still wait for the C6/board.
