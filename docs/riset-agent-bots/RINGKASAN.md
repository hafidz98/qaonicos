# Competitive Research: Physical AI Agent Bots / Companion Devices

Date: 2026-10-22. Purpose: survey of products similar to **Qabot**
(the on-device agent runtime for QaonicOS on Luckfox Pico Mini:
Cortex-A7, 64 MB RAM, 240x240 display, procedural face, physical
buttons, ESP32-C6 companion for Wi-Fi/BLE/voice), to see what
functions and features the category actually ships — and what
kills products in it.

Prices are USD list prices as reported in October 2026 unless
noted. Facts that could not be verified are marked *unverified*.

## 1. Product notes

### 1.1 Agent wearables & pendants

**Rabbit R1** (Rabbit Inc.) — Handheld 78x78 mm square, 115 g,
Teenage Engineering design. Input: push-to-talk button (not
always-listening), touchscreen, scroll wheel, 8 MP rotating
camera. Display: 2.88" TFT, card UI, no face/avatar. AI: cloud —
proprietary Large Action Model (LAM) for operating web UIs,
Perplexity for answers; rabbitOS 3 (Sep 2026) is BYOK (Anthropic,
OpenAI, OpenRouter, Ollama, Z.ai, local). Features: LAM actions
(launched with only Uber/DoorDash/Spotify/Midjourney), Teach
Mode, translation (100+ languages), Magic Camera, meeting
recording + summaries, memory recall, "Creations" (voice-generated
mini-apps). Connectivity: Wi-Fi, 4G SIM, BT, GPS; no phone app by
design — Rabbit Hole web portal. Price: $199, no subscription.
Status: alive but **pivoted from hardware to software** — R1 no
longer manufactured per WIRED reporting, no R2 planned; focus is
cross-platform OS3. Sources: en.wikipedia.org/wiki/Rabbit_r1 ;
techdogs.com (WIRED via TechDogs, OS3 coverage).

**Humane AI Pin** (Humane Inc.) — Screenless chest pin, ~34 g,
magnetic battery booster. Input: tap-and-hold voice (no wake
word), touchpad gestures, 13 MP camera + depth sensor. Display:
none — 720p laser projection onto the palm. AI: cloud CosmOS,
GPT-4-class models (exact model never officially confirmed).
Features: standalone phone replacement — own-number calls/texts,
email drafting/summaries, translation, vision/food ID, Tidal,
notes. Connectivity: LTE eSIM (T-Mobile US), Wi-Fi, BT; managed
via Humane Center web portal. Price: $699 launch, cut to $499,
**plus mandatory $24/mo**. Status: **dead** — HP acquired the
assets/CosmOS/team for $116M (Feb 2025); servers shut down
2025-02-28 and every Pin lost calls, messaging and AI. Sources:
techcrunch.com/2025/02/18 (HP acquisition) ; 9to5google.com.

**Friend** (Avi Schiffmann) — ~2-inch pendant disc on a cord.
Input: always-listening single mic, tap to talk; no camera;
Gen 1 text-only replies, Friend 2.0 (Jul 2026) adds a speaker
with a fixed random voice/personality. Display: none, one LED.
AI: cloud; provider has changed over time (Claude reported for
Gen 1, Gemini in 2026 coverage, OpenAI cited for 2.0 — single
current provider *unverified*). Features: companionship ONLY,
by design — proactive unsolicited text commentary on overheard
life, conversational memory (2.0: 30 days free). No reminders,
no actions, no productivity. Connectivity: BLE to iPhone app
only. Price: $99 preorder → $129 → 2.0 at $249 + ~$10/mo memory
extension. Status: alive but niche/struggling (~3,000 sold by
autumn 2025). Sources: techcrunch.com/2026/07/30 ;
the-gadgeteer.com/2026/07/31.

