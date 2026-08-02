# Host LVGL simulator (`env:sim`) - issue #123 spike

Renders the **real firmware top-bar code** (`lib/ui`) on your host to a
480x320 PNG, using the **same `lv_conf.h` + Montserrat fonts** as the ESP32
firmware. This eliminates HTML->LVGL drift: what you see here is what LVGL draws
on the panel (modulo RGB565 vs the HTML bench, and per-frame state).

This is a **host build only** (`platform = native`, same class as
`pio test -e native`). It never cross-compiles for the ESP32 and never flashes.

## Scope (this spike)

Top bar only: teal droplet, controller identity, P/C signal meters, battery
gauge and the 3px teal accent rule. It is the seed of a future `lib/ui` module;
the firmware and this sim link the **same** `lib/ui/top_bar.{h,cpp}`, so there
is no divergence.

## Prerequisites

The sim links SDL2 (via `sdl2-config`):

- **macOS:** `brew install sdl2`
- **Ubuntu/Debian:** `sudo apt-get install -y libsdl2-dev`

LVGL is pulled by PlatformIO (`lib_deps = lvgl/lvgl@^9.2`, pinned) and is
pre-warmed in `.github/workflows/copilot-setup-steps.yml`. Nothing is vendored
into `lib/`. `sim/lodepng.*` is a host-only, public-domain PNG codec used only
by this tool (it is deliberately not under `lib/`).

## Build and run

```sh
# Build the host sim
pio run -e sim

# Render headlessly (no display needed) -> sim/out/top-bar.png + a fidelity diff
SDL_VIDEODRIVER=dummy ./.pio/build/sim/program

# Optional: open an interactive SDL preview window
./.pio/build/sim/program --window
```

Flags:

- `--out <path>` override the output PNG (default `sim/out/top-bar.png`).
- `--ref <path>` override the diff reference (default
  `site/assets/img/screenshots/home-connected.png`).
- `--window` open an SDL preview window in addition to writing the PNG.

## Output

- `sim/out/top-bar.png` - the rendered 480x320 frame.
- `sim/out/top-bar-diff.png` - per-channel abs-diff of the top band vs the
  reference.
- A fidelity report on stdout (mean abs error + % pixels within tolerance).
