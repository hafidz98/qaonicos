# Plan sketch: Muse Gadget SDK port to QaonicOS / Luckfox

Status: SKETCH, not a committed track plan. Research only — no
implementation in this document's phases until the owner decisions
below are made. Companion: `RINGKASAN.md` (research + gap analysis).
Gap IDs (G1..G8) refer to `RINGKASAN.md` §4.

Standing constraints this plan respects:

- **FW track stays parked** until the owner reopens it (owner,
  2026-10-24: "finish the current progress first"). Phases G-0
  and G-1 are board-less and touch no firmware; everything that
  touches C6 firmware sits behind decision D-GADGET-1.
- Track order stands: C6L closes (C6L-4, C6L-5) before new
  track work competes for the link. G-1 assumes linkd (C6L-5)
  exists; if C6L-5 slips, G-1 slips with it.
- No credentials, API keys, or tokens are ever committed. The
  SDK token is entered at runtime (serial/webui), never baked
  into a doc, a script, or a firmware default.
- PC storage rule: no ESP-IDF toolchain on the owner's PC
  (~5 GB free). ESP-IDF builds run in GitHub Actions
  (Espressif IDF image); the PC only flashes and monitors.
- The Gadget channel is an *additional* Mode-B companion
  channel. Mode A (direct LLM API) and offline behaviour keep
  working when the Gadget session is down (gap G8).

## Phase G-0 — Board-less design freeze (no firmware, no unpark)

Goal: every design question answerable without hardware is
answered, so the firmware phases start from a frozen spec.

1. **Pin the SDK version.** Record the exact upstream commit
   verified against (date + hash); all later work references
   that pin until a deliberate re-pin. Re-read `link.register`
   semantics and the session/voice-note protocol from the
   pinned tree; close the two **[U]** items in `RINGKASAN.md`
   §6 (cloud session protocol, voice-note upload codec) or
   carry them as explicit unknowns into G-2.
2. **Gadget-channel wire design.** Define the UART frames
   between linkd and the Gadget firmware as a versioned
   amendment to the C6L wire: tool-call request/response
   (mapped to `link.register` commands), caption/event
   delivery (over C6L-3 EVT), voice-note audio (over v2 AUD
   frames, 16 kHz 16-bit PCM as today). State the multiplexing
   rule with QaonicOS's own link traffic (linkd arbitration,
   G5) and the park/yield rule (no 30 s spins).
3. **Mock-peer Gadget mode.** Extend `scripts/mock-c6.py`
   (QEMU side only) with a Gadget-mode peer: answers
   `link.register`-shaped dispatches, pushes caption EVTs,
   accepts AUD frames as voice notes. This is test tooling,
   not firmware.
4. **Caption flow design.** Face/uiapp caption rendering
   from EVT text: paging on 240×240, honest "Gadget offline"
   state, physical-confirm mapping for sensitive Gadget
   actions (D-ONB-3 family).
5. **Provisioning design (G4).** Token entry path (serial
   first, webui later), credstore backup of the token,
   rotation/re-pair UX, and the two-pairing story (device
   onboarding vs Muse pairing) written as a user-facing flow.
6. **XIAO C6 profile draft (G6).** Draft `sdkconfig.xiao-c6`
   from the closest stock profile (`c6-nopsram`) + XIAO
   facts (512 KB SRAM, 4 MB flash, no PSRAM, UART1 D6/D7,
   BOOT button, no clean user LED → external LED). Draft
   only — no build, no flash.
7. **Account/region check (G7, owner action).** Confirm a
   Muse account can be created, an SDK token issued, and
   the Muse app paired from the owner's region. This is a
   paperwork check, not engineering, and it gates G-2's
   live test.

Exit criteria: wire amendment written; mock-peer Gadget mode
specified; provisioning flow written; profile drafted; SDK
pin recorded. Zero firmware touched, FW track still parked.

## Phase G-1 — linkd Gadget channel, QEMU-verified (after C6L-5)

Goal: the RV1103 side of the Gadget channel works end-to-end
against the mock peer before any real firmware exists.

