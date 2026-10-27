# DEVLOG-UI8 — strip dead programs + status-bar icon font

Two user-ordered jobs (2026-10-27) in one branch,
strip first, then icon-font revision. Worktree `qaonic_os-ui8`, branch
`dev/ui8`, base = UI-7 merge (push 258).
This log grows across the two commits: commit 1
covers the strip (sections 1-2, 4-6 here); the
icon-font sections arrive with commit 2.

Job 1 executes the UI-6 strip audit (DEVLOG-UI6
§2) for Tier C + Tier A, on the user's one-line
order "hapus semua yg program mati GK kepakai".
Job 2 replaces the UI-7 hand-drawn status-bar
masks with a real icon font, on the user's
feedback that the masks "masih ga bagus".

## 1. Job 1 — strip (Tier C + Tier A)

Method (same as UI-1..UI-7): `llvm-objcopy-18 -O
binary` on `mach3.elf`; headroom = 4,194,304 −
binary size. Census: `mach3-port/build/<prog>.bin`
(pad-bss included, so .bin size = embedded size).

### 1.1 Baseline (pre-strip, = QA image)

Kernel binary 4,014,648 B, headroom 179,656 B
(UI-7 post-change figure, reproduced exactly).
This build is preserved as `logs-ui8/qa-kernel.elf`
and is the QA image for the evidence below.

### 1.2 Tier C — utls pair: 1,511,528 B, removed

| Program | Bytes | Status |
| --- | --- | --- |
| utlscli | 753,884 | dead, removed |
| utlskeep | 757,644 | dead, removed |

