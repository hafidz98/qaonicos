# DEVLOG-UI1 — LVGL vendor + port + measure

Phase: UI-1 (first phase of the UI track).
Branch: `dev/ui1` (worktree `~/workspace/qaonic_os-ui1`).
Base: C6L-5 merge (`9274c3e`).
Status: DONE, verified. No merge, no push (coordinator's job).

Location note: this track has no `docs/rencana-ui/` plan
yet (the UI plan lives as the phased sketch in
`docs/riset-ui-framework/RINGKASAN.md` §7, which is the
spec UI-1 implements). This dev log starts that
directory, mirroring the `docs/rencana-<track>/`
convention of the other tracks.

## 1. What was built

LVGL v9.5.0 (tag `v9.5.0`, commit `85aa60d`, MIT —
`LICENCE.txt` vendored) inside uiapp only, exactly the
three callbacks the research names. No kernel change.
No face change: the face stays hand-rolled.

### 1.1 Vendor layout: `user/uiapp/lvgl/`

- `lvgl.h`, `lv_version.h`, `LICENCE.txt` (upstream root).
- `src/` subset, 131 `.c` files, 9.0 MB on disk:
  full `core`, `display`, `draw` (top-level + `sw/` +
  `convert/`), `font` (`lv_font.c`, Montserrat 14,
  `fmt_txt/`), `indev`, `layouts`, `misc`, `osal`
  (none backend only), `stdlib` (builtin variants),
  `themes` (`default` only), `tick`,
  `widgets/button`, `widgets/label`,
  `libs/bin_decoder` + `libs/rle` (see §3.4).
- Headers for non-vendored subsystems (`drivers/`,
  `libs/`, `others/`, `debugging/`) are vendored
  header-only: they are needed to parse
  `lv_init.c`'s includes, but no `.c` from them is
  compiled, so they cost zero binary bytes.
- Tag, not branch: upstream has no `v9.5` branch;
  `v9.5.0` is the release tag. Pin upgrades as
  projects (research §8).

### 1.2 Port: `user/uiapp/lv_port.c`, `lv_port.h`

- Display: one byte buffer of exactly one 240x24
  RGB565 strip (11,520 B, today's strip size).
  `flush_cb` forwards each invalidated rectangle to
  `SYS_DISPLAY_FLUSH` (61), which already takes
  arbitrary rectangles; `lv_display_flush_ready()`
  follows. Render mode `PARTIAL`, color format
  RGB565. NOTE: `lv_color_t` in LVGL v9 is a 3-byte
  RGB888 struct regardless of `LV_COLOR_DEPTH`, so
  the buffer is deliberately `uint8_t[]`, not
  `lv_color_t[]` (a `lv_color_t` array would oversize
  it by 5,760 B); `flush_cb` forwards `w*h*2`.
- Tick: `sys_uptime()` deltas fed to `lv_tick_inc()`
  each loop iteration.
- Input: keypad indev draining the EV queue.
  Mapping (see §3.1 for why): DOWN -> `LV_KEY_NEXT`,
  UP -> `LV_KEY_PREV`, LEFT/RIGHT -> arrows,
  OK -> ENTER, BACK -> ESC + run exit.
- A failed flush is counted and printed
  (`UILV: flush FAIL`), never fatal: the C6L-5 run
  saw one transient face-side `SYS_DISPLAY_FLUSH`
  failure, and UI-1 tracks reproduction on the uiapp
  path instead of hiding it behind a daemon exit.
  Result in testing: 0 failures in 3 runs (§5).
- Loop exits on BACK, token loss
  (`sys_display_status() != 1`), or 30 s without
  input (mirrors `UI_IDLE_RETURN_S`). Every
  iteration calls `sys_yield()` plus non-YIELD
  syscalls, so the FND-3 watchdog and the
  yield-or-park rule are satisfied by construction.

### 1.3 Smoke screen (marker-gated)

`user/uiapp/main.c` runs the LVGL screen instead of
the menu on a token grant only when
`/data/ui1-test` reads `"1"` (same marker pattern
as the C6L-5 test daemons: idempotent
`SYS_LFS_MOUNT` 94 + `SYS_LFS_READ` 96; the lfs
trap wrappers are file-local in `lv_port.c`
because ulib does not expose lfs — same as
`user/ulink5d.c`). Without the marker the menu
loop is byte-identical to before, so the battery
never sees the smoke screen.

Screen: title label `QAONIC LVGL` (Montserrat 14,
anti-aliased), button `PRESS ME`, button `BACK`,
status label `READY`. PRESS ME click sets the
status to `ACTIVATED`; BACK (button or physical
key) ends the run. Focus starts on PRESS ME;
default-theme focus outline is visible.

### 1.4 Config: `user/uiapp/lv_conf.h`

Derived from the upstream `lv_conf_template.h`:
color depth 16, builtin stdlib (malloc/string/
sprintf), OS none, software draw, log off, 64 KiB
mem pool, Montserrat 14 only, default theme only,
flex + grid on, all filesystems off, all widgets
off except button + label. Blend targets trimmed
to native RGB565 only (see §3.3).

### 1.5 Build wiring: `mach3-port/build-md.sh`

LVGL block next to the littlefs pattern: every
`.c` under `user/uiapp/lvgl/src` is compiled with
`-I user/uiapp -I user/uiapp/lvgl
-I user/uiapp/lvgl/src` (the first finds
`lv_conf.h` via `lv_conf_internal.h`'s
`__has_include` probe; the second resolves the
font file's umbrella-free includes; the third
resolves LVGL's own `core/...` style includes)
and linked into `uiapp.elf` with `lv_port.o` +
`lv_shim.o`. uiapp embed cap raised 98304 ->
786432 (guard only; the binding constraint is
the boot partition, §2).

### 1.6 Freestanding shims: `lv_shim.c`, `inttypes.h`

- `lv_shim.c`: `memset`/`memmove` +
  `__aeabi_memclr[48]` (compiler libcalls) +
  `__aeabi_memcpy4` + `__aeabi_idivmod` (quotient
  r0, remainder r1 via asm) + `__aeabi_uldivmod` /
  `__aeabi_ldivmod` (64-bit shift-subtract,
  returned as a 16-byte struct = q in r0-r1, r
  in r2-r3, the AAPCS convention the call sites
  expect). No compiler-rt archive exists in the
  portable toolchain, so these are hand-written
  and deliberately minimal.
- `user/uiapp/inttypes.h`: PRI* macros only.
  Clang's resource `inttypes.h` does `#include_next`
  into a libc header that does not exist
  freestanding; this file (found first via
  `-I user/uiapp`) supplies the macros LVGL uses.
- No float shims were needed: every float path in
  the vendored set is config-guarded off
  (`LV_USE_FLOAT 0`, `LV_USE_MATRIX 0`,
  `LV_DRAW_TRANSFORM_USE_MATRIX 0`,
  `LV_USE_OBSERVER 0`, `LV_USE_GESTURE_RECOGNITION
  0`). Verified by the clean link, not assumed.

## 2. Measurements (objcopy-binary method, C6L-3/4/5)

Kernel binary = `llvm-objcopy-18 -O binary` on the
linked `mach3.elf`, vs the 4,194,304 B boot
partition. Base = the C6L-5 merge built in this
worktree before any UI-1 change (saved as
`mach3-base.elf`; its binary is 64 B below the
DEVLOG-C6L5 figure — a build-path `__FILE__`
artifact of the worktree name, same method).

| Metric | Base (C6L-5) | UI-1 (naive) | UI-1 (trimmed) |
|---|---|---|---|
| Kernel binary | 3,629,788 | 4,108,420 | 3,994,188 |
| Headroom | 564,516 | 85,884 | **200,116** |
| uiapp.bin | 60,364 | 538,996 | 424,764 |
| uiapp delta | — | +478,632 | +364,400 |

`llvm-size-18` on `uiapp.elf` (trimmed):
`.text` 299,076, `.rodata` 18,948, `.data` 144,
`.bss` 106,576 (64 KiB LVGL pool + 11,520 B draw
buffer + pre-existing uiapp state).
LVGL objects alone (trimmed): `.text` 267,140,
`.data`+`.rodata` 15,640, `.bss` 66,045,
total 348,825. Port + shim: `.text` 2,336.

Largest LVGL objects (trimmed): mem pool `.bss`
65,536; `lv_obj_pos` 15,220; `lv_obj_style`
12,284; `lv_indev` 10,348; `lv_display` 9,648;
`lv_draw_sw_transform` 9,328; label 8,908;
Montserrat 14 font `.rodata` 13,649.

### Go / no-go (the UI-1 exit criterion)

**GO for UI-1 and UI-2.** UI-1 fits with 200,116 B
of boot-partition headroom left. The naive build
(85,884 B left) would have been a no-go for any
further growth, which is why the blend trim (§3.3)
is part of UI-1, not polish.

**CONDITIONAL for UI-3..UI-6.** Each further
widget family costs roughly 5–15 KB of `.text`
plus theme code, and further fonts/screens add
more; the trajectory crosses zero somewhere in
UI-4/UI-5 on current trends. The structural
decision (strip test programs, §4, and/or load
daemon images from littlefs, and/or grow the
partition) must be taken before the phase that
would cross zero — re-measured at every phase,
not now. UI-1 does NOT grow linkd (61,564 B,
93.9% of its cap — untouched) and does NOT strip
anything.

### Runtime behaviour (QEMU-indicative only)

- Tick: correct by construction (uptime deltas);
  no drift observed across the verify runs.
- Input latency: key event to focus/activation
  marker well under the 20 s script timeouts
  (sub-second in practice; not formally timed).
- Flush: invalidation-scoped, proven by the
  verify run (dump1->dump2 full-frame diff ==
  status-band diff == 505 px; nothing else
  repainted).
- SPI pacing: UNVERIFIABLE in QEMU, stated
  explicitly. No display-timing claim is made.

## 3. Findings (each fixed in this phase)

### 3.1 Keypad focus moves ONLY on NEXT/PREV

First manual run: `key right` produced no focus
change, while activation worked. Root cause is
LVGL design, not a port bug: `lv_indev.c`'s
keypad handler moves group focus only on
`LV_KEY_NEXT` / `LV_KEY_PREV`; arrow keys are
delivered to the focused object as data. The port
maps DOWN -> NEXT and UP -> PREV; LEFT/RIGHT stay
arrows for widgets that consume them. UI-2's menu
navigation must use this mapping (DOWN = next
tile, UP = previous tile), not arrows.

### 3.2 LVGL v9 `lv_color_t` is 3 bytes, always

`lv_color_t` is an RGB888 struct regardless of
`LV_COLOR_DEPTH`; depth only selects the display
buffer conversion. A `lv_color_t` draw buffer
would oversize by 5,760 B. The port uses a byte
buffer and forwards `w*h*2` for RGB565.

### 3.3 Blend variants for unused formats: -108 KB

The software renderer compiles one blend backend
per color format, all defaulting to 1. Disabling
every format except native RGB565 in `lv_conf.h`
saved 108,472 B of `.text` (375,612 -> 267,140)
and the link demanded none of them back. If a
later screen needs an alpha layer format, the
link will say so; re-enable exactly that one.

### 3.4 `lv_init` unconditionally needs bin_decoder

`lv_bin_decoder_init()` is called by `lv_init()`
with no config guard, so `libs/bin_decoder` +
`libs/rle` are vendored as `.c` (42 KB source),
not header-only.

### 3.5 Vendor patches (2, both documented here)

1. `src/font/lv_font_montserrat_14.c`: the stock
   file includes the root `lvgl.h` umbrella, which
   parses every widget/lib/driver header (and
   `animimage` errors when `LV_USE_IMAGE` is 0).
   Patched to include `lv_font.h` +
   `fmt_txt/lv_font_fmt_txt.h` + `lv_version.h` +
   a local `LV_VERSION_CHECK` (both version macros
   used to come from the umbrella).
2. `lv_conf.h`: `LV_USE_ANIMIMG` set to 0 (the
   template defaults it to 1; it aliases
   `LV_USE_ANIMIMAGE` and is not vendored).

## 4. Test-program total + strip proposal

Measure only, per the brief — nothing stripped
in UI-1.

Embedded `.bin` sizes (padded, i.e. what lands
in the kernel image), base build:

| Program | Bytes |
|---|---|
| ulinktest | 54,148 |
| ulinkbr | 55,172 |
| ulink3 | 51,404 |
| ulinkaud | 56,716 |
| ulink5 | 22,832 |
| ulink5d | 18,688 |
| ulinktools | 11,552 |
| ulink5die | 8,964 |
| uipcaud | 18,360 |
| uipcld | 12,092 |
| uipkpark | 9,752 |
| uipknd | 9,268 |
| uipcregexit | 4,962 |
| uipcregfull | 5,549 |
| uiptoolssrv | 11,424 |
| **ulink/uipc test family** | **350,883** |

Second tier, also test-only by purpose (not in
the family above): `utlscli` 753,844 +
`utlskeep` 757,604 = 1,511,448 B (TLS keep-alive
test clients from the F1 era, each carrying its
own mbedTLS copy). Usage audit (UI-1, grep):
no current verify script references either
program, and no spawn hook maps them (only the
daemon-table externs, marked SEMENTARA, plus
historical F1/Q2b docs). They are dead weight
in the image — strippable with no verify-script
impact, pending only the coordinator's decision
and a rebuild + battery to confirm.

Proposal (coordinator decision, with battery
evidence in hand): strip the 350,883 B test
family from production images first (it alone
restores headroom to ~551 KB after UI-1, covering
the whole UI-2..UI-6 trajectory on current
trends); keep the programs buildable and runnable
from a QA image, or load them from littlefs.
The utlscli/utlskeep pair is the bigger prize
but needs a usage audit first — do not strip
blind. The structural fix the research names
(daemon images from littlefs instead of embedded)
subsumes both and benefits every daemon, but it
is kernel work in its own right; it is NOT
started here.

## 5. Verification

### UI1-VERIFY (new, `scripts/verify-ui1.py`)

- Pre-change (C6L-5 base kernel): **FAIL 17/24**
  (rc=1). All 7 failures are LVGL-specific
  (UILV start/focus/activation/exit + status-band
  change + scoping). NOTE, stated honestly: the
  first version's title/button/status band checks
  PASSED on base too, because the dark hand-rolled
  menu also has dark text and blue tiles in those
  bands. The discriminating check added after that
  observation is the full-frame near-white count:
  LVGL default-theme background = 41,460 white
  pixels; base menu = 0. With that check the
  script is 25 checks; the white check fails on
  base by direct measurement of the base dump
  (the pre-change run was not re-run for the one
  added check).
- Post-change: **PASS 25/25** (rc=0).
  dump1: title dark 221, btn1 blue 7,000, btn2
  blue 6,891, status dark 124, white 41,460.
  dump2: status dark 189, status-band diff 505,
  full-frame diff 505 (== status band: scoped).
  Focus: down -> back, up -> press, ok ->
  activated, back -> exit. 0 flush failures,
  0 FAULT, 0 uiapp FAIL. Marker cleared,
  disk left unprovisioned (`setup reset`),
  no QEMU left.
- Captures: `logs-ui1/ui1-dump1.ppm`,
  `ui1-dump2.ppm` (240x240; dump2 visually
  confirmed: AA title, blue buttons with focus
  outline on PRESS ME, ACTIVATED status).
- The C6L-5 face-side `SYS_DISPLAY_FLUSH`
  failure did NOT reproduce on the uiapp LVGL
  path (0 occurrences in 3 runs: 2 manual
  drives + the verify run).

### Battery (standalone, worktree kernel)

Battery order: firstboot, shell, fnd1..fnd5,
ipc2, ipc4, ipc5, c6l1..c6l5, onb1, onb2,
safemode, update, ui1. All PASS after
standalone re-runs (below); no code changed
for any battery result.

| Script | Battery | Standalone |
|---|---|---|
| firstboot | PASS | — |
| shell | FAIL (`ls` missing `/sd:`) | PASS |
| fnd1 | PASS | — |
| fnd2 | PASS (21/21) | — |
| fnd3 | PASS (46/46) | — |
| fnd4 | PASS (36/36) | — |
| fnd5 | PASS (33/33) | — |
| ipc2 | PASS (34/34) | — |
| ipc4 | PASS (58/58) | — |
| ipc5 | PASS (20/20) | — |
| c6l1 | PASS (29/29) | — |
| c6l2 | PASS (36/36) | — |
| c6l3 | PASS (72/72) | — |
| c6l4 | startup crash (missing `logs-c6l4/` dir) | PASS (60/60) |
| c6l5 | FAIL (54/55) | PASS (55/55) |
| onb1 | PASS | — |
| onb2 | FAIL verdict (finale eyes + finale log line) | PASS |
| safemode | PASS | — |
| update | PASS | — |
| ui1 | PASS (25/25) | — |

Failure analysis (all four are the documented
flake classes, none a UI-1 regression — UI-1
touches no shell/fat, linkd, IPC, or face code):

- shell: one-shot SD/fat timing; the battery
  run missed `/sd:` in a plain `ls`. Standalone
  PASS. Same class as the C6L-5 SD/fat one-shot
  note.
- c6l4: harness precondition, not a test:
  `verify-c6l4.py` crashes listing `logs-c6l4/`,
  which does not exist in a fresh worktree (it
  existed in the C6L-4 worktree). After
  `mkdir logs-c6l4`, standalone PASS 60/60.
  The directory is untracked scratch, left in
  the worktree for future runs.
- c6l5: one check, `ulink5d ok markers continue
  after client death`, in battery order only.
  Standalone PASS 55/55. Same serial-under-load
  sensitivity class as the C6L-5 caveat.
- onb2: the battery run's own verdict line was
  FAIL (2 finale checks) although the script
  exits 0 — its exit code does not reflect its
  verdict, so the battery runner's rc-based
  "PASS" for onb2 was wrong and the verdict
  line is the truth. Standalone PASS. Same
  flake class as C6L-5's "ONB2 failed twice
  then PASSED on retry". The exit-code defect
  is a harness note for a future cleanup pass,
  not fixed here (out of UI-1 scope).

Disk left healthy/unprovisioned after the
battery (ui1 teardown does `setup reset`);
no QEMU or mock-c6 peer left running.

## 6. Limitations (honest)

- UI-2..UI-6 are NOT built here: no menu/status
  bar/monitor/power/settings on LVGL, no icon
  assets, no Wi-Fi password UX decision, no
  theme-to-mock tuning, no `ui_draw` cleanup.
- The smoke screen is a test surface, not a
  product screen; it is invisible without the
  marker.
- `lv_conf.h` enables only label + button. Every
  further widget is a deliberate config + vendor
  diff (its `.c` must be added to the vendor
  tree; headers alone do not suffice).
- LVGL timers/animations were exercised only at
  smoke-screen depth (focus transitions). Timer
  behaviour under token loss mid-animation is a
  UI-2 test (research risk 5).
- QEMU timing is not RV1103 timing (research
  risk 3); no CPU-cost claim is made.
- The 9.0 MB vendored source tree grows the repo;
  that is the research's prescribed layout
  (`user/uiapp/lvgl/`), not an accident.
- `uiapp_lv_port.o` `.bss` includes the 11,520 B
  draw buffer; the 64 KiB LVGL pool is sized for
  UI-2 growth, not measured against UI-1's two
  widgets (pool sizing is a UI-2 measurement).
