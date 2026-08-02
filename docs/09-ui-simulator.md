# UI simulator: local UX iteration loop

The **host LVGL simulator** (`env:sim`, under `sim/`) renders the **real
firmware screen code** (`lib/ui`) on your laptop to 480x320 PNGs, using the
**same `include/lv_conf.h` + Montserrat fonts + RGB565 color depth** as the
ESP32 firmware. The firmware and the sim link the **same** `lib/ui` units, so
what the sim draws is what LVGL draws on the panel (there is no HTML->LVGL
translation, unlike `docs/00-mockup-reference.html`). This makes the sim the
fastest, most faithful way to iterate on on-glass UX without a bench panel.

It is a **host build only** (`platform = native`, same class as
`pio test -e native`); it never cross-compiles for the ESP32 and never flashes.

## When to use the sim

Use it whenever you touch **anything that draws on the panel**:

- Iterating on layout, spacing, colors, copy, or a control's rendered state.
- Adding or changing a screen/state (idle, run, programs, sleep, error states).
- Reviewing a UX change with Greg: render the PNGs and share them in the PR.
- Regression-checking a refactor is pixel-identical (diff vs bench screenshots).

**Do NOT** use it for things it cannot model:

- The **boot/diagnostic screens** (`draw_boot_*`) are TFT_eSPI primitives drawn
  before LVGL starts, and the **captive portal** is WiFiManager HTML. Neither is
  LVGL, so neither lives in `lib/ui` or renders in the sim. They stay in
  `src/main.cpp`; validate them on hardware (`docs/06-hardware-validation-loop.md`).
- Real controller behavior, timing, touch calibration, SPI/backlight, Wi-Fi
  recovery. The sim renders a **static frame** from a view model; it does not run
  the poll loop or the network stack. Use `docs/mock_os.py` + the firmware for
  behavior, and the bench for hardware.

## Prerequisites (one-time setup)

1. **Install the pinned host toolchain** — run `./tools/setup.sh`. This creates
   a repo-local `.venv` with the project-pinned PlatformIO (`6.1.19`), so the
   sim uses the exact same PlatformIO as CI. Either `source .venv/bin/activate`
   first (then `pio ...` works), or invoke it directly as `./.venv/bin/pio`.
2. **Install SDL2** — needed only for the optional `--window` preview; headless
   rendering does not need a display:
   - macOS: `brew install sdl2`
   - Ubuntu/Debian: `sudo apt-get install -y libsdl2-dev`

LVGL and ArduinoJson are resolved by PlatformIO (pinned in `platformio.ini`
`lib_deps`) and pre-warmed in `.github/workflows/copilot-setup-steps.yml`.
Nothing is vendored into `lib/`. `sim/lodepng.*` is a host-only, public-domain
PNG codec used only by this tool.

## The fast iteration loop

```sh
# 1. Edit the screen code in lib/ui/ (e.g. panel_screen.cpp, ui_layout.h).

# 2. Build the host sim (seconds, no ESP32 toolchain needed).
pio run -e sim

# 3a. Render EVERY state -> sim/out/<state>.png
SDL_VIDEODRIVER=dummy ./.pio/build/sim/program

# 3b. ...or just the one you're working on (faster feedback):
./.pio/build/sim/program --state run-program

# 4. Open the PNG (sim/out/run-program.png) and eyeball it. Repeat.

# Optional: live SDL preview window for one state
./.pio/build/sim/program --window --state programs-list
```

`program` flags: `--state <name>` (single state; omit for all), `--out <path>`
(single-state output override), `--outdir <dir>` (default `sim/out`),
`--window` (SDL preview of one state).

## Driving states with accurate mocks (fixtures)

Each screen/state is produced from **portable view-model inputs** in
`sim/fixtures.cpp` - the same structs the firmware feeds into
`osp::ui::update_panel_screen()`:

- `PanelView` (phase, link state, countdown, paused, run time, etc.)
- `StationModel` (named stations + which are runnable)
- `JpData` (programs, for the list + program-run header)
- `osp::ui::HostStatus` (panel Wi-Fi bars, battery %, controller host string)

To iterate on a specific scenario, tweak or add a fixture:

1. In `sim/fixtures.cpp`, edit the branch in `make_fixture()` for your state
   (or add a new `else if (state == "my-state")` branch and set the view-model
   fields you need). Keep the demo station/program helpers realistic so labels
   read like a real yard.
2. Add the new state name to `all_states()` so the render-all driver and CI
   pick it up.
3. If it should be fidelity-checked, set `f.ref` to a committed bench screenshot
   basename under `site/assets/img/screenshots/` (leave empty for render-only).
4. Rebuild + render. The new `sim/out/<state>.png` appears.

This is the accurate way to mock a UX state: you are exercising the real
`lib/ui` render path, not a hand-drawn mockup.

## Fidelity check vs committed bench screenshots

For states with a `ref`, the sim full-frame-diffs the render against the
committed bench screenshot and prints `MAE /255` + `% pixels within 8/16/32`
(same tolerance bands as the #124 spike). A `sim/out/<state>-diff.png` is
written (git-ignored). Because the bench references were shot against a real
controller (different station names, host/IP, battery %), expect text-content
deltas; the structural guarantee is that the sim and firmware link identical
`lib/ui` code. Treat a **new** large delta after a refactor as a regression to
investigate.

## Adding a new screen the right way

Keep the split that makes the sim possible:

- Put **all screen geometry** in `lib/ui` (LVGL only - no Arduino, no
  TFT_eSPI, no network). Builders create widgets and take a `Callbacks` struct
  of generic `lv_event_cb_t`; per-frame updates read a pure `PanelView` +
  models + `HostStatus`. See `lib/ui/panel_screen.cpp` and the TopBar pattern.
- Keep `src/main.cpp` as **thin glue**: wire the LVGL drivers + poll loop to the
  `lib/ui` builders, attach the `ev_*` handlers via `Callbacks`, assemble
  `HostStatus`, and own hardware side-effects (backlight). No geometry in
  `main.cpp`.
- Add a fixture state (above) and render it. On-panel UI changes must refresh
  the affected `sim/out/` PNGs (and any manual/README screenshots per
  `docs/08-docs-site.md`) in the same PR.

## CI

The `sim-render` job in `.github/workflows/ci.yml` builds `env:sim`, renders the
full state set headlessly (`SDL_VIDEODRIVER=dummy`), and uploads them as the
`sim-screens-<sha>` artifact, so every PR carries a visual record of the screens.

## Related tools (don't confuse them)

- `docs/00-mockup-reference.html` - the original interactive HTML mockup; the
  design intent. The sim supersedes it as the source of truth for *what the
  firmware actually renders*.
- `docs/mock_os.py` - an emulator of the **controller HTTP API**. It drives the
  real firmware/model over the network (behavior, run queue, programs, pause).
  The sim instead drives `lib/ui` directly from view-model fixtures (rendering).
  Use `mock_os.py` for behavior, the sim for pixels.
- `docs/06-hardware-validation-loop.md` - the bench runbook for the physical
  panel. Real-hardware re-shoots of screenshots are a separate, gated step.
