# 03 — Architecture & Build Brief

## Target

- **Board:** LCDwiki **3.5″ ESP32-32E Display** — SKU **E32R35T**, community name **ESP32-3248S035R** (a "CYD" / Cheap Yellow Display). Classic **ESP32-D0WD-V3** (ESP32-WROOM-32E module, dual-core LX6 @ 240 MHz, 520 KB SRAM, **no PSRAM**, 4 MB flash), 320×480 **TN** TFT, **ST7796U** display over 4-wire SPI, **XPT2046 resistive** touch over the **same SPI bus**, RGB status LED, on-board Li-battery charge circuit (no fuel gauge), BOOT + RESET side buttons, TF slot (unused), Type-C for power + flashing.
- **Orientation:** landscape, **480×320**. (TN panel — pick the landscape flip whose best viewing angle faces the installed direction; tune during bring-up.)
- **Power:** 5 V via Type-C (wall). Optional 3.7 V Li backup on the JST header, charged on-board; **no battery monitoring** (see below).

### Pin map (from the LCDwiki E32R35T wiki — ground truth; verify on the physical board)

| Function | ESP32 GPIO | Notes |
|---|---|---|
| LCD CS | 15 | ST7796U chip select (active low) |
| LCD DC | 2 | data/command |
| LCD SCLK | 14 | **SPI clock — shared LCD + touch** |
| LCD MOSI | 13 | **SPI MOSI — shared LCD + touch** |
| LCD MISO | 12 | **SPI MISO — shared LCD + touch** |
| LCD RST | — | tied to ESP32 EN (no dedicated GPIO; `TFT_RST = -1`) |
| LCD backlight | 27 | high = on (drive via LEDC PWM for dimming/sleep) |
| Touch CS | 33 | XPT2046 chip select |
| Touch IRQ | 36 | XPT2046 pen interrupt (input-only pin) |
| RGB LED R / G / B | 22 / 16 / 17 | common anode, **low = on** |
| SD CS / MOSI / SCLK / MISO | 5 / 23 / 18 / 19 | separate bus, unused |
| Audio enable / DAC | 4 / 26 | unused |
| BOOT button | 0 | re-provision trigger (hold on boot) |
| Battery voltage ADC | 34 | available but **unused** (no battery indicator) |
| Spare inputs | 35, 39 | input-only, unused |

> **The LCD and the XPT2046 touch share one SPI bus** (SCLK 14 / MOSI 13 / MISO 12) with separate chip-selects (LCD 15, touch 33). TFT_eSPI drives both on the same bus — keep the touch SPI clock low (~2.5 MHz) even though the display runs fast.

---

## Framework decision — PlatformIO + stock `espressif32` (Arduino)

**Use PlatformIO's official `platformio/espressif32` platform**, Arduino framework, with **TFT_eSPI** + **LVGL 9.x**.

