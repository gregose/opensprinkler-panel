# Copilot instructions — opensprinkler-panel

ESP32 firmware for a wall-mounted 3.5" CYD touch panel that runs and steps
OpenSprinkler stations over the controller's local HTTP API. Built with
PlatformIO (stock `espressif32`) + Arduino + TFT_eSPI + LVGL.

Two supported boards — identical MCU and display, different touch controller:
- **ESP32-3248S035R** (`cyd-35r`): XPT2046 resistive touch, shared SPI bus.
- **ESP32-3248S035C** (`cyd-35c`): GT911 capacitive touch, I2C (SDA33/SCL32/INT21/RST25). Touch path gated by `-D TOUCH_GT911=1`.

## Dependency management (READ THIS BEFORE ADDING ANY LIBRARY)

Whenever you add or change a dependency in `platformio.ini` (`lib_deps`,
`platform`, `platform_packages`, or `test_framework`), you MUST **also** add it
to the pre-warm step in `.github/workflows/copilot-setup-steps.yml`:

- Add the library to the `Pre-warm milestone libraries (global)` step as
  `pio pkg install -g --library "<owner>/<name>@<version>"`, pinned to the same
  version range you put in `platformio.ini`.
- The coding agent's runtime firewall may block `registry.platformio.org`
  during the work phase. `copilot-setup-steps.yml` runs BEFORE that firewall
  with full network, so pre-warming there guarantees the library is resolvable
  offline. This is not optional — skipping it is how a past PR ended up
  vendoring a framework into the repo by mistake.

Do **not** vendor third-party libraries into `lib/` to work around network
issues. Resolve them through PlatformIO and pre-warm them as above.

## Toolchain consistency

`platformio.ini` is the single source of truth for the ESP32 platform version.
Keep the pinned tool versions identical across `platformio.ini`,
`.github/workflows/ci.yml`, and `.github/workflows/copilot-setup-steps.yml`
(currently Python 3.11, PlatformIO 6.1.19, esptool 5.3.1).

## Architecture & testing

- Put hardware-independent logic (domain models, API client, parsing, state
  machines) in `lib/*` as pure C++ (no Arduino, no network) with injected
  transports, so it is covered by native Unity tests. See `lib/station_model`
  as the reference pattern.
- Keep Arduino/hardware glue thin, in `src/`.
- Every logic change must keep `pio test -e native` green. Firmware must build
  with `pio run -e cyd-35r` AND `pio run -e cyd-35c`.

## OpenSprinkler API contract

Follow `docs/02-opensprinkler-api.md` exactly. Most important quirk: `en=1` on
an already-running station is a NO-OP, so advance/prev/jump/extend must do
off-then-on (two `/cm` calls). Stop = `/cv?rsn=1`. Auth `pw` = MD5 hex of the
device password. `result == 1` means success.

## Hardware notes

Classic ESP32 (ESP32-WROOM-32E), **no PSRAM**, 4MB flash. Display is ST7796U
SPI (identical on both boards). LVGL draw buffers must be small/partial
(internal RAM only). See `docs/03-architecture.md` for the authoritative pin maps.

- **035R:** XPT2046 resistive touch shares the SPI bus with the display — keep
  the touch clock low (~2.5MHz, `SPI_TOUCH_FREQUENCY`). Touch calibration blob
  persists in NVS (`touch_cal` key).
- **035C:** GT911 capacitive I2C touch (SDA33/SCL32/INT21/RST25). The INT pin is
  commonly tied to GND — poll via `read()`, do NOT rely on hardware interrupts.
  Calibration-free; no NVS touch calibration blob needed.