**Omi** (Based Hardware) — Circular disc pendant (necklace/
clip/wrist/temple). Input: always-on dual mics, no wake word,
offline capture with later sync; no camera. Display: none, LED
only; all output in the app. AI: cloud by default (OpenAI +
Anthropic subprocessors), **fully open-source (MIT) app +
firmware + backend, self-hostable/local mode**; on-phone
transcription on the free plan. Features: productivity/memory —
real-time transcription, summaries, auto-extracted action
items, reminders, searchable memory / "Ask Omi", daily recaps,
1,000+ app integrations, REST API, webhooks, hosted MCP server,
Personas. Connectivity: BLE + Wi-Fi (nRF5340/nRF7002), apps for
iOS/Android/Mac/web. Price: $129 (list $179; launched $89); free
plan with 300 cloud min/mo; Unlimited $29.99/mo. Status: alive,
shipping worldwide — the open-source survivor of the category.
Sources: testedintelligence.com (Omi review) ; techrepublic.com.

**Limitless Pendant** (Limitless, ex-Rewind) — ~32 mm aluminium
pendant, ~100 h battery. Input: voice only, beamforming mics;
tap = bookmark, tap-and-hold = ask. No camera, no display (LED).
AI: cloud; LLM provider *unverified*; "Consent Mode" for new
voices (off by default). Features: always-on recording,
real-time transcription, meeting notes/summaries, searchable
memory of everything heard, meeting prep, Ask over past
conversations. Connectivity: BT + Wi-Fi; web/Mac/Windows/mobile
apps. Price: $99 + Pro ~$19/mo originally. Status: **acquired
by Meta (2025-12-05)** — new sales stopped immediately, Rewind
app shut down, service withdrawn in EU/UK and several other
regions; existing users supported ~1 year under Meta terms.
Sources: srnnews.com ; morningstar.com (Meta acquisition).

**Plaud Note / NotePin** (Plaud) — Note: credit-card recorder,
3 mm, 30 g, magnetically stuck to a phone; NotePin: 16.7 g
wearable capsule (pin/clip/necklace/wrist). Input: voice, 2
MEMS mics (+ vibration sensor on Note for phone-call capture);
long-press record, tap to highlight. Display: none (Note Pro
variant adds a small AMOLED status screen). AI: cloud "Plaud
Intelligence" — user-selectable frontier LLMs (GPT / Gemini
Pro / Claude Sonnet per Plaud support). Features: transcription
in 112 languages with speaker diarization, AI summaries from
10,000+ templates, mind maps, "Ask Plaud" Q&A over recordings,
automated export workflows, custom vocabulary. Connectivity:
BLE/Wi-Fi; app + web + desktop. Price: $159 each; free tier
300 min/mo, Pro $99.99/yr (1,200 min), Unlimited $239.99/yr.
Status: alive, independent, actively developed. Sources:
plaud.ai ; support.plaud.ai.

### 1.2 Desktop companion robots

**EMO** (Living.AI) — Desktop pet robot, 117 mm, walks on two
legs, self-docks ("skateboard" / Home Station). Input: 4-mic
far-field voice ("Hey EMO"), wide-angle camera (face recognition
for up to 10 people), head touch sensor, TOF/anti-drop/gyro.
Face: animated screen face, 1,000+ expressions; no text UI. AI:
hybrid — on-device neural processor (recognition, navigation,
behaviour) + cloud ChatGPT for open conversation (version
*unverified*), free, no subscription. Features: evolving
pet personality, greets people by name, dances/sings, games,
weather/time, alarms/reminders, birthdays, bundled smart light
control, Bluetooth speaker. Connectivity: Wi-Fi + BLE; app for
setup/games/OTA. Price: $279 (Go Home $369). Status: alive,
ongoing firmware updates. Sources: keyirobot.com (EMO guide) ;
forums.living.ai.

