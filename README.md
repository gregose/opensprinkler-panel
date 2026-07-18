# OpenSprinkler Station Control Panel

Firmware for a wall-mounted 3.5″ touch panel that runs and steps through
OpenSprinkler irrigation **stations** locally — for seasonal blow-out and spring
testing — with no phone, app, or network credentials. It drives an OpenSprinkler
v3 controller directly over its local HTTP API.

## Hardware

LCDwiki **3.5″ ESP32-32E Display** (SKU **E32R35T**, community name
**ESP32-3248S035R**, a "CYD"):

- Classic **ESP32-D0WD-V3** (ESP32-WROOM-32E) — dual-core LX6, 520 KB SRAM,
  **no PSRAM**, 4 MB flash
- 320×480 **TN** panel, **ST7796U** over SPI → mounted landscape (**480×320**)
- **XPT2046 resistive** touch on the **same SPI bus** as the display
- Wall-powered via 5 V / Type-C (optional passive Li backup; no battery gauge)

Full pin map and rationale in [`docs/03-architecture.md`](docs/03-architecture.md).

## Software stack

PlatformIO · stock **`espressif32@7.0.1`** platform · Arduino framework ·
**TFT_eSPI** (ST7796U display + XPT2046 touch) · **LVGL 9** · WiFiManager ·
ArduinoJson · Preferences (NVS).

## Project layout

```
docs/                 Design docs — the source of truth (read docs/README.md first)
src/                  Firmware entry point (hardware glue: display/touch/LVGL/WiFi)
lib/station_model/    Hardware-independent domain logic (grid, navigation, bitmasks)
test/                 Native (host) unit tests — run in CI, no board needed
docs/mock_os.py       Mock OpenSprinkler controller for developing without hardware
platformio.ini        Build config: `cyd-35r` (firmware) + `native` (tests)
```

## Building & testing

> Builds and tests run in **CI and cloud sessions**, not on a local machine.
> The ESP32 toolchain is pinned in `platformio.ini`, so every environment builds
> identically.

```bash
pio test -e native      # hardware-independent unit tests
pio run  -e cyd-35r     # compile the firmware
pio run  -e cyd-35r -t upload && pio device monitor   # flash + serial (local, with board)
```

### Develop without the controller

```bash
python3 docs/mock_os.py   # serves the OpenSprinkler API on :8080 (14 fake stations)
```

Point the panel's configured host at `your-machine-ip:8080`.

## Continuous integration

`.github/workflows/ci.yml` runs the native unit tests and compiles the firmware
on every push/PR. `.github/workflows/copilot-setup-steps.yml` pre-installs the
**same** Python + PlatformIO + ESP32 toolchain for Copilot cloud sessions, so CI
and cloud development stay in lockstep.

## Roadmap

Milestones **M0–M8** are tracked as GitHub issues/milestones; see
[`docs/04-agent-kickoff.md`](docs/04-agent-kickoff.md) for the build order and
per-milestone "done when" criteria.
