# 03 — Architecture & Build Brief

## Target

- **Board:** LCDwiki **3.5″ ESP32-32E Display** — SKU **E32R35T**, community name **ESP32-3248S035R** (a "CYD" / Cheap Yellow Display). Classic **ESP32-D0WD-V3** (ESP32-WROOM-32E module, dual-core LX6 @ 240 MHz, 520 KB SRAM, **no PSRAM**, 4 MB flash), 320×480 **TN** TFT, **ST7796U** display over 4-wire SPI, **XPT2046 resistive** touch over the **same SPI bus**, RGB status LED, on-board Li-battery charge circuit (no fuel gauge), BOOT + RESET side buttons, TF slot (unused), Type-C for power + flashing.
- **Orientation:** landscape, **480×320**. (TN panel — pick the landscape flip whose best viewing angle faces the installed direction; tune during bring-up.)
- **Power:** 5 V via Type-C (wall). Optional 3.7 V Li backup on the JST header (JP2), charged on-board (TP4054); no fuel gauge, but **battery voltage is sensed on GPIO34** (`BAT_ADC`, ÷2 divider) for a coarse charge indicator (see below).

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
| Battery voltage ADC (`BAT_ADC`) | 34 | VBAT ÷ 2 via 100K/100K divider; ADC1, input-only. **Wired — used for battery indicator** |
| Spare inputs | 35, 39 | input-only, broken out on headers, unused |

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
  board_build.partitions = min_spiffs.csv ; dual-app OTA layout (~1.9MB/slot); fits the LVGL app (~1.3MB) with headroom and enables wireless updates
  monitor_speed = 115200
  lib_deps =
    bodmer/TFT_eSPI
    lvgl/lvgl@^9
    bblanchon/ArduinoJson
    tzapu/WiFiManager
  build_flags =
    -D USER_SETUP_LOADED=1
    -D ST7796_DRIVER=1
    -D LOAD_GLCD=1 -D LOAD_FONT2=1 -D LOAD_FONT4=1 -D LOAD_GFXFF=1
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

> **Gotcha (verified on hardware):** with `USER_SETUP_LOADED=1`, TFT_eSPI compiles **only** the fonts you explicitly enable. Without `LOAD_GLCD`/`LOAD_FONT*`/`LOAD_GFXFF`, `drawString()`/`print()` silently render **nothing** (graphics primitives still work), which reads as a "blank text" display fault. The production UI is LVGL-rendered so it wouldn't catch this — but the diag firmware's labels and `calibrateTouch()` prompts need these flags. Keep them in the shared `[cyd_common]` build_flags so both envs get them.

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
tunables you expose (`run_time_default`, `sleep_s`).

**First run / no valid config / Wi-Fi fails to connect:**
1. Start SoftAP + **WiFiManager captive portal**. Show on the panel: "Setup — join Wi-Fi **OSPanel-Setup**, open the page" (so the tech-less first-time setup is obvious).
2. WiFiManager's config page collects the Wi-Fi network **plus custom parameters**: **OpenSprinkler host** and **OpenSprinkler device password** (WiFiManager supports custom fields).
3. Save to NVS, MD5 the password, reboot into normal (station) mode.

**Normal boot:** load NVS → connect Wi-Fi → `GET /jn` → run.

**Wi-Fi modem-sleep is disabled** (`WiFi.setSleep(false)` right after `WiFi.mode(WIFI_STA)`). Arduino-ESP32 enables `WIFI_PS_MIN_MODEM` (DTIM-beacon modem-sleep) by default — the same knob as ESPHome's `power_save_mode` — which causes latency spikes, jitter, and dropped frames on some APs (notably UniFi). This is a wall-powered panel where an always-on, responsive link matters far more than the sub-milliamp idle saving, so we keep the radio awake. (If a future battery mode wants it back, gate it behind an explicit opt-in that is off by default and validated against the target AP.)

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
- **Idle + untouched ≥ sleep timeout → backlight off** (sleep overlay). Any touch wakes and is consumed.
- **Never sleep while a station is running.**
- **Sleep timeout is configurable + NVS-persisted** (`sleep_s`, seconds, default **300**,
  clamp 0–3600, **`0` = never sleep**). Set it in the WiFiManager config portal
  ("Screen sleep timeout" field, reachable via the cold-boot AP portal or the 3 s
  BOOT-hold STA edit portal). `PanelState::set_sleep_timeout_ms()` applies it at boot.
