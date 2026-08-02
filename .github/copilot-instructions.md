# Copilot instructions — opensprinkler-panel

ESP32 firmware for a 3.5" resistive CYD touch panel that runs and
steps OpenSprinkler stations over the controller's local HTTP API. Built with
PlatformIO (stock `espressif32`) + Arduino + TFT_eSPI + LVGL.

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
  with `pio run -e cyd-35r`.
- On-panel UI changes must refresh the affected manual/README screenshots in
  the same PR. Follow `docs/08-docs-site.md` section "Screenshots".

## OpenSprinkler API contract

Follow `docs/02-opensprinkler-api.md` exactly. Most important quirk: `en=1` on
an already-running station is a NO-OP, so advance/prev/jump/extend must do
off-then-on (two `/cm` calls). Stop = `/cv?rsn=1`. Auth `pw` = MD5 hex of the
device password. `result == 1` means success.

## Hardware notes

Classic ESP32 (ESP32-WROOM-32E), **no PSRAM**, 4MB flash. Display and XPT2046
resistive touch share ONE SPI bus — keep the touch clock low (~2.5MHz) while the
display runs fast. LVGL draw buffers must be small/partial (internal RAM only).
See `docs/03-architecture.md` for the authoritative pin map.

## Pull request labels

Release notes are auto-generated from merged PRs and grouped by label via
`.github/release.yml`. Always apply one primary label to a PR so it is
categorized correctly: `enhancement` (features), `bug` (fixes), `documentation`,
`hardware`, `infra` (build/CI/tooling), or `dependencies`. Unlabeled PRs fall
under "Other changes".
