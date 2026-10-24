# Research: porting the Muse Gadget SDK to QaonicOS / Luckfox Pico Mini B

Status: RESEARCH ONLY. No implementation, no firmware work, no unpark of
any parked track. Written 2026-10-24 against the SDK as published
2026-10-02. C6L-3 was in flight at time of writing; gap analysis assumes
C6L-3 as specified in `docs/rencana-komunikasi-c6/PLAN.md` and must be
re-checked at its merge.

Companion: `PLAN.md` (phased plan sketch + owner decision points).

## 0. Sources and verification standing

Primary sources read directly (2026-10-24):

- `github.com/facebookincubator/muse-gadget-sdk` — repo root README and
  `esp32/` README (board table, pairing flow, PSRAM note, `link.register`,
  token handling, license exceptions). Repo at read time: public, main
  `c9354b7`, 128 commits, 2395 stars, 523 forks, 0 releases, 0 tags,
  created 2026-10-02T18:21:23Z, Apache-2.0.
- `docs/riset-agent-bots/RINGKASAN.md` §6 (prior in-repo research,
  verified live 2026-10-22: 2364 stars, 511 forks, 127 commits, HEAD
  `e6fe517` at that date) — consistent with the primary read; board
  count grew since (see §1).
- QaonicOS docs in this repo: `docs/rencana-komunikasi-c6/PLAN.md`,
  `WIRE-FREEZE.md`, `DEVLOG-C6L1.md`, `DEVLOG-C6L2.md`,
  `docs/rencana-onboarding/PLAN.md` (D-ONB-1..7), `mach3-port/src/ext/
  credstore.c` (QSEC format).

Secondary sources (press / community write-ups, used only where the
primary source is silent, and flagged as such): cellcog.ai, ai-tldr.dev,
codersera.com, dev.to (first-hand build report), hackernoon.com
(M5StickS3 build), therundown.ai, Seeed XIAO ESP32-C6 datasheet mirrors
(store.comet.bg, pakronics.com.au, shillehtek.com).

Load-bearing claims carry a tag: **[V]** verified against a primary
source or in-repo code, **[S]** secondary-source only,
**[U]** unverified — do not build on it without a check.

## 1. What the Muse Gadget SDK is

**Publisher and shape.** Meta (repo under `facebookincubator`) published
Muse Gadgets on 2026-10-02 as open-source firmware/SDKs that turn
DIY hardware into peripherals ("gadgets") for Muse, Meta's personal
AI agent **[V]**. It is explicitly a hacker side project — "built by
hackers, for hackers, just for fun", as-is, no warranty, "not a
supported product and not a developer platform" per the Gadget SDK
Terms as reported **[S]**. There are no releases and no tags; the only
distribution is the moving `main` branch **[V]**.

**Two SDKs, one cloud brain.**

- **ESP32 Device SDK** (`esp32/`): ESP-IDF **v6.0.1** firmware — other
  versions are explicitly unsupported **[V]**. Boards run an animated
  avatar + push-to-talk + settings on UI boards, or status light /
  ring / simple screen on the rest **[V]**. Voice flow: push-to-talk
  sends a voice note, Muse transcribes it and **answers in writing**;
  boards with a screen show the answer as captions **[V]**. Spoken
  answers are DIY: send the reply text to a third-party TTS API and
  play the returned audio; on PSRAM boards `start_tts` in
  `components/muse/muse_chat_session.cpp` is the spot, where an MP3
  decoder, speaker and volume are already wired **[V]**.
- **Linux Device SDK** (`linux/`): a systemd service (`/opt/musegadget`)
  turning a Raspberry Pi / BLE Linux box into a gadget. Muse gets
  exactly four commands — `system.run`, `file.read`, `file.write`,
  `device.health` — executed **as the installing user, sudo included
  if the account has it** **[V]**. Local programs can push messages
  into Muse chats via `musegadget send-user-msg` **[V]**.
- Also in the repo: a **skills catalog** (`skills/`) of community
  Markdown device skills (`gadget-<device>/SKILL.md`) for
  Home-Link-reachable devices **[V]**; an **LVGL/SDL desktop
  simulator** of the production UI **[V]**; a **video protocol**
  (`protocols/VIDEO.md`: phone camera → board screen, H.264/VP8,
  30 fps on the round 466 px boards) **[V]**; and a **web flasher** **[V]**.

**Board support — ESP32-C6 is first-class, including without PSRAM.**
The `esp32/` README table lists ~33 boards across ESP32 / C5 / C6 /
S3 **[V]** — the "11 boards" figure in launch press is stale. Two
rows matter most here:

