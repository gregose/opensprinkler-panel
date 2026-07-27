# OpenSprinkler Station Control Panel — Build Handoff

A wall-mounted 3.5″ touch panel that lets a sprinkler tech run and step through
irrigation **stations** locally — for seasonal blow-out and spring testing —
without a phone, an app, or network credentials. It drives an OpenSprinkler v3
controller directly over its **local HTTP API**.

This folder is the complete package for a coding agent to build the firmware.

## Read in this order

| File | What it is |
|------|------------|
| `00-mockup-reference.html` | **The visual + behavioral source of truth.** An interactive, to-scale (480×320) simulation of the exact UI. Open it and tap through it. When the spec and the mockup disagree, ask — don't guess. |
| `01-ux-spec.md` | The UX contract: states, transitions, every control's behavior, copy strings, layout, tokens, edge cases. |
| `02-opensprinkler-api.md` | The controller API: endpoints, params, response shapes, and the **non-obvious firmware quirks** (verified against OpenSprinkler firmware source). This is the highest-risk-to-get-wrong document. |
| `03-architecture.md` | Target board, framework decision (**PlatformIO + stock espressif32, TFT_eSPI + LVGL**), library stack, WiFi/config provisioning, concurrency model, sleep, and Definition of Done. |
| `04-agent-kickoff.md` | Milestone order, per-step verification, and a **mock OpenSprinkler server** so you can build the whole thing without the physical controller. |
| `05-programs.md` | The **Programs feature (M9)** as shipped: the programs list, running a program, the live queue view, pause, program identification, the API contract used, and next-run computation. Authoritative for Programs behavior. |
| `06-hardware-validation-loop.md` | The **bench runbook**: how a coding agent validates firmware on the physical panel — the emulator-first + OTA-first loop, screenshot-driven objective measurement, USB/NVS fallback, and reporting. |
| `07-docs-site.md` | Maintaining the **public user manual & hosted web flasher** under `site/` (Just the Docs on GitHub Pages): structure, URLs/`relative_url`, adding pages, the flasher manifest, the build/deploy workflow, and how to (re)shoot the manual screenshots. Not published on the site. |
| `mock_os.py` | Runnable emulator of the controller's API — serves `/jn`, `/jo`, `/jc`, `/js`, `/jp` and accepts `/cm`, `/cv`, `/mp`, `/cp`, `/pq`. Models the full sequential run queue, programs (M9), and pause. `test_mock_os.py` is its contract/connection test suite (`python3 docs/test_mock_os.py`). |

## Decisions already locked (do not re-litigate)

- **Hardware:** LCDwiki **3.5″ ESP32-32E Display** (SKU **E32R35T**; community name **ESP32-3248S035R**, a "CYD"). Classic **ESP32-D0WD-V3** (ESP32-WROOM-32E, dual LX6, 520 KB SRAM, **no PSRAM**, 4 MB flash); 320×480 **TN** panel, **ST7796U** display over SPI; **XPT2046 resistive** touch over the **same SPI bus**. Mounted landscape → **480×320**. Wall-powered via 5 V / Type-C (optional Li-battery backup, no fuel gauge — no battery monitoring). See `03` for the full pin map.
- **Terminology:** "**Station**" everywhere (industry standard), 1-based in the UI, 0-based `sid` in the API.
- **UX:** single screen. Idle → pick a station. Running → step / jump / stop. One **Auto-advance** toggle is the only mode switch. Run time doubles as the per-station safety timeout. Full detail in `01`.
- **Framework:** PlatformIO with the **stock `espressif32`** platform + Arduino framework, **TFT_eSPI** (drives both the ST7796U display and XPT2046 touch) + **LVGL 9.x**. (No pioarduino: this is a classic ESP32, not an S3, so the official platform builds fine.) Rationale in `03`.
- **Config:** stored in **NVS**; entered on first run via a **WiFiManager captive portal** (WiFi + OpenSprinkler host + device password). Detail in `03`.

## Definition of done (summary — full list in `03`)

A tech can walk up to the panel, and with no other device: pick any enabled
station and run it, Advance (wrapping), jump via the grid, adjust the run time
(takes effect on the next run), toggle auto-advance to cycle the yard, and Stop — all
reflected against the controller's real state, recovering cleanly from Wi-Fi
loss, and sleeping when idle.
