# 03 — Architecture & Build Brief

## Target boards

Both boards share the identical **ESP32-D0WD-V3** (ESP32-WROOM-32E) MCU, **ST7796U** SPI display, display pin map, USB-serial bridge (CP2102), and flash layout. The only difference is the touch controller.

| | **ESP32-3248S035R** (035R) | **ESP32-3248S035C** (035C) |
|---|---|---|
| Community name | CYD / resistive | CYD / capacitive |
| MCU | ESP32-WROOM-32E, 4 MB flash, no PSRAM | identical |
| Display | ST7796U, SPI, 320×480 (landscape 480×320) | identical |
| Touch | XPT2046 resistive, SPI, needs calibration | **GT911 capacitive, I2C, calibration-free** |
| Firmware env | `cyd-35r`, `cyd-35r-diag` | `cyd-35c`, `cyd-35c-diag` |

- **Orientation:** landscape, **480×320**. (TN panel — pick the landscape flip whose best viewing angle faces the installed direction; tune during bring-up.)
- **Power:** 5 V via Type-C or micro-USB (wall). Optional 3.7 V Li backup on the JST header, charged on-board; **no battery monitoring** (see below).

### Display pin map (identical on both boards)

| Function | ESP32 GPIO | Notes |
|---|---|---|
| LCD CS | 15 | ST7796U chip select (active low) |
| LCD DC | 2 | data/command |
| LCD SCLK | 14 | **SPI clock** |
| LCD MOSI | 13 | **SPI MOSI** |
| LCD MISO | 12 | **SPI MISO** |
| LCD RST | — | tied to ESP32 EN (no dedicated GPIO; `TFT_RST = -1`) |
| LCD backlight | 27 | high = on (drive via LEDC PWM for dimming/sleep) |
| RGB LED R / G / B | 22 / 16 / 17 | common anode, **low = on** |
| SD CS / MOSI / SCLK / MISO | 5 / 23 / 18 / 19 | separate bus, unused |
| Audio enable / DAC | 4 / 26 | unused |
| BOOT button | 0 | re-provision trigger (hold on boot) |
| Battery voltage ADC | 34 | available but **unused** (no battery indicator) |
| Spare inputs | 35, 39 | input-only, unused |

### Touch pin map — 035R (XPT2046 resistive, SPI)

| Function | ESP32 GPIO | Notes |
|---|---|---|
| Touch CS | 33 | XPT2046 chip select; shares SPI bus with LCD |
| Touch IRQ | 36 | XPT2046 pen interrupt (input-only pin, unused) |

> **The LCD and the XPT2046 touch share one SPI bus** (SCLK 14 / MOSI 13 / MISO 12) with separate chip-selects (LCD 15, touch 33). TFT_eSPI drives both on the same bus — keep the touch SPI clock low (~2.5 MHz, `SPI_TOUCH_FREQUENCY`) even though the display runs fast (40 MHz).

### Touch pin map — 035C (GT911 capacitive, I2C)

| Function | ESP32 GPIO | Notes |
|---|---|---|
| Touch SDA | 33 | GT911 I2C data |
| Touch SCL | 32 | GT911 I2C clock |
| Touch INT | 21 | GT911 interrupt — **commonly tied to GND on these boards; poll via `read()`, do NOT rely on the interrupt line** |
| Touch RST | 25 | GT911 reset |

GT911 I2C address: 0x5D (or 0x14, depending on INT state at power-on). GT911 is calibration-free; no NVS touch calibration blob is needed or written.

---

## Framework decision — PlatformIO + stock `espressif32` (Arduino)

**Use PlatformIO's official `platformio/espressif32` platform**, Arduino framework, with **TFT_eSPI** + **LVGL 9.x**.