- `ESP32-C6 devkit without PSRAM` — "Status light and button",
  built with `tools/board.sh c6-nopsram build` **[V]**.
- `Waveshare ESP32-C6-LCD-1.47` and two Waveshare C6 AMOLED boards
  (1.8", 2.06") — C6 with screens **[V]**.

The README states the PSRAM rule in plain text: "Boards without
PSRAM, like the classic ESP32 and the ESP32-C6, run without the
home-network tunnel, which needs more memory than they have. Muse can
still reach the device and control it." **[V]** This **corrects and
refines** the earlier in-repo note ("Mode-B tunnel needs PSRAM"):
the tunnel needs PSRAM, the gadget does not. What no-PSRAM loses,
per the README: the home-network tunnel (Muse reaching LAN devices /
local HTTP APIs through the gadget) and the `start_tts` spoken-reply
path **[V]**. Everything else — pairing, control, voice notes,
captions — works.

**Transport and pairing.** Pairing runs over **BLE through the Muse
app** (Settings > Devices, Developer mode on); the gadget appears as
`MuseGadget-XXXXXX`; pairing requires a **physical button press**
(BOOT) and creates a fresh encrypted session **[V]**. After pairing
the gadget is on home Wi-Fi and connected to Muse (status `ws=up`
in a first-hand build report **[S]** — i.e. a WebSocket session to
the Muse cloud; the exact cloud endpoint and session protocol are
**[U]** from primary sources). Meta's own warning, verbatim in
spirit: community pairing has no manufacturer verification and
cannot prevent an active man-in-the-middle attack; set up on a
network you trust **[V]**.

**Auth and account.** Every gadget needs an **SDK token** from
`gadgets.muse.ai` (Account > SDK tokens) **[V]**. The token ships
inside the firmware or sits in NVS if set later over serial
(`>sdk_token=…` → `@sdk_token ok`) **[V]**. The README is blunt:
treat the token "as an identifier rather than a password"; if it
leaks, revoke it and re-pair (hold the button 5 s) **[V]**. NVS
encryption is strongly recommended (`CONFIG_HOMEHUB_NVS_ENCRYPTION`,
key from an eFuse HMAC block) because NVS holds Wi-Fi credentials
and device tokens in plaintext otherwise **[V]**. One token covers
up to 50 devices, personal non-commercial use only, and the Apache
licence "does not grant any right to Gadget SDK Tokens" **[S]**.
Subscription: building a gadget needs a **Muse account**, and the
SDK Terms as reported set **no subscription requirement** (the
subscription is only for claiming the free Home Link dongle, US
only) **[S]**. **Muse availability in the owner's region
(Indonesia) is [U]** — reported US-only **[S]**; this is a
go/no-go check for the owner before any account-dependent work.

**Custom commands — the extension point that matters for QaonicOS.**
"To give Muse a command of its own, such as reading a sensor or
switching a relay, advertise it in `link.register` and handle it in
`main/app.c`." **[V]** This is the seam a QaonicOS port hangs off:
QaonicOS-side capabilities become Gadget commands, and Gadget-side
events (voice notes, captions) become QaonicOS-side inputs.

**Licence compatibility: clean, with noted exceptions.** Apache-2.0
overall **[V]** — compatible with the repo's Apache-2.0 code and
with the CMU-licensed Mach kernel (separate components, no
linking). Third-party exceptions retained upstream: minimp3 CC0-1.0,
libvpx VP8 BSD-3-Clause + patent grant, tinyh264 Apache-2.0 (AOSP),
Adafruit font BSD-2-Clause, one panel init table MIT **[V]**. The
Jollybot avatar is explicitly NOT covered by Apache-2.0 **[V]** —
irrelevant here: QaonicOS's face is procedural C, not the avatar.
The binding constraint is not copyright but the **Token Terms**
(personal use only, no sale / giveaway / promotion embedding)
**[S]** — a QaonicOS+Gadget product could not be distributed with
an embedded token.

## 2. What "port to QaonicOS/Luckfox" means — three shapes

### Shape (a) — RECOMMENDED: C6 runs Gadget firmware, QaonicOS talks to it over the C6 link

- **What runs where.** The XIAO ESP32-C6 runs (extended) Gadget
  firmware: BLE pairing, Wi-Fi, Muse WebSocket session, custom
  `link.register` commands. The RV1103 runs QaonicOS unchanged in
  architecture: face/uiapp/qabotd talk to a `linkd` daemon (C6L-5)
  that owns the UART transport; linkd bridges QaonicOS IPC clients
  to the C6.
