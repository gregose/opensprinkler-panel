# 04 — Agent Kickoff

Build in this order. Each milestone has a concrete "done when". Don't wire the
full UX until the display, touch, LVGL, and the API client (against the mock)
all work in isolation — that's what keeps debugging tractable.

## Develop without the controller

`mock_os.py` (in this folder) is a runnable emulator of the OpenSprinkler API.

```
python3 mock_os.py          # serves 0.0.0.0:8080, 24 fake stations, 6 programs
python3 test_mock_os.py     # contract/connection tests for the emulator
```

Set the panel's `os_host` to your dev machine's `IP:8080` (the mock ignores
`pw` unless you pass `--require-pw`). It implements `/jn`, `/jo`, `/jc`, `/js`,
`/jp`, `/cm`, `/cv`, `/mp`, `/cp`, `/pq` and **faithfully models the
`en=1`-on-running-station no-op**, so your off-then-on logic gets exercised for
real. Programs (M9) run sequentially with a live queue; a manual `/mp` run
reports `pid 254` while a scheduled run (`--schedule` or the `/_run` debug
endpoint) reports the real 1-based program id — exactly like the controller.
Edit `DEFAULT_NAMES`/`default_programs()` at the top to test different station
counts, disabled stations, and program layouts. It's sequential and
time-accurate, so countdowns and auto-advance behave like the real thing.

## Milestones

**M0 — Toolchain.** PlatformIO project on the **stock `espressif32`** platform (see `03`),
Arduino framework, `board = esp32dev` (classic ESP32, **no PSRAM**); board flashes and prints over serial.
*Done when:* a blink/serial hello runs on the board.

**M1 — Display.** Bring up ST7796U via **TFT_eSPI** using the E32R35T (3248S035R)
pin map from `03`; set landscape 480×320. *Done when:* solid color fills and text
render correctly, right orientation, no offset/inversion issues (tune
rotation/offset/`invertDisplay` as needed).

**M2 — Touch.** XPT2046 **resistive** touch over the shared SPI bus (touch CS 33,
IRQ 36) via TFT_eSPI; **calibrate** raw ADC → pixel for the chosen rotation and
**persist the calibration in NVS**. *Done when:* on-screen crosshair follows your
finger accurately across the whole panel after calibration, and calibration
survives a reboot.

**M3 — LVGL.** Integrate LVGL 9, draw buffers in **internal RAM** (small partial
buffers — no PSRAM), bind the TFT_eSPI flush + XPT2046 read callbacks; a monospace
font available for digits. *Done when:* an LVGL button reacts to touch and a label
updates smoothly on a timer.

**M4 — Provisioning + NVS.** WiFiManager captive portal with custom fields for
**OS host** + **device password** (+ optional **OTA password**); persist Wi-Fi +
host + MD5(password) + `ota_pass` + tunables in NVS (`Preferences`); BOOT-hold
clears config and re-enters the portal. *Done when:* first boot with no config
opens the portal, saved config survives reboot, BOOT-hold re-provisions.

**M4.5 — Wireless dev loop (OTA + network logs).** Switch to the `min_spiffs.csv`
dual-OTA partition table; add an `ArduinoOTA` responder (password from NVS, mDNS
hostname `ospanel.local`) and a lightweight `WiFiServer` TCP log sink that mirrors
`Serial`, both behind a `DEV_LOOP` build flag. Add `tools/ota.sh` (download the
app-only `firmware.bin` from the CI artifact, push with standalone `espota.py`)
and `tools/logs.sh` (stream the TCP log port to `logs/serial.log`, auto-reconnect
across reboots); CI includes `espota.py` in the artifact. See `03` §Wireless dev
loop. **Adds no new PlatformIO deps** (framework built-ins). *Done when:* after a
one-time USB flash, a subsequent `tools/ota.sh` pushes new firmware over Wi-Fi
**without wiping NVS config**, and `tools/logs.sh` shows boot + runtime logs with
no USB attached.

**M5 — API client (against the mock).** Implement the `02` client: `/jn` load,
`/jc` poll, `/cm` run/off (with off-then-on helpers), `/cv` stop; short timeouts;
JSON via ArduinoJson; a connection-state flag. *Done when:* against `mock_os.py`
you can log the parsed station list, start/stop a station, advance (off-then-on),
and read back the running station + `rem` from `/jc`.

**M6 — Wire the UX / state machine.** Build the single screen per `01`
(idle/running visibility, name-headline + eyebrow + countdown, one consistent
Advance/Stop row, run-time stepper, Auto-advance toggle, scaling grid),
driven by the M5 client. Implement: select=run, Advance wrap + skip
disabled, jump, run-time-change-applies-to-next-run (editing run time while running does not restart the current station), auto-advance
(stop-after-last), the `/jc` reconcile, signal-loss state, and 5-min idle sleep.
*Done when:* the on-device behavior matches `00-mockup-reference.html` tapped
side by side, using the mock.

**M7 — Real controller.** Point `os_host` at the actual OpenSprinkler. *Done
when:* runs against the real 14-station system; every action behaves as on the
mock; pull the plug on the controller mid-run and confirm the panel shows signal
loss and recovers when it's back (and the station auto-stopped on its own).

**M8 — Polish.** Verify the 24-station (extender) layout; tune backlight/sleep;
confirm disabled + master stations are filtered; final display calibration.
*Done when:* the Definition of Done in `03` is fully met.

## Verify against the DoD

Walk the 10-point Definition of Done in `03` §"Definition of done" as the
acceptance checklist. The mockup is the tie-breaker for any UI ambiguity; this
package's `01`/`02` are the tie-breakers for behavior and API.

## Things that will bite if ignored (all detailed in the specs)
- **No PSRAM** on this classic ESP32 → LVGL draw buffers must be small/partial and in internal DRAM; a full framebuffer won't fit (`03`).
- LCD **and** touch share one SPI bus → keep the touch SPI clock low (~2.5 MHz) while the display runs fast (`03`).
- `en=1` on a running station is a **no-op** → advance/extend must **off-then-on** (`02`).
- Short HTTP timeouts, or a dead controller freezes the UI (`02`, `03`).
- Single-task model → no LVGL locking needed; don't add threads first (`03`).
- ST7796U rotation/offset/inversion needs tuning; **resistive touch must be calibrated and the calibration persisted in NVS** (`03`).
- Filter **disabled and master** stations from the grid (`01`, `02`).
- **USB `flash.sh` writes the merged image at `0x0` → it wipes NVS** (Wi-Fi + OS
  config) every time; that's bootstrap/recovery only. Iterate with **OTA**
  (`tools/ota.sh`), which rewrites only the app partition and preserves NVS
  (`03` §Wireless dev loop). OTA requires the **`min_spiffs.csv` dual-OTA partition
  table** (M4.5).
