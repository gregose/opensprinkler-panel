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
docs/mock_os.py       OpenSprinkler controller emulator for developing without hardware
docs/test_mock_os.py  Contract/connection tests for the emulator (run in CI)
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

## Flashing without a local toolchain

Cloud CI builds the firmware on every push and pull request, then uploads a
`cyd-35r-firmware` artifact with this layout:

```text
merged-firmware.bin
bootloader.bin
partitions.bin
boot_app0.bin
firmware.bin
flash/index.html
flash/manifest.json
```

- **Primary path, local flash/debug:** install only `esptool` + `pyserial`, then use
  [`tools/flash.sh`](tools/flash.sh) to download a branch or PR artifact and write
  `merged-firmware.bin` at `0x0`. Use [`tools/monitor.sh`](tools/monitor.sh) for a
  115200 serial monitor. Full workflow in [`tools/README.md`](tools/README.md).
- **Browser flashing:** after extracting the artifact, open `flash/index.html` in
  Chrome or Edge and flash over WebSerial with ESP Web Tools.
- **Fallback:** `python3 -m esptool --chip esp32 --port <port> write_flash 0x0 merged-firmware.bin`
  flashes the merged image directly.

### Develop without the controller

```bash
python3 docs/mock_os.py            # serves the OpenSprinkler API on :8080 (14 fake stations, 3 programs)
python3 docs/mock_os.py --schedule # also fire programs at their scheduled start times
python3 docs/test_mock_os.py       # run the emulator's contract/connection tests
```

Point the panel's configured host at `your-machine-ip:8080`. The emulator serves
every endpoint the firmware uses — `/jn`, `/jo`, `/jc`, `/js`, `/jp` and
`/cm`, `/cv`, `/mp`, `/cp`, `/pq` — and models the sequential run queue,
programs, pause, and the `en=1`-on-running-station no-op. See the header of
`docs/mock_os.py` for CLI flags and the debug endpoints (`/_run`, `/_reset`,
`/_state`).

## Continuous integration

`.github/workflows/ci.yml` runs the native unit tests, compiles the firmware, and
publishes the flashable firmware artifact on every push/PR.
`.github/workflows/copilot-setup-steps.yml` pre-installs the **same** Python +
PlatformIO + esptool toolchain for Copilot cloud sessions, so CI and cloud
development stay in lockstep.

## Roadmap

Milestones **M0–M8** are tracked as GitHub issues/milestones; see
[`docs/04-agent-kickoff.md`](docs/04-agent-kickoff.md) for the build order and
per-milestone "done when" criteria.