- **What crosses the link.** Tool calls (QaonicOS capability →
  Gadget command), tool results, caption text (Muse reply → face),
  voice-note audio (RV1103 mic → C6 → Muse), and link events
  (pairing state, Wi-Fi state, Gadget session up/down). All of
  these map onto the C6L wire: TOOL.DISPATCH, EVT, and the v2 AUD
  frames — which is why this shape is feasible at all.
- **What breaks / what it costs.** Stock Gadget firmware does NOT
  speak the QaonicOS wire: the UART channel (a "QaonicOS companion
  channel" inside the Gadget firmware: receive tool calls over
  UART, emit captions/events over UART, accept PCM audio over
  UART) is new firmware work in the FW track — the single largest
  gap (§4, G1). The C6's UART to the RV1103 is also the link the
  Gadget session's own traffic does not use (Gadget cloud traffic
  is Wi-Fi), so there is no bandwidth contention between cloud
  and link traffic — only the UART budget in §3.

### Shape (b) — REJECTED: RV1103 runs a Gadget-compatible stack natively

The RV1103 would have to implement the Muse gadget protocol
itself: BLE pairing stack, Wi-Fi (it has none onboard — Wi-Fi
lives on the C6 by design), WebSocket session to the Muse cloud
with the undocumented session protocol **[U]**, plus the token and
session handling. QaonicOS has no BLE stack, no Wi-Fi hardware on
the RV1103, and no Linux-style userspace to host the Linux SDK
(the Linux SDK assumes systemd, a package system, and a BLE
Linux box). This shape discards the entire C6L architecture to
re-implement an undocumented cloud protocol on the wrong chip.
Rejected outright; it is recorded here only so it is not
re-proposed.

### Shape (c) — VIABLE VARIANT of (a): split-brain

The C6 runs Gadget firmware for the Muse session AND keeps
QaonicOS's own link services (Wi-Fi/BLE management for the
QaonicOS onboarding flow, KV store) on the same UART via
linkd multiplexing. In practice this is shape (a) with a
scheduling question: the Gadget session and QaonicOS link
traffic share one UART and one C6, so linkd (C6L-5) must
arbitrate, and the C6 firmware must serve both masters without
one starving the other. This is the long-term steady state if
QaonicOS keeps its own onboarding; the plan phases it after
shape (a) works, not instead of it. A fallback inside (c):
if the Gadget session is down, QaonicOS keeps its own
Mode A (direct LLM API) and Mode C (hybrid) paths — the
Gadget is an additional companion channel, never a
single point of failure for the device.

## 3. Hard constraints check

| Constraint | Verdict |
|---|---|
| **PSRAM (XIAO C6 has none)** | **RESOLVED [V].** Stock `c6-nopsram` profile exists; no-PSRAM C6 is a full gadget minus the home-network tunnel and minus `start_tts`. The earlier "tunnel needs PSRAM" note stands; "gadget needs PSRAM" does not. A PSRAM variant is only needed if the owner later wants the tunnel, spoken replies via `start_tts`, or the video protocol. |
| **Link budget at 921600 baud (~92 KB/s)** | **Voice fits, video does not.** QaonicOS voice is already 16 kHz × 16 bit = 32 KB/s PCM (§4, G2) — one third of the budget, and Opus-class compression (~12 kbps **[S]**) would be far less. Commands, captions and events are bytes-to-hundreds-of-bytes. The video protocol (H.264/VP8 30 fps) needs Wi-Fi-class bandwidth and PSRAM-class boards; it is **out of scope over UART permanently**. |
| **Single-TCP-connection guest constraint** | **QEMU-test artifact, not a hardware constraint.** The guest TCP stack is single-connection (C6L-2 dev log §7), which shapes how QEMU tests run. On hardware the C6 talks Wi-Fi directly to the Muse cloud; the RV1103↔C6 UART has no TCP at all. The plan keeps QEMU tests honest about this (mock peer = one client at a time). |
| **Cooperative scheduler + watchdog** | **Compatible by construction.** The Gadget session lives on the C6, not under the RV1103 scheduler. On the RV1103 side, linkd follows the standing rule from DEVLOG-IPC5 §11: park or yield, never spin 30 s without a switch (the FND-3 class-(b) detector). Caption/event delivery via EVT is async and bounded; no RV1103-side polling loop is needed. |
| **Provisioning vs ONB hybrid flow** | **Two pairings, one device — design gap (G4).** QaonicOS onboarding (D-ONB-1: one QR; phone via BLE or SoftAP captive portal; D-ONB-3: mandatory device PIN) provisions the *device*. Gadget pairing (Muse app, Developer mode, BOOT press, SDK token) provisions the *Muse session*. They are separate flows with separate secrets. The plan must decide: who enters the SDK token, where it is stored (C6 NVS is the live copy; QaonicOS credstore can hold a backup), and how revocation/re-pair is surfaced on the 240×240 screen. |
| **Credential storage vs credstore** | **Backup yes, live copy no.** QaonicOS credstore (`QSEC` format, AES-256-GCM, syscall 104) can hold the SDK token as a backed-up secret, but the Gadget firmware reads its token from its own NVS — the live copy lives on the C6. Token rotation therefore needs a defined sync step (webui or serial), not silent drift. NVS encryption on the C6 is the matching hardening step. |
| **TLS / mbedTLS reuse** | **No conflict.** Gadget firmware uses ESP-IDF's mbedTLS on the C6 for its own session; QaonicOS's mbedTLS (Mode A) on the RV1103 is untouched. No shared TLS state crosses the UART — only plaintext-over-UART tool/audio/caption frames inside the device's own trust boundary (physical-access caveat: UART between the two chips is unencrypted by design, same as today). |
| **Account / region** | **Owner check required [U].** Muse account + SDK token needed; subscription reportedly not needed for building **[S]**; Muse reported US-only **[S]**. If the Muse app / token issuance is unavailable in the owner's region, shape (a) is blocked at pairing regardless of engineering. Check first, build second. |
| **Display** | **Compatible.** The 240×240 ST7789 face shows captions exactly as Gadget UI boards do (text replies as captions **[V]**). The procedural face stays; no avatar asset is needed (and the Jollybot avatar is licence-excluded anyway). |