1. linkd exposes the Gadget channel to IPC clients
   (`tools`-family naming per the frozen IPC contract; the
   exact IPC surface is linkd's design decision, not this
   sketch's).
2. QEMU verification: guest client → linkd → bridge backend
   → Gadget-mode mock peer → dispatch result / caption EVT
   → face caption path, all in `scripts/verify-gadget*.py`
   style (FAIL pre-change, PASS post-change), plus the
   Gadget-down fallback (Mode A keeps working, face says
   the channel is down).
3. Audio loop in QEMU: recorded PCM → AUD frames → mock
   peer voice-note counter, proving the budget math at the
   frame level (real acoustics still need hardware).

Exit criteria: QEMU battery green with the Gadget channel
exercised; fallback proven. Still no firmware, still parked.

## OWNER DECISION D-GADGET-1 — unpark the FW track for Gadget firmware

Question for the owner, in one line: **may C6 firmware work
resume, scoped to (a) building stock Gadget firmware for the
XIAO C6 profile and (b) adding the UART QaonicOS channel?**

What a YES unlocks: phases G-2 and G-3. What a NO means:
G-0/G-1 stand as the complete deliverable; the Gadget port
stays a designed-but-unbuilt option, and no board work starts.
Note the hardware position at decision time: the XIAO C6 is
already owned (G-2 needs only it + a PC + a Muse account);
the Luckfox board is still unpurchased (G-3's RV1103 side
can be QEMU-verified, but the real UART link needs the board).

## Phase G-2 — Stock Gadget firmware on the XIAO C6 (needs D-GADGET-1)

Goal: prove the Gadget itself works on the owner's hardware
before QaonicOS touches it.

1. Build the pinned SDK for the XIAO profile in GitHub
   Actions (ESP-IDF v6.0.1 image); flash from the PC.
2. Pair with the Muse app (Developer mode, BOOT press,
   SDK token): status-light flow, Wi-Fi join, `ws=up`.
3. Baseline checks: voice note → text caption in the app,
   custom `link.register` echo command, re-pair after token
   rotation, NVS encryption enabled.
4. Record the measured facts the research could not verify:
   voice-note upload codec/format, session behaviour on
   Wi-Fi drop, and the exact status-light mapping on the
   XIAO (external LED).

Exit criteria: a paired XIAO C6 gadget, documented behaviour,
and the two **[U]** items closed or re-scoped. QaonicOS still
not involved.

## Phase G-3 — UART channel + RV1103 integration (needs D-GADGET-1 + board)

Goal: the full loop on real hardware.

1. Add the UART QaonicOS channel to the Gadget firmware
   (G1): tool-call serving, caption/event emission, PCM
   audio ingress. Keep the Gadget session and the UART task
   from starving each other (C6-side scheduling note).
2. Wire the bench: RV1103 UART ↔ C6 UART1 (D6/D7), levels
   checked; physical buttons mapped (QaonicOS PTT/confirm
   vs Gadget BOOT press).
3. End-to-end tests: PTT on the device → voice note →
   Muse reply → caption on the 240×240 face; tool call
   from Muse → QaonicOS capability → result; Gadget-down
   fallback on the face; token rotation + re-pair flow.
4. Hardening: NVS encryption on the C6, credstore backup
   sync, watchdog/park rules on both sides, privacy note
   (what leaves the device) in the user docs.

Exit criteria: the bench loop green; the out-of-box bar
still holds (offline first boot tells the user what to do;
Gadget-down is an honest state, not a brick).

## OWNER DECISION D-GADGET-2 — PSRAM variant (only if wanted)

Question for the owner, in one line: **is the home-network
tunnel, spoken replies via `start_tts`, or the video protocol
worth a PSRAM board (e.g. a PSRAM ESP32-C6/S3), or does the
no-PSRAM XIAO C6 stay the target?**

Default if unanswered: **stay no-PSRAM.** The tunnel is the
only feature in this plan that needs PSRAM, and nothing in
phases G-0..G-3 depends on it. A YES spawns a separate small
track (new board profile + tunnel validation); it does not
reopen G-0..G-3.

## Explicit non-goals

- No commercial device with an embedded token (Token Terms:
  personal use only).
- No video protocol over UART (bandwidth + PSRAM — permanently
  out of scope for this link).
- No port of the Linux SDK (`system.run` as installing user)
  to QaonicOS — wrong security shape, no host.
- No re-implementation of the Muse cloud protocol on the
  RV1103 (shape (b), rejected in `RINGKASAN.md` §2).
- No change to the frozen C6L wire except by versioned
  amendment; no silent edits to v1.1/v1.2.
