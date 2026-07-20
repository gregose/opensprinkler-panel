# 01 — UX & Behavior Spec

**The mockup (`00-mockup-reference.html`) is the visual source of truth.** This
document is the authoritative text for behavior. Build to both; where they
conflict, raise it rather than guessing.

Screen is **480×320 landscape**. All pixel values below are in real panel
pixels (the mockup is 1:1). The panel's corners are physically rounded — keep
tappable content and text off the extreme corners.

---

## 1. Screens & states

There is **one screen** with two states: **Idle** and **Running**. Plus a
**Sleep** overlay and a first-run **Setup** flow (Setup is covered in `03`).

### Top bar (always, 26 px tall)
- **Left — controller reachability:** `◉ <host>` in teal when the API is reachable; turns **red** `◎ <host>` when not.
- **Right — two indicators:**
  - **PANEL** signal — the panel's own Wi-Fi RSSI (`WiFi.RSSI()`, read locally), as 4 bars.
  - **CTRL** signal — the controller's Wi-Fi RSSI, from the `RSSI` field in the same `/jc` poll (`02`), as 4 bars. When the controller is unreachable, show `— —`; PANEL stays valid, which is the diagnostic ("panel's fine, controller's the problem").
- **No battery indicator.** This board has no fuel gauge and is wall-powered, so battery monitoring is dropped (see `03`). Where the mockup shows a battery glyph, omit it.
- Bar mapping (both signals), RSSI→bars: ≥ −55 = 4, −65…−55 = 3 (teal); −72…−65 = 2 (amber); −82…−72 = 1 (red); < −82 = 0.
- No station count here — the grid conveys that.

### Idle state
- **Left panel:** a prompt, not a station. Heading **"Select a station"**, sub-line **"▾ Tap a station below to start"** (chevron teal). No station number, no countdown, no Advance/Stop nav.
- **Right panel (settings, 190 px wide):** Run time stepper + Auto-advance toggle. No Stop.
- **Grid:** all station pills, **none highlighted**. Label reads **"Stations"**.
- Tapping any station pill → starts that station → Running.

### Running state
- **Left panel:**
  - Small eyebrow: **"Station N"** (mono, teal), where N is 1-based.
  - **Station name, large** — this is the headline (e.g. **North Beds**). Ellipsize if it would overflow one line.
  - **Countdown**, large, amber (e.g. `2:14`) — the single time element. **No** "Running" label, **no** "left" label, **no** progress bar.
  - Action row pinned to the bottom: **Advance ›** (teal) and **■ Stop** (red).
- **Right panel:** Run time stepper + Auto-advance toggle.
- **Grid:** the active station pill is highlighted teal. Label reads **"Jump to station"**.

**Action buttons — one consistent left-nav row.** Advance and Stop are the same
height and share one baseline at the panel bottom. Stop is distinguished by
**color (red)**, not by size or position.

### Sleep overlay
- After **5 minutes idle and untouched**, blank the screen (backlight off) and show a minimal "screen off" state. Any touch wakes it and is consumed (does not also trigger the control under the finger).
- **Never sleep while a station is running** — screen stays lit the whole time.

---

## 2. Controls & behavior

Let `RT` = the current run-time setting in seconds. Stations are 1-based in the
UI (`n`), 0-based in the API (`sid = n-1`).

| Control | Where | Behavior |
|---|---|---|
| **Station pill** | grid | Runs that station for `RT`. From idle → Running. While running → jump to it (turn current off, this one on). |
| **Advance ›** | running | Move to the next station and run it for `RT`. **Wraps**: after the last station, goes back to station 1. Skips disabled stations. |
| **Run time − / +** | always | Adjust `RT` (0:15–10:00, 15 s steps, default 1:00). The new value applies to the **next** run or advance; the currently running station is not restarted. |
| **Auto-advance** (toggle) | always | **Off (default):** when `RT` elapses, the station stops → Idle. **On:** when `RT` elapses, automatically run the **next** station. A full auto pass **stops after the last station** (no loop). Helper text: On → "Runs the next station"; Off → "Stops when time ends". |
| **■ Stop** | running | Immediately stop everything → Idle. No confirmation. |