- **Root-cause fix (#60):** the idle-sleep timer never expired because
  `PanelState::enter_idle()` re-stamped the "last interaction" time on **every
  ~2 s `/jc` poll** while already idle (`on_jc()` re-affirms idle each poll), so
  the elapsed-idle clock was wiped continuously and never reached the timeout.
  `enter_idle()` now only resets the idle clock on a genuine transition **into**
  idle (e.g. Running→Idle); re-affirming idle on a poll leaves it running. Raw
  touch is **not** debounced (single taps stay instant); a rising-edge `[TOUCH]`
  dev-log trace is emitted once per press for bench correlation.
- **Heartbeat observability:** when `dev_log` is enabled the 1 s `[HB]` line also
  reports `sleeping`, `idle_ms` (time since last confirmed touch), and `sleep_to_ms`
  (active timeout), and a `[TOUCH]` trace prints once per confirmed press — so the
  bench can watch the idle timer climb, confirm phantom presses are gone, and verify
  the sleep transition without a DMM.

---

## Battery & power

The board runs on **5 V via Type-C** (wall-powered) and has an on-board Li-battery
charge circuit with an optional 3.7 V backup cell on the JST header (JP2). There is
**no PMIC / fuel gauge** (unlike the AXP2101 on the S3 board), so charge %, current,
and AC-present are **not** exposed over I²C — but the raw battery **voltage is wired
to an ADC** (see below), so a coarse charge estimate is available.

Verified against the **E32R35T schematic** (authoritative for the 3248S035R/C base
PCB — the 035R and 035C share this exact power circuit, so battery support is
universal across both):

- **Battery level detection:** `BAT+ → R2 (100K) → BAT_ADC → R3 (100K) → GND`, with
  C1/C2 (0.1 µF) filter caps. That's an even **100K/100K divider (÷2)**, so
  `VBAT = ADC_mV × 2`. The node is **`BAT_ADC` on GPIO34** (ADC1_CH6, input-only,
  Wi-Fi-safe). At 11 dB attenuation the full LiPo range (4.2 V → 2.10 V, 3.0 V →
  1.50 V at the pin) sits in the ADC's linear region.
- **Charge/discharge:** a **TP4054** single-cell linear charger (R27 = 3.3 K PROG ≈
  300 mA) plus a **Q3 (SL2305) P-FET + R28** load-share/reverse-blocking path, so the
  battery is isolated while USB 5 V is present. The TP4054 `CHRG` status pin is **not
  routed to a GPIO**, so there is **no hardware charge-status flag** — firmware sees
  voltage only (charging can only be inferred heuristically from a steady ~4.2 V).
- Battery is **backup, not the primary mode** — don't gate any control behaviour on
  it. The battery indicator is display-only.

**Battery monitoring firmware** lives as a pure `lib/battery_monitor` (counts/mV →
VBAT → LiPo % with smoothing, native-unit-tested) plus thin ADC glue in `src/`, so
the same code serves both board variants. The exact divider ratio and ADC-Vref
calibration are confirmed empirically on the bench via the diagnostic firmware's
`a` (ADC/battery probe) command before the ratio is trusted.

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
9. Sleeps after the configurable idle timeout (default 5 min; backlight off via GPIO27 PWM); stays lit while running.
10. Runs against the real controller with 14 stations, and correctly shows a 24-station (extender) layout.
11. Top bar shows PANEL + CTRL Wi-Fi bars (CTRL → `— —` on signal loss); no battery indicator.
12. Resistive touch is calibrated once and the calibration persists in NVS across reboots.