## 4. Gap analysis vs the C6L track

What C6L already provides (C6L-1..2 merged; C6L-3 in flight):
link layer with swappable backends, frozen wire v1.1 (text
commands + KV length-prefix + v2 AUD frames), bridge backend +
host Python mock peer, NET.HTTP, and — per C6L-3's spec —
TOOL.DISPATCH, EVT, and NET.WS framing as wire v1.2.

Ranked gaps (G1 = blocks everything else):

1. **G1 — UART "QaonicOS channel" inside Gadget firmware.**
   Stock firmware has no UART companion protocol. Needs: a
   UART task in the Gadget firmware that accepts tool calls
   (mapped to `link.register` commands), emits caption/event
   frames, and accepts PCM audio for voice notes. **Needs-board
   for validation** (XIAO C6 owned; Luckfox for the RV1103
   side); **design + mock-peer extension board-less**.
2. **G2 — Voice-note audio path.** RV1103 ACODEC PCM (16 kHz
   16-bit, the existing `VOICE.START 16000 16` contract) →
   UART AUD frames → C6 → Muse voice-note upload. The
   voice-note upload codec/format on the Gadget side is **[U]**
   from primary sources (Opus-class is the industry default
   **[S]**; the SDK's own MP3 playback path is for replies,
   not uploads). If the C6 must transcode PCM→upload-format,
   that is C6-side DSP work to size. **Board-less:** protocol
   design + mock-peer audio loop; **needs-board:** real mic
   attach + acoustic test.
3. **G3 — Caption/event path.** Muse text reply → C6 → UART
   EVT → linkd → face/uiapp caption rendering. Maps cleanly
   onto C6L-3 EVT; **board-less** end-to-end via the extended
   mock peer.
4. **G4 — Provisioning merge.** One device, two pairings, two
   secrets (device PIN vs SDK token). Needs a designed flow:
   token entry (serial now; webui later?), token backup in
   credstore, revocation/re-pair UX on the face, and the
   pairing-press mapping (XIAO BOOT button = Gadget confirm
   press; QaonicOS physical-confirm = its own buttons).
   **Board-less design; live pairing needs-board + account.**
5. **G5 — linkd arbitration (C6L-5 dependency).** linkd must
   multiplex Gadget-channel traffic with QaonicOS link traffic
   on one UART, with the park/yield rule, and report Gadget
   session up/down to qabotd. **Board-less design;** live
   arbitration test needs-board.
6. **G6 — XIAO ESP32-C6 board profile.** No XIAO C6 profile
   ships in the SDK (closest stock profile: `c6-nopsram`)
   **[V]**. A custom profile is small (copy the closest
   `devices/sdkconfig.*`, set chip/button/status-light/flash
   per the README **[V]**) but needs XIAO facts: 512 KB SRAM,
   4 MB flash, no PSRAM **[S]**; UART1 on D6 (TX, GPIO16) /
   D7 (RX, GPIO17) **[S]**; BOOT button present **[S]**; **no
   user LED on a clean GPIO** — status light needs an external
   LED or another indicator **[S]**. **Profile drafting
   board-less; build/flash/verify needs-board.** Build-side
   note: ESP-IDF v6.0.1 + toolchain is multi-GB; the standing
   rule (builds in GitHub Actions, PC storage ~5 GB free) means
   the ESP-IDF build should run in Actions (Espressif's IDF
   image) with flash/test on the PC — a CI decision for the
   FW track, not assumed here.