Why:
- Both boards use a **classic ESP32** (not an ESP32-S3), so the frozen-below-core-3.x limitation that forces the `pioarduino` fork on S3 boards **does not apply**. The official `platformio/espressif32` platform builds both boards cleanly. **Do not use pioarduino here** — it adds nothing for a classic ESP32 and diverges from the huge body of CYD examples.
- **TFT_eSPI** is the canonical, best-documented driver stack for these CYD boards: it drives the **ST7796U** display on both boards and, on the 035R, the **XPT2046** resistive touch (with built-in calibration helpers) on the shared SPI bus.
- The 035C uses a separate **TAMC_GT911** library for the GT911 I2C touch controller. The display stack (TFT_eSPI + ST7796U) is identical on both boards; only the touch path changes, gated by the `-D TOUCH_GT911=1` build flag.

  ```ini
  ; platformio.ini — shared display config (cyd_common)
  [cyd_common]
  platform = espressif32@7.0.1
  framework = arduino
  board = esp32dev
  board_build.partitions = min_spiffs.csv
  build_flags =
    -D USER_SETUP_LOADED=1
    -D ST7796_DRIVER=1
    -D LOAD_GLCD=1 -D LOAD_FONT2=1 -D LOAD_FONT4=1 -D LOAD_GFXFF=1
    -D TFT_WIDTH=320 -D TFT_HEIGHT=480
    -D TFT_MISO=12 -D TFT_MOSI=13 -D TFT_SCLK=14
    -D TFT_CS=15 -D TFT_DC=2 -D TFT_RST=-1 -D TFT_BL=27
    -D SPI_FREQUENCY=40000000

  ; 035R: XPT2046 resistive touch on the shared SPI bus
  [env:cyd-35r]
  extends = cyd_common
  build_flags = ${cyd_common.build_flags} -D TOUCH_CS=33 -D SPI_TOUCH_FREQUENCY=2500000

  ; 035C: GT911 capacitive touch over I2C
  [env:cyd-35c]
  extends = cyd_common
  lib_deps = ... tamctec/TAMC_GT911@^1.0.2
  build_flags = ${cyd_common.build_flags}
    -D TOUCH_GT911=1 -D GT911_SDA=33 -D GT911_SCL=32 -D GT911_INT=21 -D GT911_RST=25
  ```

> **Gotcha (verified on hardware):** with `USER_SETUP_LOADED=1`, TFT_eSPI compiles **only** the fonts you explicitly enable. Without `LOAD_GLCD`/`LOAD_FONT*`/`LOAD_GFXFF`, `drawString()`/`print()` silently render **nothing** (graphics primitives still work), which reads as a "blank text" display fault. The production UI is LVGL-rendered so it wouldn't catch this — but the diag firmware's labels and `calibrateTouch()` prompts need these flags. Keep them in the shared `[cyd_common]` build_flags so both envs get them.

- **No PSRAM.** LVGL draw buffers must live in **internal DRAM** and be **small/partial** (e.g. a 480×40 line buffer, ~38 KB × 2), not a full framebuffer (480×320×2 ≈ 300 KB won't fit). This is the standard CYD approach; the single-task model below still applies.

### Library stack (PlatformIO `lib_deps`)
- **Display:** **TFT_eSPI** (bodmer) — ST7796U display driver. On the 035R, also drives the XPT2046 resistive touch (with `touch_calibrate` / `setTouch`) on the shared SPI bus.
- **Touch (035C only):** **TAMC_GT911** (tamctec) — GT911 I2C capacitive touch driver. Polled in `touchpad_read_cb` (INT line is commonly tied to GND on these boards — do not rely on hardware interrupts).
- **GUI:** **LVGL 9.x**. Provide `lv_conf.h`; draw buffers in **internal RAM** (small partial buffers — no PSRAM). Enable a monospace font for the digits (or convert JetBrains Mono; a built-in LVGL mono is an acceptable fallback).
- **Provisioning:** **WiFiManager** (tzapu).
- **OTA + wireless logs:** **ArduinoOTA** (bundled in arduino-esp32, no extra `lib_deps`) as the OTA responder; a tiny built-in `WiFiServer` TCP log sink. Both are framework built-ins — **M4.5 adds no new PlatformIO dependencies**.
- **HTTP:** built-in `HTTPClient` (short timeouts, see `02`).
- **JSON:** `ArduinoJson`.
- **Config storage:** `Preferences` (NVS).

### Known bring-up gotchas (flag, don't be surprised by)
- ST7796U on these panels often needs specific **rotation + column/row offset** and sometimes **color inversion** (`tft.invertDisplay(true)` is common on CYDs); expect to tune rotation/mirror/offset/invert against the physical panel.
- **Resistive touch (XPT2046, 035R only) needs calibration** — a raw-ADC→pixel mapping per rotation. Run TFT_eSPI's calibration once, then **persist the 5 calibration values in NVS** and load them on boot (don't recalibrate every run).
- **035R shared SPI bus** for LCD + touch: keep `SPI_TOUCH_FREQUENCY` low (~2.5 MHz) while the display runs at ~40 MHz; TFT_eSPI switches per transaction.
- **GT911 capacitive touch (035C):** calibration-free. The INT pin is commonly tied to GND on production boards — poll via `read()` in `touchpad_read_cb`, do NOT rely on the interrupt. I2C address is 0x5D (or 0x14 if INT is pulled high at power-on).
- **No PSRAM** → keep LVGL draw buffers small and in internal DRAM; watch heap.
- TN panel → viewing angle is directional; choose the landscape flip accordingly.
- Backlight on GPIO27 → drive with LEDC PWM so sleep can dim/blank it.