Why:
- This is a **classic ESP32** (not an ESP32-S3), so the frozen-below-core-3.x limitation that forces the `pioarduino` fork on S3 boards **does not apply**. The official `platformio/espressif32` platform builds this board cleanly. **Do not use pioarduino here** — it adds nothing for a classic ESP32 and diverges from the huge body of CYD examples.
- **TFT_eSPI** is the canonical, best-documented driver stack for these CYD boards: it drives the **ST7796U** display *and* the **XPT2046** resistive touch (with built-in calibration helpers) on the shared SPI bus, and integrates trivially with LVGL.

  ```ini
  ; platformio.ini
  [env:cyd-35r]
  platform = espressif32            ; pin a version once building (e.g. espressif32@6.x)
  framework = arduino
  board = esp32dev                  ; ESP32-WROOM-32E, 4MB flash, no PSRAM
  board_build.f_flash = 40000000L
  board_build.flash_mode = dio
  board_build.partitions = default.csv   ; dual-app OTA layout (ota_0/ota_1 + nvs), enables wireless updates
  monitor_speed = 115200
  lib_deps =
    bodmer/TFT_eSPI
    lvgl/lvgl@^9
    bblanchon/ArduinoJson
    tzapu/WiFiManager
  build_flags =
    -D USER_SETUP_LOADED=1
    -D ST7796_DRIVER=1
    -D TFT_WIDTH=320
    -D TFT_HEIGHT=480
    -D TFT_MISO=12 -D TFT_MOSI=13 -D TFT_SCLK=14
    -D TFT_CS=15 -D TFT_DC=2 -D TFT_RST=-1 -D TFT_BL=27
    -D TOUCH_CS=33
    -D SPI_FREQUENCY=40000000
    -D SPI_TOUCH_FREQUENCY=2500000
    -D LV_CONF_PATH=... (or lv_conf.h in include/)
  ```
  (Provide TFT_eSPI's setup either via these `build_flags` or a `User_Setup.h`; pin the platform/library versions once the build is green — they move.)

- **No PSRAM.** LVGL draw buffers must live in **internal DRAM** and be **small/partial** (e.g. a 480×40 line buffer, ~38 KB × 2), not a full framebuffer (480×320×2 ≈ 300 KB won't fit). This is the standard CYD approach; the single-task model below still applies.

**Alternative:** `Arduino_GFX` (ST7796) + a standalone `XPT2046_Touchscreen` library is workable, but TFT_eSPI bundles display+touch and matches the CYD ecosystem — prefer it to de-risk bring-up.

### Library stack (PlatformIO `lib_deps`)
- **Display + touch:** **TFT_eSPI** (bodmer) — ST7796U display driver *and* XPT2046 resistive-touch reader (with `touch_calibrate` / `setTouch`). One library covers both on the shared SPI bus.
- **GUI:** **LVGL 9.x**. Provide `lv_conf.h`; draw buffers in **internal RAM** (small partial buffers — no PSRAM). Enable a monospace font for the digits (or convert JetBrains Mono; a built-in LVGL mono is an acceptable fallback).
- **Provisioning:** **WiFiManager** (tzapu).
- **OTA + wireless logs:** **ArduinoOTA** (bundled in arduino-esp32, no extra `lib_deps`) as the OTA responder; a tiny built-in `WiFiServer` TCP log sink. Both are framework built-ins — **M4.5 adds no new PlatformIO dependencies**.
- **HTTP:** built-in `HTTPClient` (short timeouts, see `02`).
- **JSON:** `ArduinoJson`.
- **Config storage:** `Preferences` (NVS).

### Known bring-up gotchas (flag, don't be surprised by)
- ST7796U on these panels often needs specific **rotation + column/row offset** and sometimes **color inversion** (`tft.invertDisplay(true)` is common on CYDs); expect to tune rotation/mirror/offset/invert against the physical panel.
- **Resistive touch (XPT2046) needs calibration** — a raw-ADC→pixel mapping per rotation. Run TFT_eSPI's calibration once, then **persist the 5 calibration values in NVS** and load them on boot (don't recalibrate every run). This replaces the capacitive axis-mapping step.
- **Shared SPI bus** for LCD + touch: keep `SPI_TOUCH_FREQUENCY` low (~2.5 MHz) while the display runs at ~40 MHz; TFT_eSPI switches per transaction.
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

## Wireless dev loop (OTA + network logs)

Mirrors the ESPHome iterate-without-USB loop. **Why it works:** OTA rewrites
only the **app partition** — the bootloader, partition table, and **NVS**
(Wi-Fi creds + `os_host` + `os_pw_md5`) are left intact, so connectivity and
config survive every app iteration. Contrast the USB `flash.sh` path, which
writes `merged-firmware.bin` at `0x0` (**full flash → wipes NVS**); use USB only
for bootstrap/recovery, then OTA for iteration.

- **OTA responder:** `ArduinoOTA` (framework built-in) with a password from NVS
  (`ota_pass`) + a stable mDNS hostname (e.g. `ospanel.local`). Guard behind a
  `DEV_LOOP` build flag so release builds can omit it.
- **Log sink:** a small `WiFiServer` TCP log server (e.g. port 2323) that echoes
  what goes to `Serial` to any connected client — LAN-only, lightweight (no
  ESPAsyncWebServer/WebSerial, to protect the no-PSRAM RAM budget).
- **Local push (no compiler):** `tools/ota.sh` downloads the **app-only
  `firmware.bin`** from the CI artifact (not the merged bin) and pushes it with
  the standalone **`espota.py`** (Python only — mirrors how `flash.sh` shells out
  to `esptool`). CI should drop `espota.py` into the firmware artifact so the
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
3. Idle shows the prompt; tapping a station runs it. Running shows Station N eyebrow + name headline + amber countdown + Prev/Advance/Stop in one consistent row.
4. Advance/Prev wrap and skip disabled; grid jump works; Stop is immediate.
5. Run time 0:15–10:00/15 s; changing it while running restarts the current station (extend), via off-then-on.
6. Auto-advance off = stop at end; on = next station, stopping after the last.
7. Every action issues the correct `/cm`/`/cv` calls (off-then-on where required); the ~2 s `/jc` poll reconciles highlight + countdown from controller truth.
8. Wi-Fi loss → red top bar, commands halt, open station still auto-stops on the controller; reconnect self-heals from `/jc`.
9. Sleeps after 5 min idle (backlight off via GPIO27 PWM); stays lit while running.
10. Runs against the real controller with 14 stations, and correctly shows a 24-station (extender) layout.
11. Top bar shows PANEL + CTRL Wi-Fi bars (CTRL → `— —` on signal loss); no battery indicator.
12. Resistive touch is calibrated once and the calibration persists in NVS across reboots.