**Eilik** (Energize Lab) — Stationary desktop robot, 108x105x
133 mm, servo-animated arms/body; does not roam. Input: **touch
+ vibration only — no mic, no camera, no voice**. Face: 1.54"
OLED 128x64 animated eyes, 100+ expressions. AI: **none** —
fully offline pre-programmed "emotion engine" ("No Wi-Fi, No
AI"). Features: idle emotional animations, touch-reactive
moods, mini-games, fear-of-heights behaviour, two Eiliks
recognise and play with each other; updates via USB desktop
app. Connectivity/app: none for the base unit. (Separate
~$99 AI Station add-on and the Wi-Fi Eiliko model add cloud
chat; the Eiliko line had serious cloud privacy flaws reported
by The Verge, Oct 2026 — the offline base unit is unaffected.)
Price: $139.99, no subscription. Status: alive. Sources:
energizelab.com ; ankicozmorobot.com.

**Loona** (KEYi Tech) — Wheeled petbot, 2.42 lb, auto-docking.
Input: 4-mic voice, touch, gestures; camera for face/object
recognition and following. Face: 2.4" LCD animated eyes. AI:
cloud ChatGPT/ChatGPT-4o conversation (via Amazon Lex +
ChatGPT per retailer specs); on-device A53 + 5 TOPS BPU for
navigation/recognition. No subscription per KEYi. Features:
roaming/following pet behaviour, per-person face recognition,
games (fetch, laser chase, ChatGPT RPG/guessing), home
monitoring via 720p camera, Blockly programming in app.
Connectivity: dual-band Wi-Fi, BLE 5.0; full companion app.
Price: ~$499.90 (sales ~$379–449). Status: alive, 2024+
hardware refresh. Sources: keyirobot.com ; homecrux.com.

**Vector / Vector 2.0** (Anki → Digital Dream Labs) —
Palm-sized treaded desk robot with lift arm and interactive
Cube. Input: "Hey Vector" 4-mic array, back touch sensor, HD
camera (face recognition). Face: colour IPS 184x96 animated
eyes (ex-Pixar-style animation), also shows answers/weather.
AI: on-device Snapdragon for perception/navigation; **voice,
Q&A, ChatGPT and Alexa require a DDL cloud subscription** —
without it the robot goes largely mute. Community alternatives:
Escape Pod (paid local server), Wire-Pod (free). Features:
autonomous roaming, edge avoidance, face recognition by name,
weather/timers/photos, blackjack, Cube games, Alexa, OSKR
open kit. Connectivity: Wi-Fi + BT; app for setup only. Price:
~$199.99 hardware **+ $11.99/mo or $99.99/yr subscription**.
Status: mixed/at-risk — Anki shut down in 2019; DDL revived
the brand and still sells Vector 2.0, but a July 2026 community
investigation claims DDL's infrastructure is under a court
receiver (*unverified*). Sources: ankicozmorobot.com ;
pixlrobotics.com.

**Looi** (TangibleFuture) — Small wheeled base (110x98x110 mm,
385 g); **your smartphone magnetically mounts as its head**.
Input: via the phone — voice ("Hey LOOI"), camera for face/
object/gesture recognition; base adds touch/ToF/cliff sensors.
Face: the phone screen (animated eyes); base has LED bar. AI:
cloud via phone — GPT-4o integrated (Gemini voice mentioned in
later listings, mix *unverified*); "Biomimetic Behavior Engine"
in-app for evolving personality/memory. Features: pet-like
behaviours (1,200+ actions), conversation/storytelling with
memory, games, face-tracking video calls, Auto-Snap photos,
clock/weather/schedule reminders, 10 W wireless phone charging.
Connectivity: phone over BT + Wi-Fi; the app is the brain and
is required. Price: ~$169–189. Status: alive (Kickstarter
2024, app updated Jul 2026). Sources: hackster.io ; keyirobot.com.

**Ropet / KAMOMO** — Stationary plush desktop pet, ~20 cm,
interchangeable fur/outfits; does not roam. Input: multi-touch
petting sensors, mic, camera (face/gesture/object), motion
sensors. Face: domed-lens full-colour eye displays, hundreds
of customisable eye styles via app; no full face screen. AI:
hybrid — lifelike behaviour/recognition/personality on-device;
GPT conversation unlocks after 7 days of care (cloud, free per
official site); free-form chat deliberately limited to preserve
"creature feel". Features: emotional companionship, personality
grows with care, mood reactions, dancing, ~39 °C constant
warmth, dress-up, Ropet-to-Ropet interaction. Connectivity: BT
to app (customisation, unlocks, updates). Price: $279.99
current store (Kickstarter was $169/$189). Status: alive/
growing (~20,000 units and a $10M+ round reported). Sources:
ropetai.com ; hackster.io.

**Mirumi** (Yukai Engineering) — Tiny furry charm robot that
clings to bag straps; USB-C, ~8 h/charge. Input: proximity +
IMU + head touch (+ sound reaction in production reports) —
**no voice understanding, no camera**. Face: none, screen-free
and silent; expression is head motion only (peek, nod, shy
head-hiding). AI: **no LLM, no cloud, no app** — proprietary
on-device behaviour algorithm. Features: companionship by
presence only — looks around curiously, gazes at nearby
people, hides when startled. No assistant functions at all.
Price: ~$118 Kickstarter, ~$150 planned retail. Status: alive,
early delivery phase (Kickstarter late 2025, shipping from
~Apr–May 2026). Sources: engadget.com (CES 2025 hands-on) ;
techbuzz.ai.

### 1.3 AI glasses (wearable comparators)

**Ray-Ban Meta** (Meta) — Normal Wayfarer glasses, ~50 g.
Input: "Hey Meta" voice, temple touchpad, capture button,
5-mic array, nose-bridge camera. Display: none (audio-only;
a separate Display model exists). AI: cloud Meta AI (Llama
family; exact serving model *unverified*) via phone. Features:
live camera-aware AI conversation, visual Q&A, live translation
(Gen 2: 6 languages incl. offline packs), reminders/memory,
WhatsApp/Messenger calls, music, FB/IG livestream. Gen 1: 12 MP
/ 1080p / ~4 h; Gen 2: 3K video, ~8 h. Connectivity: BT 5.2/5.3,
Wi-Fi 6; Meta AI app required. Price: Gen 1 from $299, Gen 2
from $379. Status: on sale; Gen 3 launched Sep 2026 (from
$449). Sources: meta.com/ai-glasses ; digitaltrends.com.

**Brilliant Labs Frame → Halo** — Open-source round glasses,
~40 g. Input: voice, nose-bridge camera (Frame: 1280x720;
Halo: low-power optical sensor for inference, not POV capture),
IMU/tap. Display: monocular HUD, 640x400 micro-OLED, ~20° FOV.
AI: "Noa" via app — Frame: cloud (OpenAI vision, Perplexity
search, Whisper translation); Halo: hybrid with on-device NPU
(embeddings on-device; exact split *unverified*). Features:
visual Q&A, web search, translation; Halo adds "Narrative"
long-term memory and "Vibe Mode" (build mini-apps by natural
language), ~14 h claimed battery. Connectivity: BT to phone,
open-source hardware + SDK. Price: Frame $349; Halo $299
pre-order → $349 retail (some roundups say $399 — *unverified/
conflicting*). Status: Frame sold out, superseded by Halo
(announced Jul 2025, shipping from late 2025, software early).
Sources: engadget.com ; roadtovr.com.

**Halliday** — Retro glasses, 35 g, **no camera by design**.
Input: voice, frame controls, and a **companion ring with
trackpad** (Gen 1 signature). Display: Gen 1 "DigiWindow" —
tiny green-text projection above the right eye (perceived
3.5"); G2: binocular waveguide micro-LED. AI: cloud via phone;
**proactive agent** that listens and offers answers unprompted;
models undisclosed (*unverified*). Features: real-time
translation/captions (40–45+ languages), **teleprompter /
"Cheatsheet"**, meeting transcription + summaries, G2
"Meeting Flow" (live captions, decision/commitment tracking),
notifications, navigation, voice memos. Price: Gen 1 $489
(Kickstarter $369); G2 $599. Status: Gen 1 shipping since Mar
2025; G2 shipping from Sep 2026. Sources: techcrunch.com ;
androidguys.com.

## 2. Feature matrix (compact)

| Product | Form | Face/display | Voice | Camera | On-device AI | Memory features | Actions/agent | Subscr. | Status |
|---|---|---|---|---|---|---|---|---|---|
| Rabbit R1 | handheld | 2.88" touch UI | PTT | yes | no | notes recall | LAM web actions | none | alive, HW ended |
| Humane Pin | chest pin | laser on palm | tap-hold | yes | minimal | notes | routed "experiences" | $24/mo req. | **dead 2025** |
| Friend | pendant | none (LED) | always-on | no | no | conversational | none (by design) | opt. memory | alive, niche |
| Omi | pendant | none (LED) | always-on | no | STT option | full, searchable | tasks, 1k+ apps, MCP | opt. | alive |
| Limitless | pendant | none (LED) | always-on | no | no | full, searchable | Ask over memory | was ~$19/mo | **acquired by Meta** |
| Plaud Note/Pin | card / pin | none | record | no | no | transcripts | summaries, Ask | tiered mins | alive |
| EMO | desk robot | animated face | wake word | yes | behaviour/recog. | birthdays, names | limited smart light | none | alive |
| Eilik | desk robot | OLED eyes | **no** | no | emotion engine | no | no | none | alive |
| Loona | wheeled pet | LCD face | yes | yes | navigation/recog. | per-person faces | games, monitoring | none | alive |
| Vector 2.0 | desk robot | IPS eyes | wake word | yes | perception | faces by name | Alexa, Cube | **$11.99/mo req.** | at-risk |
| Looi | phone-base | phone screen | via phone | via phone | in app | personality | reminders | none | alive |
| Ropet | plush pet | eye displays | yes | yes | behaviour | personality | no | none | alive |
| Mirumi | bag charm | none (motion) | no | no | behaviour algo | no | no | none | alive |
| Ray-Ban Meta | glasses | none (audio) | wake word | yes | minimal | reminders | calls/messages | none | alive |
| Brilliant Halo | glasses | mono HUD | yes | sensor | NPU embeddings | "Narrative" | Vibe Mode apps | opt. Plus | shipping, early |
| Halliday | glasses | mono/binoc. | yes | **no** | no | meeting memory | proactive agent | *unverified* | alive |

## 3. Recurring patterns

1. **The brain is in the cloud, the body is a peripheral.**
   Almost every product is a mic/camera/display endpoint for a
   cloud LLM. The only meaningful on-device intelligence is
   perception (face/voice recognition, navigation) and scripted
   behaviour — EMO, Eilik, Ropet and Mirumi run their entire
   personality layer offline.
2. **Push-to-talk or wake word beats always-listening.**
   Always-on products (Friend, Limitless) drew the strongest
   privacy backlash; survivors use PTT (Rabbit, EMO-style wake
   words, Plaud's explicit record switch, Muse Gadgets' PTT).
3. **The winning utility is memory, not chat.** The products
   still growing (Plaud, Omi) sell transcription → summaries →
   searchable memory → extracted tasks. Companionship-only
   (Friend) stays niche; presence-only (Mirumi) is a charm,
   not a platform.
4. **Faces are animated, procedural and LLM-free.** EMO
   (1,000+ expressions), Eilik (OLED eyes), Loona, Vector and
   Ropet all render eyes/expressions locally; nobody spends
   LLM tokens on the face. Personality comes from behaviour
   state machines, not model calls.
5. **A companion app is mandatory infrastructure** — setup,
   memory browsing, firmware OTA, subscription management.
   The two products that refused a phone app (Rabbit's portal,
   Humane's portal) are the two biggest failures.
6. **Subscriptions decide who lives.** Core-function paywalls
   are toxic (Vector goes mute without one; Humane's $24/mo
   was mandatory). Survivors either charge nothing extra
   (EMO, Eilik, Loona) or sell metered utility (Plaud minutes,
   Omi cloud minutes) with a genuinely usable free/local tier.
7. **Openness is a survival strategy.** Omi (MIT everything,
   self-host), Brilliant Labs (open hardware/SDK), Rabbit's
   pivot to BYOK — products that let users bring their own
   keys/models outlive their cloud.
8. **Proactive behaviour is the new differentiator** —
   Halliday's unprompted answers, Friend's unsolicited texts,
   Halo's Narrative memory, Muse Gadgets' device-initiated
   messages. Everyone is moving from "answers when asked" to
   "speaks first, carefully".
9. **Physical confirmation for pairing/actions** is becoming
   standard (button press to pair on Muse Gadgets, tap
   gestures, Plaud's physical record switch).

## 4. What Qabot can adopt (mapped to QaonicOS today)

| Category pattern | Qabot mapping |
|---|---|
| Procedural animated face, offline personality (EMO/Eilik) | Already Qabot's design: procedural face on the 240x240 display, expression state machine, no LLM in the render path. Add Eilik-style idle behaviours and touch/button-reactive moods — zero cloud cost. |
| Push-to-talk via physical button (Rabbit, Muse Gadgets) | Qabot's physical buttons are the PTT. Avoids the always-listening backlash; matches the existing button-only input decision (no keyboard). C6 carries the audio (VOICE uplink already designed at 921600 baud). |
| Transcription → summary → tasks pipeline (Plaud/Omi) | Mode A (direct LLM API): voice note → STT → summary → action items stored in littlefs, shown on face/web. The single most proven revenue feature in the category. |
| Searchable memory + daily recap (Omi/Limitless) | Qabot already logs events (bootlog pattern); extend to a conversation/event log on /data with a "recap" face screen and web view behind the PIN. |
| Captions/text replies on the device screen (Muse Gadgets, Halliday teleprompter) | The 240x240 face can page LLM replies as captions and act as a mini-teleprompter/reminder display — fits the existing display-token architecture. |
| BYOK / open models (Rabbit OS3, Omi) | Already Mode A policy (user's own API key, D-ONB-6 offline when empty). Keep it — it is the survivors' pattern. |
| Physical-confirm pairing + PIN gate (Muse Gadgets, D-ONB-3) | Already implemented (PIN + web settings). Add the Gadget-style rule: sensitive voice actions require a physical button confirm. |
| Device-initiated messages (Muse Gadgets `send-user-msg`) | Mode B: Qabot can push events (boot failures, reminders, sensor triggers) to the harness chat instead of only answering — cheap to add over the existing UART tool protocol. |
| Phone-as-brain fallback (Looi) | Mode C hybrid already covers this: phone/Jivix as brain, Qabot as face/body. |
| Two-unit social behaviour (Eilik pairs) | Later: two Qabots recognising each other over BLE via C6 — cheap delight, no cloud. |

## 5. What to avoid (lessons from the failures)

1. **Cloud-only = brick-on-shutdown.** Humane's Pin became
   e-waste the day HP turned the servers off; Vector owners
   are one receivership away from the same. Qabot must keep
   its offline core (face, reminders, local tools) fully
   functional with no server — D-ONB-6 already points this way;
   treat it as a hard invariant, not a fallback.
2. **Don't promise a general action agent at launch.** Rabbit's
   LAM shipped with 4 working services against "it operates
   your apps" marketing; the gap defined the product's
   reputation. Ship a short list of verified tools (Qabot's
   policy-gated tool registry is the right shape).
3. **No mandatory subscription for core functions.** Humane
   ($24/mo) and Vector ($11.99/mo to keep its voice) are the
   cautionary tales. Meter optional cloud extras if anything.
4. **Always-listening needs visible consent mechanics.**
   Friend's backlash and Limitless's bolted-on Consent Mode
   show the cost of getting this wrong; recording indicators
   and PTT are cheaper than apologies. (Also: Eiliko's cloud
   conversation-storage breach, reported Oct 2026 — if voice
   leaves the device, its storage is part of the product.)
5. **Don't skip the companion app.** Rabbit and Humane tried
   portal-only management; the market expects an app for
   setup, memory and OTA. Qabot's hybrid onboarding (BLE app
   or SoftAP portal, D-ONB-1) already avoids this trap —
   keep both paths.
6. **Screenless + premium price + phone replacement claims**
   (Humane, $699) collapse under review scrutiny. Qabot's
   honest scope — a desk companion with a face — is the
   segment that survived (EMO/Eilik/Ropet/Mirumi all sell
   presence, priced $70–280).
7. **Hardware without a software runway.** Rabbit stopped
   manufacturing within ~2 years. For Qabot: the C6 link and
   OTA A/B updates (already built, Fase B4) matter more than
   any single hardware revision.

## 6. Muse Gadget SDK — status check (for the Mode B question)

Repo: **github.com/facebookincubator/muse-gadget-sdk** —
verified live on 2026-10-22.

- **Stats:** created 2026-10-02; **2,364 stars, 511 forks,
  127 commits**, Apache-2.0, no releases yet; actively
  developed (community issues still being answered on
  2026-10-20; HEAD `e6fe517`). Announced as a hacker side
  project (Nat Friedman); a community Discord is linked.
- **What it is:** open-source firmware/SDKs that turn DIY
  hardware into peripherals ("gadgets") for the Muse app/
  agent. Every gadget needs an **SDK token** from
  gadgets.muse.ai and pairs with the Muse app (iOS/Android)
  via Settings > Devices with **Developer mode** on; pairing
  requires a **physical button press** on the device and
  creates a fresh encrypted session. Gadgets appear as
  `MuseGadget-XXXXXX`.
- **ESP32 Device SDK** (`esp32/`): ESP-IDF v6.0.1 firmware for
  ESP32 / C5 / **C6** / S3 — ~35 boards supported, including
  the **ESP32-C6 devkit (no PSRAM)** and many round/AMOLED
  display boards. UI boards run an animated avatar,
  push-to-talk and settings; voice notes are transcribed by
  Muse and **replies come back as text captions** (spoken
  replies = DIY via a third-party TTS endpoint, `start_tts()`).
  Boards with PSRAM additionally get the **home-network
  tunnel** ("Home Link": Muse can reach devices on your LAN
  and anything with a local HTTP API); **no-PSRAM boards like
  the C6 run without the tunnel** but stay fully controllable.
  Custom device commands are advertised in `link.register`
  and handled in `main/app.c`. Also in the box: a web flasher,
  an LVGL/SDL desktop simulator, a video protocol (phone
  camera → board screen, H.264/VP8, 30 fps), and power-
  profiling tooling.
- **Linux Device SDK** (`linux/`): turns a Raspberry Pi / any
  BLE Linux box into a gadget (systemd service in
  `/opt/musegadget`). Muse gets exactly four commands —
  `system.run`, `file.read`, `file.write`, `device.health` —
  executed **as the installing user (sudo included if the
  account has it)**. Local programs can push messages into
  Muse chats via `musegadget send-user-msg` (with
  `--session-id` for side chats).
- **Skills catalog** (`skills/`): community-contributed
  **device skills as Markdown** — `gadget-<device>/SKILL.md`
  files (name, description, instructions, limits, sources)
  that teach Muse to operate Home Link-reachable devices
  (the README cites Google Cast as a shared skill example).
  Not official integrations; you paste a SKILL.md into a Muse
  chat or point Muse at the repo. Meta notes they "may
  eventually be made available in the Muse product."
- **Meta's own hardware:** Muse Home Link — a plug-in bridge
  device (free with an active Muse subscription, US only,
  5,000-unit first batch, shipping October 2026 per daily.dev
  reporting). A keychain "Muse Charm" gadget has been reported
  for December 2026 (secondary sources — treat as unconfirmed).

**Relevance to Qabot.** This is a concrete, shipping
implementation of Qabot's **Mode B** (device as companion to a
host harness) — with two integration paths that need no new
QaonicOS work to evaluate:
1. The **ESP32-C6 is a first-class target**: Qabot's companion
   chip could run the Gadget firmware (or a Gadget-compatible
   profile) and expose QaonicOS tools as custom `link.register`
   commands, with the RV1103 side unchanged behind the UART
   protocol. The C6's lack of PSRAM only costs the LAN tunnel.
2. The pairing UX validates D-ONB directly: token + app +
   physical-press confirmation is exactly Meta's flow.
   Caveats: a gadget is useless without the Muse app/subscrip-
   tion behind it (the Humane lesson applies to Meta too),
   pairing has no manufacturer verification (MITM caveat in
   their own README), and the SDK is 3 weeks old with no
   releases — promising reference and possible Mode B target,
   not a foundation to depend on yet.

## Sources

Primary sources are listed per product above; SDK figures from
the repository itself (github.com/facebookincubator/
muse-gadget-sdk, retrieved 2026-10-22) plus daily.dev and
byteiota coverage of the launch. Items marked *unverified*
could not be confirmed against a primary source.
