# Host LVGL simulator (`env:sim`)

Renders the **real firmware screen code** (`lib/ui`) on your host to 480x320
PNGs, using the **same `lv_conf.h` + Montserrat fonts** as the ESP32 firmware.
This eliminates HTML->LVGL drift: what you see here is what LVGL draws on the
panel (modulo RGB565 vs the HTML bench, and per-frame state). The firmware and
this sim link the **same** `lib/ui` units, so there is no divergence.

This is a **host build only** (`platform = native`, same class as
`pio test -e native`). It never cross-compiles for the ESP32 and never flashes.

## Scope

Every LVGL screen/state, driven from portable view-model fixtures
(`sim/fixtures.{h,cpp}`): the idle prompt (connected / loading / syncing /
reconnecting / offline / auth error), the unified run screen (manual and
program, each running and paused), the programs-list overlay (first + paged),
and the sleep overlay. The top bar renders as part of every state.

## Prerequisites

1. **Pinned host toolchain** — run `./tools/setup.sh` to create a repo-local
   `.venv` with the project-pinned PlatformIO (`6.1.19`), matching CI. Then
   either `source .venv/bin/activate` (so `pio ...` below works) or invoke it as
   `./.venv/bin/pio ...`.
2. **SDL2** (via `sdl2-config`):
   - **macOS:** `brew install sdl2`
   - **Ubuntu/Debian:** `sudo apt-get install -y libsdl2-dev`

LVGL and ArduinoJson are pulled by PlatformIO (pinned in `lib_deps`) and are
pre-warmed in `.github/workflows/copilot-setup-steps.yml`. Nothing is vendored
into `lib/`. `sim/lodepng.*` is a host-only, public-domain PNG codec used only
by this tool (it is deliberately not under `lib/`).

## Build and run

```sh
# Build the host sim
pio run -e sim

# Render EVERY state headlessly (no display needed) -> sim/out/<state>.png
SDL_VIDEODRIVER=dummy ./.pio/build/sim/program

# Render a single state
./.pio/build/sim/program --state run-program

# Optional: open an interactive SDL preview window for one state
./.pio/build/sim/program --window --state programs-list
```

Flags:

- `--state <name>` render a single named state (see `all_states()` in
  `sim/fixtures.cpp`); omit to render every state.
- `--out <path>` override the output PNG (single-state only).
- `--outdir <dir>` override the output directory (default `sim/out`).
- `--window` open an SDL preview window for one state.

## Output

- `sim/out/<state>.png` - the rendered 480x320 frame for each state.
- `sim/out/<state>-diff.png` - per-channel abs-diff vs the committed bench
  screenshot, for states that have one (git-ignored).
- A fidelity report on stdout (mean abs error + % pixels within tolerance)
  for each state with a bench reference.