**Manual Advance wraps; auto-advance stops after the last station.** This split
is intentional: manual stepping is free navigation (wrap enables a second pass);
an automatic pass is a bounded test that must not water forever.

**Idle vs Running visibility is strict.** Advance/Stop and the station
number/name/countdown exist **only** while running. Idle shows only the prompt,
the settings, and the (unhighlighted) grid. Do not show a "selected but not
running" station — selecting a station *is* running it.

---

## 3. Station grid

- Built at runtime from the controller's station list (`/jn`), never hardcoded — the station **count, names, and enabled set all come from the controller**; nothing about zones is compiled in.
- **When it loads:** once at startup (and after the first-run config flow), then cached for the session — the grid and running-state navigation are built from this list, never hardcoded. Station config only changes when the controller is reconfigured (an extender add requires power-cycling the OS), so a fresh launch always reflects current config; no runtime re-fetch needed.
- **Disabled stations are omitted** (from `stn_dis`, see `02`). Master/pump stations should also be omitted (they can't be run individually and the controller rejects them).
- Layout scales without shrinking the pills:
  - ≤ 12 stations → **one row**.
  - 13–24 → **two balanced rows** (e.g. 14 → 2×7, 24 → 2×12).
  - (Design rule: `rows = ceil(N/12)` capped so 13–24 is 2 rows; `cols = ceil(N/rows)`.)
- Pills show the 1-based station number. Active pill highlighted teal while running.

---

## 4. Copy (exact strings)

- Idle heading: `Select a station`
- Idle sub: `Tap a station below to start` (with a leading teal `▾`)
- Grid label: `Stations` (idle) / `Jump to station` (running)
- Eyebrow: `Station N`
- Run time label: `Run time`
- Auto-advance label: `Auto-advance`; helper `Runs the next station` / `Stops when time ends`
- Buttons: `Advance ›`, `■ Stop`
- Connected: `◉ <host>` (teal) · Disconnected: `◎ <host>` (red). Signal bars labelled `PANEL` / `CTRL`; CTRL shows `— —` when unreachable.
- Transient toasts (~3 s): `Stopped.` · `Station N finished.` · `Finished all stations.`

Voice: plain, active, no apologies. A control names what it does; the same word
carries through (Stop → "Stopped.").

---

## 5. Visual tokens

Colors: screen `#07100f`; text `#e9f2ef`; muted `#7f938f`; **teal (active/selected)
`#35d0c3`**; **amber (running / countdown) `#f2a63b`**; **red (stop) `#ff5b5b`**;
hairline `rgba(233,247,243,.10)`.

Type: a **monospace** face for telemetry (station number eyebrow, countdown) so
digits don't jitter; a clean sans for labels/buttons. (Mockup uses JetBrains
Mono + Space Grotesk; on-device, convert a mono for the digits or use a built-in
LVGL mono font — see `03`.)

Targets: Advance/Stop and the stepper buttons are large; grid pills are the
fast secondary jump. Everything finger-sized at both 14 and 24 stations.

---

## 6. Edge cases

- **Signal loss:** on any failed/timed-out request, enter the red top-bar state and stop issuing commands. The open station still auto-stops on the controller at its `RT`. When a poll succeeds again, clear the state and re-sync from `/jc` (see `02`).
- **Reconcile to reality:** the controller is the source of truth. The poll updates which station is highlighted and the countdown from the controller's own status — so if a station stops on the controller (cap reached, or someone used the app), the panel reflects it.
- **Missing/disabled stations:** never appear in the grid; Advance and jump skip them.
- **Empty list / can't reach controller at boot:** show a clear "can't reach controller" state (not a blank grid), with the configured host, and keep retrying.