Re-verified by grep (this tree): NO
`scripts/verify-*.py` references `utlscli` or
`utlskeep`. Removed: both build sections in
`build-md.sh`, the `gen/*_img.c` embed steps, the
`extern` declarations in `user.c`. (Their map
entries were already commented out, "Nonaktif",
and stay as comments.) Kept: the sources
(`user/utlscli.c`, `user/utlskeep.c`) buildable on
demand — restore the deleted sections from git
history (this commit's parent) or build the .bin
standalone with the `init.ld` pattern in
`build-md.sh` loop 1. Battery impact: ZERO.

One catch found while deleting: the utlscli
section also built `ca_pem.o` (pem2c.py from the
TLS test CA), and qabotd's link line still needs
it. The pem2c + ca_pem.o steps moved into the
TLS lib section (1d3), their natural home; the
TLS objects themselves are untouched.

### 1.3 Tier A — embed-map family: 320,623 B, removed

| Program | Bytes | Battery leg that referenced it | Disposition |
| --- | --- | --- | --- |
| ulinktest | 54,188 | C6L1 | leg runs on the QA image only |
| ulinkbr | 55,212 | C6L2 | leg runs on the QA image only |
| ulink3 | 51,444 | C6L3 | leg runs on the QA image only |
| ulinkaud | 56,756 | C6L4 | leg runs on the QA image only |
| ulink5 | 22,872 | C6L5 | leg runs on the QA image only |
| ulinktools | 11,592 | C6L5 | leg runs on the QA image only |
| ulink5die | 9,004 | C6L5 | leg runs on the QA image only |
| uipkpark | 9,792 | IPC2 | leg runs on the QA image only |
| uipknd | 9,308 | IPC2 | leg runs on the QA image only |
| uiptoolssrv | 11,464 | IPC2, IPC4 | leg runs on the QA image only |
| uipcregexit | 5,002 | IPC4 | leg runs on the QA image only |
| uipcregfull | 5,589 | IPC4 | leg runs on the QA image only |
| uipcaud | 18,400 | IPC5 | leg runs on the QA image only |

Removed: loop-1 list entries, the five
individual build sections (ulinktest, ulinkbr,
ulink3, ulinkaud, ulink5), the Phase C list
entries, the `extern` declarations, the `hooks[]`
entries, and the `uprogs[]` ulinktest entry in
`user.c`. Kept: all sources, plus a QA build
flag (below) so the programs stay buildable
in-tree.

No kernel-feature coverage is silently dropped:
every Tier A leg still runs — on the QA image,
which retains Tier A. The production image loses
only the ability to RUN those test programs, not
the kernel features they exercise (link layer,
bridge, wire v2, AUD, linkd client, IPC parks,
registry, AUD-rate queue — all still present in
the production kernel; only their test drivers
are gone).

### 1.4 QA build flag (the durable mechanism)

`QA=1 ./build-md.sh` builds the QA image: loop 1
+ Phase C include the Tier A programs, and
`user.c` compiles with `-DQA_TIER_A`, which
enables the Tier A `extern` declarations, the
`hooks[]` entries, and the `uprogs[]` ulinktest
entry. Default (QA unset/0) is the stripped
production image. Tier C is NOT behind the flag:
nothing references utls, so no QA leg needs it;
Tier C stays restorable from git history only
(§1.2). The flag is documented here and in the
`build-md.sh` comments, not in a separate script,
so it cannot drift from the build.

Evidence rule (UI-6 audit, followed): full
battery on the QA image + smoke + non-Tier-A
legs on the stripped production image (§5).

### 1.5 Totals

Tier C 1,511,528 + Tier A 320,623 =
**1,832,151 B stripped**. Headroom before
179,656 B → after [TO FILL from run] (objcopy
method). Tier B (30,860 B) deliberately NOT
stripped — see §2.

### 1.6 What the strip did NOT touch

`face`, `uiapp`, `qabotd`, `linkd`, `sh`
(production daemons), `uipcld` + `ulink5d`
(Tier B, §2), `SYS_STAT` semantics, the UI-5
Memory-row guard, any screen behavior, the
name-editor revert (UI-7).

## 2. Tier B impact brief (NOT executed — needs its own decision)

Tier B = `uipcld` (12,132 B, daemon_images
index 3) + `ulink5d` (18,728 B, index 5),
**30,860 B** total. Both are marker-gated
test daemons (dormant in production boots:
uipcld needs `/data/iptoolsc-test == "1"`,
ulink5d needs `/data/link5-test == "1"`), but
they are load-bearing in the daemon table.

### 2.1 Battery legs that need them

- IPC2: concurrent SEND/WAKE proof runs between
  the uipcld daemon and the uiptoolssrv mock
  `tools` server. Without uipcld there is no
  concurrent client daemon for this proof.
- IPC4: marker-gated client daemon (uipcld) for
  registry + concurrent-client legs.
- C6L5: ulink5d is the second linkd client for
  demultiplexing, EVT fan-out, the qlimit-5
  voice-rate measurement (its ok markers), and
  the client-death probe (ulink5die sends one
  raw CMD and exits; linkd must keep serving
  ulink5d).

Removing Tier B therefore does NOT just delete
two programs: it deletes the harnesses for
concurrent-client and second-client kernel
coverage. A Tier B removal must re-point that
coverage (new harness daemons) or explicitly
drop those legs — it cannot silently keep the
battery green.

### 2.2 Progid shift (the mechanical part)

Current `daemon_images[]`: face 0, uiapp 1,
qabotd 2, uipcld 3, linkd 4, ulink5d 5, sh 6
(+ spawn slot 7, NDAEMON 8). After removal:
face 0, uiapp 1, qabotd 2, linkd 3, sh 4
(+ spawn slot 5, NDAEMON 6).

### 2.3 Code that keys on progid (the checklist)

A Tier B removal PR must change ALL of these
together, or the image breaks:

1. `daemon_images[]` itself (remove 2 entries)
   + `NDAEMON` 8 → 6 + its comment.
2. `ipc_glue.c` `reg_reserved[]` (D-IPC-2):
   `{ "tools", 2 }` unchanged (qabotd stays
   index 2), but `{ "link", 4 }` MUST become
   `{ "link", 3 }` — otherwise linkd (now
   progid 3) CANNOT claim its reserved name
   and all of linkd IPC dies.
3. `user.c` watchdog deadline switch
   (`launch_daemon`): `case 6: /* sh */`
   (6000/12000 ticks = 60/120 s) MUST become
   `case 4` — otherwise sh falls into `default`
   (1500/3000 = 15/30 s) and the shell, which
   blocks on input, gets watchdog-killed after
   30 s idle. This is the least obvious coupling
   and the easiest to miss. (Better fix for that
   track: key deadlines by daemon name/role,
   not progid.)
4. `SH_IMAGE_IDX` is computed
   (`NDAEMON_IMAGES - 1`) — safe. `daemons[]`
   sizing, spawn slot (`NDAEMON - 1`),
   `wd_imgidx` respawn identity, and
   `launch_daemon_idx` all follow indices —
   safe, but re-verify by grep in that PR.
5. `hooks[]` progid overrides are unaffected:
   uiptoolssrv/ulinktools carry progid 2
   (qabotd index, for the `tools` claim) and
   ufaultd/uhangd carry PROG_UIAPP (1) — none
   of those indices move. `PROG_NTP` (2) is
   defined but unused — no impact.
6. Docs: DEVLOG-IPC4, DEVLOG-C6L5, and any
   text naming "linkd index 4 / ulink5d index 5
   / sh index 6" need index updates (docs only).

### 2.4 One-line approval form

If the user later orders "hapus Tier B juga",
execute §2.3 items 1–4 + re-point or drop the
§2.1 legs + full battery. Estimated shape: a
small kernel-adjacent track, not a strip.

## 3. Job 2 — status-bar icon font

### 3.1 Why a font, and which one

UI-7 masks: hand-drawn 1-bit point tables
(wifi 15x11, BT 8x11) composited into the flush
strip. User feedback 2026-10-27: "masih ga
bagus". A real icon font gives antialiased,
proportionally drawn glyphs at a readable size
instead of 11 px dot patterns.

Font choice, researched first: **Material
Symbols Outlined** (Google, Apache 2.0) —
official font, has exactly the needed glyphs
including real off variants (wifi_off,
bluetooth_disabled) and an empty-battery
outline (battery_0_bar). Lucide was NOT used:
its main distribution is SVG, not a font file,
so there is no clean Lucide font to subset
(the UI-7 masks only followed Lucide's shapes
as a convention). No hand-drawn glyph is left
in the status bar.

Source (downloaded by curl 2026-10-27, no
browser task):
`https://github.com/google/material-design-icons/raw/master/variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf`
— Material Symbols Outlined v2.978, 6,653
glyphs, variable axes FILL 0–1 / GRAD −50–200 /
opsz 20–48 / wght 100–700, default instance
(FILL 0 = outline, wght 400, opsz 24) used as
is. SHA-256 of the downloaded file:
`f7837ba8573d9a7a74931618ced8307bc81f96d49af296bf6c8052c772ed5282`.
License file: same repo, `LICENSE` (Apache
2.0). Codepoints file (same directory,
`.codepoints` suffix): wifi e63e, wifi_off
e648, bluetooth e1a7, bluetooth_disabled e1a9,
battery_0_bar ebdc (battery_horiz_000 f8ae is
the newer alias for the same empty outline;
battery_0_bar chosen as the classic name).

### 3.2 Subset (lv_font_conv, headless)

Tool: `lv_font_conv` (npm, installed in
scratch, not in the repo) run headless:
`lv_font_conv --font MaterialSymbolsOutlined.ttf
--size 18 --bpp 4 --format lvgl -r
0xE63E,0xE648,0xE1A7,0xE1A9,0xEBDC
--lv-font-name lv_font_qaon_icons
--no-compress -o lv_font_qaon_icons.c`. It
accepted the variable font directly (default
instance) — no static instantiation needed.
Output: `user/uiapp/lv_font_qaon_icons.c`,
byte-identical to the tool output
(regeneration command in its header comment),
6,860 B source, 1,708 B compiled object
in the real build
(5 glyphs, 18 px, 4 bpp antialiasing; line
height 17). No `lv_conf.h` change: the
generated file defaults `LV_FONT_QAON_ICONS`
to 1 itself. Build: one compile step + one
object in the uiapp link (see `build-md.sh`
UI-8 comments).

`--no-compress` is load-bearing, not
cosmetic. The first subset used lv_font_conv's
default RLE compression (`.bitmap_format =
1`); the vendored LVGL build has
`LV_USE_FONT_COMPRESSED 0` (lv_conf.h), so
`lv_font_get_bitmap_fmt_txt` returns NULL
for every glyph (with a log warning) and the
labels render empty. The first font battery
proved it: icons_ok v2 measured zero
icon-gray pixels on EVERY screen (menu,
list, detail, Monitor, Power) while the
Montserrat clock band rendered. The plain
subset (`.bitmap_format = 0`, the same
format as the in-tree Montserrat 14) is the
fix; enabling compressed-font support in
lv_conf.h was rejected as the wider change.

### 3.3 Rendering + binding rules (unchanged)

`lv_port.c` owns three labels on the top
layer (one glyph each), created by
`uilvgl_set_radio_icons()` — same call site
in `lv_shell.c` `build_status_bar()`, same
signature, so the UI-1 smoke screen (which
never calls it) stays icon-free, and labels
die with the display in `uilvgl_shell_end()`
(pointers cleared in `begin()` for the fresh
display). Layout: 240x24 bar, right-aligned,
6 px margin, 4 px gaps, y offset 3 (17 px
line height in a 24 px bar); batt at −6, BT
at −28, wifi at −50 (one 18 px advance each).
Color: the same dim gray 0x808080. OFF = the
font's real wifi_off / bluetooth_disabled
glyphs (no hand-drawn slash). Battery =
battery_0_bar outline only, never a level (no
ADC). QEMU defaults OFF/OFF (no radio,
honest). Live linkd wiring (WIFI.STATUS /
BLE.STATUS) stays deferred, same follow-up as
UI-7. The UI-7 mask tables, `paint_status_icons`,
and the flush-strip compositing are deleted;
`flush_cb` just forwards now.

### 3.4 Verify updates (icons_ok v2)

`verify-ui5.py` `icons_ok()` rewritten for
font glyphs: per-label gray counts (wifi box
172..190, BT 194..212, batt 216..234;
thresholds 25/15/15, tuned from the first
font dumps) + font-height proof (gray at
rows ≤ 5 AND ≥ 18 in the wifi box) + battery
interior empty. Test-first evidence: v2 run
against the UI-7 mask dump FAILS
(wifi-gray=34 bt-gray=44 batt-gray=42
wifi-top=False wifi-bottom=False
batt-empty=True) — the 11 px masks cannot
reach the font rows, so v2 discriminates
font from mask by construction, not by
threshold luck. Post-change (plain font):
every screen measures wifi-gray=120
bt-gray=77 batt-gray=78 wifi-top=True
wifi-bottom=True batt-empty=True — the
thresholds (25/15/15) pass with 3-5x
margin, and the height proof (rows ≤ 5
and ≥ 18) holds on all five screens.
Denominator: unchanged at 73; v2 changes
checks inside icons_ok, not the check
count. The bound retry loop (§3.4 note
above) is also test-only; UI5's denominator
stays 73.

Bound retry loop (UI-8 harness fix,
test-only, also in verify-ui5.py): the bound
SET now retries until the readback shows the
39-char prefix (up to 3 attempts), because
chunked delivery can still lose a whole
8-char chunk to PL011 RX FIFO overflow under
load (§5.2: the debug run stored 32 of 40
A's while the shell printed "name saved").
Same verdict checks, same count; the product
code is untouched.


## 4. Size

- Baseline (UI-7 tree, pre-strip): kernel
  binary 4,014,648 B, headroom 179,656 B.
- Post-strip (production, QA=0): kernel binary
  2,182,024 B, headroom 2,012,280 B (measured
  reduction 1,832,624 B vs the 1,832,151 B program
  sum; the 473 B delta is alignment/padding from
  the removed objects). Verified: no Tier A/C
  _img.o in obj-md, no Tier A/C symbols in the
  ELF (llvm-nm); Tier B (uipcld, ulink5d) still
  present; linkd 61,604 B (94.0% cap, unchanged);
  uiapp 442,992 B (unchanged).
- QA image (QA=1): kernel binary 2,503,112 B,
  headroom 1,691,192 B. Cross-check: pre-strip
  4,014,648 - Tier C 1,511,528 = 2,503,120,
  vs measured 2,503,112 (8 B alignment).
  llvm-nm confirms all 13 Tier A symbols +
  Tier B present, Tier C absent. Build log
  logs-ui8/qa-strip-build.log ("MD: 43 ok,
  0 failed" + "LINK OK"); kernel preserved
  as logs-ui8/qa-strip-kernel.elf.
- Post-strip + font (plain subset, commit 2):
  production binary 2,181,984 B, headroom
  2,012,320 B (net −40 B vs strip-only: the
  1,708 B font object costs less than the
  mask tables + compositing it replaced);
  QA binary 2,503,072 B, headroom 1,691,232 B
  (same −40 B net). Build logs
  logs-ui8/prod-font2-build.log and
  qa-font2-build.log; kernels preserved as
  prod-font-kernel.elf and qa-font-kernel.elf.
  The earlier compressed-font build measured
  2,181,864 / 2,502,952 B (net −160 B) but
  rendered zero icon pixels (§3.2) and was
  discarded; its logs are kept as
  fontprod-ui5-compressed.log.

## 5. Battery

### 5.1 QA image full battery (pre-strip build)

Run 1 (killed mid-FND3 by a VM/runtime restart,
not by a test): UI1 25/25, UI2 38/38, UI3 78/78,
UI4 72/73, UI5 72/73, FND1 PASS, FND2 21/21.
Remainder run: FND3 46/46, FND4 36/36, FND5
33/33, C6L1 29/29, C6L2 36/36, C6L3 72/72,
C6L4 60/60, C6L5 55/55, IPC2 34/34, IPC4 58/58,
IPC5 20/20, ONB1 PASS, ONB2 PASS, SHELL PASS,
SAFEMODE PASS, UI4 rerun 73/73, UI5 rerun
71/73. Standalone UI5: 72/73. Debug UI5 run:
72/73. Full logs: logs-ui8/qa-*.log,
qa-rest-summary.log, qa-ui5-standalone.log,
qa-ui5-dbg.log.

### 5.2 UI5 bound flake: proven harness delivery loss

UI4's run-1 failure ("sleep grant starts the
shell menu", 72/73) was a harness observation
race: every later check passed (Power opened,
Sleep marker, token released), so the menu
demonstrably worked; the rerun passed 73/73.

UI5 failed only the 40-char bound checks in
every run (72/73, 71/73, 72/73 standalone,
72/73 debug). The debug run captured the
serial: after a chunked set that reported
"name saved", bare `setup` printed
"  name: " + 32 A's. The check expects 39
(the shell name buffer holds 40 bytes incl.
NUL, so 39 is the max storable) — 32 = 40
minus exactly one 8-char chunk: chunked
delivery lost a whole chunk to PL011 RX FIFO
overflow while the guest drained slowly under
load, and the shell saved the short name
anyway ("name saved" does not validate
length). A quiet-host probe of the identical
chunked send passed cleanly (39 A's
read back). The detail dark check (> 20)
passes for any longish value and does not
discriminate length — the readback is the
length proof. Verdict: harness delivery
flake (the documented PL011 drain class),
NOT a product regression: storage, display,
and short-name writes all pass in the same
runs, and the code is the UI-7 merge (which
passed 73/73 in its rerun4). The bound
harness gets a retry-until-readback loop in
commit 2 (test-only, same verdict checks).

### 5.3 Production image: smoke + non-Tier-A legs

Chain run (44 min, full logs logs-ui8/
prod-*.log, prod-summary.log): UI1 25/25,
UI2 38/38, UI3 78/78, UI4 73/73, UI5 72/73
(bound readback only, §5.2 class), FND1
PASS, FND2 7/20, FND3 9/45, FND4 36/36,
FND5 33/33, ONB1 PASS, ONB2 finale pair
only, SHELL PASS, SAFEMODE PASS. Smoke
mapping: boot (every leg) + shell (SHELL)
+ GUI menu (UI2) + FAT ls /sd (SHELL) +
littlefs (FND4) — all green.

FND2/FND3 chain failures are NOT a strip
regression: both fail at the same first
step, "web login creates PIN + session",
while their serial legs pass (console
reclaim, fault isolation, spawn codes).
A scratch boot of the production kernel
(probe-weblogin.py, copies of the shared
images, port 18081) logs in cleanly: POST
/login pin=246810 -> 303 + qaon_sess
cookie + Location /settings. So the
production HTTP/PIN path works and the
shared PIN is 246810; the chain failures
are environmental (hostfwd/port contention
or a lingering QEMU at that point in the
chain — the documented chain-load class).
Per the standing rule they were re-run
standalone before any verdict; the rerun
outcome is recorded below.

Standalone reruns on the production image
(build/mach3.elf verified identical to
prod-kernel.elf by cmp): FND2 PASS 21/21,
FND3 PASS 46/46, ONB2 PASS (logs
prod-fnd2-rerun.log, prod-fnd3-rerun.log,
prod-onb2-rerun.log). The production
battery is therefore green except the
UI5 bound readback flake (§5.2).

Tier-A legs cannot run here by design;
they run on the QA image (§5.1).

### 5.4 Font battery (commit 2, plain subset)

Two-part evidence per the UI-6 rule, on the
final plain-font binaries (build/mach3.elf
verified by cmp before each part).

QA+font (Tier-A legs): C6L1 29/29, C6L2
36/36, C6L3 72/72, C6L4 60/60, C6L5 55/55,
IPC2 34/34, IPC4 58/58, IPC5 20/20 on
rerun (chain 19/20: only the trailing
"shell responsive at the end" probe failed
after boot 2 was already proven alive with
fail_count=0 — an end-of-run serial race).
Logs logs-ui8/fontqa-*.log.

PROD+font (all other legs): UI1 25/25,
UI2 38/38, UI3 78/78, UI4 73/73, FND1
PASS, FND2 21/21, FND3 46/46, FND4 36/36,
FND5 33/33, ONB1 PASS, SHELL PASS,
SAFEMODE PASS. UI5 chain 70/73: the three
failures were all bound-section (set +
detail-open + readback) under chain load;
the bound retry loop passed in the
compressed-font partial run and in both
standalone reruns, so the chain bound
failures are the §5.2 delivery class, not
the loop. UI5 rerun 72/73: only "setup
flow reaches finale" missed its marker
while every downstream check passed —
the UI4 grant-race class. ONB2 chain
finale pair only; rerun PASS. Second UI5
standalone rerun 72/73: only "back: shell
still reads KeepMe" — the post-release
window race documented in shell_probe's
docstring (a line consumed by qabotd while
sh never sees it); the same check passed
in the chain run and rerun 1. Three UI5
runs, three different single-race
signatures, all product behavior verified
in each run (icons 120/77/78 everywhere,
bound green in both reruns): the harness
race pattern, not a product regression.
No fourth run chasing 73/73; the number
is reported honestly as 72/73.

Compressed-font run (discarded build,
§3.2): the prod chain reached UI5 before
a runtime restart killed it; its UI5
partial log is the evidence for the
compression root cause (zero icon-gray
pixels on all screens) and for the bound
loop (bound section passed). UI1 25/25,
UI2 38/38, UI3 78/78, UI4 72/73 (only the
"restart grant starts the shell menu"
race) before the kill.

Visual gate: menu + name-detail + Power
dumps with the font icons in
logs-ui8/dumps/ui8-{menu,detail,power}.png
(+ .ppm), plus COMPARISON-ui7-vs-ui8.md.
The coordinator reads the dumps personally
before merge; the agent's own reading:
wifi_off, bluetooth_disabled, and the
battery outline render as real gray
glyphs, legible at 240x240, clearly
better than the 11 px masks.

## 6. Honest limitations / follow-ups

- Live radio state wiring for the icons (UI-7
  follow-up, unchanged by the strip and the
  font work; still deferred).
- Tier B removal (§2) needs its own decision.
- The structural fix (daemon images from
  littlefs, DEVLOG-UI6 §2) subsumes the strip
  + QA flag; still its own kernel track.
- The UI-6 flake follow-ups stand: `pin_entry`
  per-key marker verification (loss downstream
  of sh); the kernel VM accounting root
  (negative `vm_page_free_count`, DEVLOG-UI5
  §7); FND-3 respawn not covering face; blk
  probe retry/robustness.
- The QA flag keeps Tier A buildable, but the
  production image can no longer run C6L/IPC
  legs: any future leg that needs a Tier A
  program must run on a QA=1 build, by
  construction.