7. **G7 — Account, region, and Terms acceptance.** Muse
   account, token issuance, region availability, personal-use
   Terms. **Owner decision, no code.** Blocks G4's live test.
8. **G8 — Offline fallback contract.** Gadget = cloud terminal:
   no Wi-Fi ⇒ no Muse session **[S]**. QaonicOS must keep
   Mode A (direct LLM API) and offline behaviour working when
   the Gadget channel is down, and the face must say so
   honestly (cf. the out-of-box acceptance bar: first boot
   offline → device tells the user what to do). **Board-less
   design.**

## 5. Risks

- **Cloud/account dependency.** Every Gadget interaction routes
  through Meta's Muse cloud with a Meta account and a revocable
  token. Revocation, Terms change, or region restriction kills
  the feature remotely. Mitigation: Gadget is an *additional*
  Mode-B channel; Mode A/C keep working.
- **SDK maturity.** 22 days old, 0 releases, moving `main`,
  self-described unsupported hacker project. APIs (`link.register`,
  session protocol) can change without notice. Mitigation: pin
  a verified commit; wrap the Gadget channel behind linkd so a
  firmware change does not ripple into QaonicOS daemons.
- **Privacy path.** Voice notes, captions, and tool results
  leave the device to Meta's cloud; pairing has no manufacturer
  verification (MITM warning). Mitigation: physical-confirm
  for sensitive actions (already the QaonicOS rule, D-ONB-3
  family); document what leaves the device; never route the
  device PIN, credstore contents, or local files through the
  Gadget channel.
- **Token handling.** The token is an identifier, not a
  password **[V]**, but it is still a bearer-ish credential
  sitting in C6 NVS. Mitigation: NVS encryption on the C6,
  credstore backup on the RV1103, documented rotation flow.
- **Linux-SDK temptation (rejected).** The Linux SDK's
  `system.run` as the installing user (sudo included) is the
  wrong security shape for an appliance and has no host on
  QaonicOS anyway. Do not port it; the ESP32 path is the port.
- **Scope creep into video.** The video protocol is the most
  demo-able Gadget feature and the least portable: it needs
  Wi-Fi bandwidth and PSRAM-class boards. Explicitly out of
  scope for the UART link.

## 6. Verified vs unverified — load-bearing claims

| Claim | Standing |
|---|---|
| SDK is Meta's, Apache-2.0, repo `facebookincubator/muse-gadget-sdk`, created 2026-10-02 | **[V]** repo README, read 2026-10-24 |
| ESP-IDF v6.0.1 pinned, other versions unsupported | **[V]** `esp32/` README |
| `c6-nopsram` profile exists; no-PSRAM C6 = full gadget minus tunnel + `start_tts` | **[V]** `esp32/` README board table + PSRAM note |
| Pairing = BLE + Muse app Developer mode + BOOT press + fresh encrypted session; MITM warning | **[V]** `esp32/` README |
| Custom commands via `link.register` + `main/app.c` | **[V]** `esp32/` README |
| Token from gadgets.muse.ai; identifier-not-password; NVS encryption recommended | **[V]** `esp32/` README |
| Replies are text captions; spoken replies via third-party TTS | **[V]** `esp32/` README |
| Licence exceptions (minimp3 CC0, libvpx BSD-3, Jollybot excluded) | **[V]** repo + `esp32/` README licence sections |
| XIAO C6: 512 KB SRAM, 4 MB flash, no PSRAM, BOOT button, UART1 D6/D7, no clean user LED | **[S]** datasheet mirrors + community manual |
| One token = 50 devices, personal non-commercial use only | **[S]** Terms reporting (Terms text not read directly) |
| No subscription needed to build; subscription only for Home Link | **[S]** Terms reporting, secondary sources agree |
| Muse US-only / region availability for the owner | **[U]** — owner check required before account-dependent work |
| Gadget cloud session = WebSocket; voice-note upload codec | **[U]** — `ws=up` seen in a build report; codec not confirmed from primary sources |
| No XIAO C6 stock profile; custom profile = copy closest sdkconfig | **[V]** board table has no XIAO row; README documents the copy-a-board recipe |