---

## Configuration & provisioning (NVS + captive portal)

Config to persist in NVS (`Preferences`): `wifi_ssid`, `wifi_pass`,
`os_host` (IP or mDNS name), `os_pw_md5` (MD5 of device password),
`ota_pass` (ArduinoOTA/log-stream password), and any
tunables you expose (`run_time_default`, `sleep_minutes`).

**First run / no valid config / Wi-Fi fails to connect:**
1. Start SoftAP + **WiFiManager captive portal**. Show on the panel: "Setup — join Wi-Fi **OSPanel-Setup**, open the page" (so the tech-less first-time setup is obvious).
2. WiFiManager's config page collects the Wi-Fi network **plus custom parameters**: **OpenSprinkler host** and **OpenSprinkler device password** (WiFiManager supports custom fields).
3. Save to NVS, MD5 the password, reboot into normal (station) mode.

**Normal boot:** load NVS → connect Wi-Fi → `GET /jn` → run.

**Re-provision path:** hold the **BOOT button (GPIO0)** for ~3 s to clear
NVS/creds and re-enter the portal (also fall into the portal automatically if
Wi-Fi can't connect after a few tries). An on-screen "Settings → reconfigure" is
optional/nice-to-have.

---

## Wireless updates (OTA + network logs)

OTA is a first-class production feature, always compiled into `cyd-35r`. **Why
it works:** OTA rewrites only the **app partition** — the bootloader, partition
table, and **NVS** (Wi-Fi creds + `os_host` + `os_pw_md5` + `ota_pass` +
`dev_log`) are left intact, so connectivity and config survive every OTA.
Contrast the USB `flash.sh` path, which writes `merged-firmware.bin` at `0x0`
(**full flash → wipes NVS**); use USB only for bootstrap/recovery, then OTA for
iteration.

- **OTA responder:** `ArduinoOTA` (framework built-in) compiled unconditionally.
  `ArduinoOTA.begin()` is called at runtime **only when the NVS `ota_pass` key
  is non-empty** — a panel with no provisioned password never opens an
  unauthenticated update endpoint on the LAN. mDNS hostname: `ospanel.local`.
- **Log sink:** `TeeSerial` (a `Print` subclass) always redirects
  `Serial.xxx` calls in `main.cpp` through itself. A `WiFiServer` TCP log
  server on port 2323 is started **only when the NVS `dev_log` bool is true**
  (default false, toggled via the config portal). When `dev_log` is false,
  `TeeSerial` forwards to UART0 only — no open port, negligible overhead.
- **Local push (no compiler):** `tools/ota.sh` downloads the **app-only
  `firmware.bin`** from the CI artifact (not the merged bin) and pushes it with
  the standalone **`espota.py`** (Python only — mirrors how `flash.sh` shells out
  to `esptool`). CI bundles `espota.py` into the production artifact so the
  local bridge stays PlatformIO-free.
- **Local logs:** `tools/logs.sh` connects to the TCP log port, tees to
  `logs/serial.log`, and auto-reconnects across OTA reboots (same ergonomics as
  `monitor.sh`).

Adds **no new PlatformIO libraries** (ArduinoOTA + WiFiServer are framework
built-ins), so the copilot-instructions pre-warm rule is not triggered.

---

## Concurrency model — single task

Run **everything on the Arduino loop task**:
- `loop()` calls `lv_timer_handler()` frequently (~every 5 ms).
- A millis-based scheduler drives: the 1 s UI countdown tick, the ~2 s `/jc` poll, and the 5 min idle-sleep timer.
- User actions (LVGL event callbacks) issue their HTTP calls inline.

Because LVGL and all HTTP happen on the same task, **there is no thread-safety
problem** — no mutex, no `esp_lvgl_port` locking. The only cost is that a blocking
HTTP call briefly stalls the UI; with the short timeouts from `02` (~1–1.5 s
worst case on a dead controller, tens of ms normally) this is imperceptible in
practice.

**Upgrade path (only if UI stalls become noticeable):** move HTTP to a second
FreeRTOS task and marshal results back to the UI task via a queue, guarding any
LVGL calls with a mutex. Don't start here.

---

## Sleep / backlight
- Backlight on **GPIO27** (high = on). Drive it with **LEDC PWM** so sleep can blank/dim it.
- **Idle + untouched ≥ 5 min → backlight off** (sleep overlay). Any touch wakes and is consumed.
- **Never sleep while a station is running.**
- `sleep_minutes` configurable (default 5).

---

## Battery & power

The board runs on **5 V via Type-C** (wall-powered) and has an on-board Li-battery
charge circuit with an optional 3.7 V backup cell on the JST header. There is **no
PMIC / fuel gauge** (unlike the AXP2101 on the S3 board), so charge %, current, and
AC-present are **not** exposed over I²C.

- **No battery indicator.** Battery monitoring is intentionally **dropped** for this
  hardware — the panel is a wall-mounted, mains-powered unit and the backup cell is
  passive. The top bar shows only the two Wi-Fi signal readouts (below); there is no
  battery glyph. (Where the mockup shows a battery, omit it.)
- Battery voltage is physically available on the **GPIO34 ADC** if a future revision
  wants a coarse voltage readout, but it is **unused** in this build.
- Battery is backup, not the primary mode — don't gate any control behavior on it.

## Signal indicators

Two Wi-Fi RSSI readouts in the top bar (see `01`, `02`):
- **PANEL** — the ESP32's own link: `WiFi.RSSI()`, read locally each second.
- **CTRL** — the controller's link: the `RSSI` field already in the `/jc` poll (ESP8266/OS-v3). No extra request. Shows `— —` when the controller is unreachable while PANEL stays live — the key "which end is the problem" diagnostic.

## Definition of done

1. Boots from NVS config; first-run captive portal collects Wi-Fi + OS host + password; re-provision via BOOT hold.
2. Loads stations from `/jn`; grid shows only enabled, non-master stations; scales 1-row (≤12) / 2-row (13–24) without shrinking.
3. Idle shows the prompt; tapping a station runs it. Running shows Station N eyebrow + name headline + amber countdown + Advance/Stop in one consistent row.
4. Advance wraps and skips disabled; grid jump works; Stop is immediate.
5. Run time 0:15–10:00/15 s; the new value applies to the next run or advance; the currently running station is not restarted.
6. Auto-advance off = stop at end; on = next station, stopping after the last.
7. Every action issues the correct `/cm`/`/cv` calls (off-then-on where required); the ~2 s `/jc` poll reconciles highlight + countdown from controller truth.
8. Wi-Fi loss → red top bar, commands halt, open station still auto-stops on the controller; reconnect self-heals from `/jc`.
9. Sleeps after 5 min idle (backlight off via GPIO27 PWM); stays lit while running.
10. Runs against the real controller with 14 stations, and correctly shows a 24-station (extender) layout.
11. Top bar shows PANEL + CTRL Wi-Fi bars (CTRL → `— —` on signal loss); no battery indicator.
12. Touch is operational: on the 035R, resistive calibration is performed once and persists in NVS; on the 035C, GT911 capacitive touch works without calibration.
