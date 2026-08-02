# 01 — UX & Behavior Spec

**The mockup (`00-mockup-reference.html`) is the visual source of truth.** This
document is the authoritative text for behavior. Build to both; where they
conflict, raise it rather than guessing.

Screen is **480×320 landscape**. All pixel values below are in real panel
pixels (the mockup is 1:1). The panel's corners are physically rounded — keep
tappable content and text off the extreme corners.

---

## 1. Screens & states

There is **one main screen** with two states: **Idle** and **Running**. Plus a
**Sleep** overlay and a first-run **Setup** flow (Setup is covered in `03`). A
separate **Programs** feature (list + program-run screens) is documented in
`05-programs.md`; it is reached from the settings panel and is out of scope here.

### Top bar (always, 26 px tall)
- **Left identity:** three independent labels show a water droplet, controller identity, and optional colored status text. Identity prefers `/jo.dname` and falls back to the configured host/IP. The droplet represents controller reachability: teal when reachable, amber while syncing, and red when the controller is unavailable or rejects authentication.
- **Status precedence:** Syncing first, then connection/auth issues, then `DISABLED`, then `RAIN DELAY <duration>` only while operation is enabled, otherwise empty. Disabled suppresses rain delay. Status text is amber for syncing/reconnecting, red for auth/offline/disabled, and teal for rain delay.
- **Motion:** the droplet pulses from full to 35% opacity over a 1.2 s ping-pong cycle only while syncing or reconnecting. Idle, watering, offline, and program-running states keep it fully static.
- **Right fixed slots, left to right:** controller current in mA when `/jc.curr` is present, a 2 x 16 px teal-dim divider, `P` panel RSSI, `C` controller RSSI, and the ADC-backed battery glyph plus percent. A present zero current remains visible as dim `0 mA`; an absent field hides the full current slot. Fixed widths keep changing digits from shifting neighboring metrics.
- **Calm numerics:** live current uses normal text, idle zero uses muted text, and a healthy battery percent uses `CLR_LEDE`. Low/critical battery percent and glyph use their amber/red tier color.
- **State accent:** a 3 px rule spans the bottom of the bar. It is teal-dim for normal/rain, amber for syncing/reconnecting, and red for disabled/offline/auth.
- Bar mapping (both signals), RSSI→bars: ≥ −55 = 4, −65…−55 = 3 (teal); −72…−65 = 2 (amber); −82…−72 = 1 (red); < −82 = 0. The controller meter clears while syncing or disconnected, while the panel meter stays live.
- Long controller identities ellipsize before the status label; the right cluster remains pinned. No station count appears here because the grid already conveys it.

### Idle state
- **Left panel:** a prompt, not a station. Heading **"Select a station"**, sub-line **"Tap a station below to start"** (teal). No station number, no countdown, no Advance/Stop nav.
- **Right panel (settings, 190 px wide):** Run time stepper + Auto-advance toggle + a **`≡ Programs`** entry button (leading `LV_SYMBOL_LIST`, no chevron; styled + sized like the stepper — opens the Programs list — see `05`). No Stop.
- **Grid:** all station pills, **none highlighted**. Label reads **"STATIONS"**.
- Tapping any station pill → starts that station → Running.

### Running state
- **Left panel:**
  - Small eyebrow: **"STATION N"** (mono caps, teal), where N is 1-based.
  - **Station name, large** — this is the headline (e.g. **North Beds**). Ellipsize if it would overflow one line.
  - **Countdown**, large, amber (e.g. `2:14`) — the single time element. **No** "Running" label, **no** "left" label, **no** progress bar.
  - Action row pinned to the bottom: **Next ›** (advance, teal), **Pause** (toggles to **Resume**, grey) and **■ Stop** (red).
- **Right panel:** Run time stepper + Auto-advance toggle + `≡ Programs` entry button (see `05`).
- **Grid:** the active station pill is highlighted teal. Label reads **"STATIONS"** (kept consistent with idle).

**Shared status/nav layout.** The manual-run and program-run screens use one
identical left-column status stack (eyebrow, station name, big countdown, and the
paused block) and one identical **Next / Pause / Stop** action row at the same
coordinates, fonts, and colors. The only per-mode difference is the mode-specific
content: the manual screen shows the station grid (and the right settings panel),
while the program screen shows the queue list on the right (see `05`). The two
screens are built from a single shared template so the geometry cannot drift.

**Action buttons — one consistent left-nav row.** Next (advance), Pause and Stop
are the same height and share one baseline at the panel bottom. Stop is
distinguished by **color (red)**, not by size or position; Pause is grey and
swaps its label to **Resume** while paused.

### Paused state
- A run (manual **or** program) can be paused from the panel's **Pause** button or
  externally from the OpenSprinkler app. Pause issues `GET /pq?dur=600`, a global
  10-minute auto-resuming pause; **Resume** (or any re-`/pq`) cancels it.
- While paused:
  - The big station countdown **freezes** (it holds, it does not tick).
  - A two-line amber block appears beside the frozen countdown: **`PAUSED`** on
    line 1 and **`Resumes in MM:SS`** (zero-padded, counting down) on line 2. The
    block is right-aligned to the left panel's inner right edge so the gap to the
    countdown flexes; both the run time and the pause cap at `10:00`, so the layout
    is sized for `10:00` and `Resumes in 10:00` co-occurring.
  - The **Pause** button reads **Resume**.
  - The **top bar is unchanged** by pause: there is deliberately **no** top-bar
    "Paused" status word or amber cue, since the two-line `PAUSED` block on the
    run screen is the sole paused indicator. The top bar keeps reflecting the
    underlying connection/enabled/rain-delay state.
- **Auto-advance while paused:** a station does **not** advance while paused (the
  frozen countdown never reaches 0). On resume it continues the current station's
  remaining time, and auto-advance fires normally when that time elapses.
- Entering and leaving the paused state is driven by the controller's polled
  `pq`/`pt` (from `/jc`), so an external pause or resume from the app is reflected
  on the panel, not just a local tap.

### Sleep overlay
- After **5 minutes idle and untouched**, blank the screen (backlight off) and show a minimal "screen off" state. Any touch wakes it and is consumed (does not also trigger the control under the finger).
- **Never sleep while a station is running** — screen stays lit the whole time.

---

## 2. Controls & behavior

Let `RT` = the current run-time setting in seconds. Stations are 1-based in the
UI (`n`), 0-based in the API (`sid = n-1`).

| Control | Where | Behavior |
|---|---|---|
| **Station pill** | grid | Runs that station for `RT`. From idle → Running. While running → jump to it (turn current off, this one on). **Re-tapping the station that is already running restarts it** with the current `RT` (off-then-on), so you can apply a new run time immediately. |
| **Next ›** (advance) | running | Move to the next station and run it for `RT`. **Wraps**: after the last station, goes back to station 1. Skips disabled stations. |
| **Pause / Resume** | running | **Pause** issues `GET /pq?dur=600` (global 10-minute auto-resuming pause). While paused the station countdown freezes and the button reads **Resume**; **Resume** (or any re-`/pq`) cancels the pause and the countdown continues from its remaining time. Applies to both manual and program runs. |
| **Run time − / +** | always | Adjust `RT` (0:15–10:00, 15 s steps, default 1:00). The new value applies to the **next** run or advance; editing it alone does **not** restart the currently running station (re-tap the station pill to apply it now). |
| **Auto-advance** (toggle switch) | always | **Off (default):** when `RT` elapses, the station stops → Idle. **On:** when `RT` elapses, automatically run the **next** station. A full auto pass **stops after the last station** (no loop). The whole row is the tap target; the switch shows state (teal on). No helper sub-text. |
| **■ Stop** | running | Immediately stop everything → Idle. No confirmation. |

**Manual Advance wraps; auto-advance stops after the last station.** This split
is intentional: manual stepping is free navigation (wrap enables a second pass);
an automatic pass is a bounded test that must not water forever.

**Idle vs Running visibility is strict.** Next/Pause/Stop and the station
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
- Idle sub: `Tap a station below to start` (teal)
- Grid label: `STATIONS` (idle and running)
- Eyebrow: `STATION N` (uppercase)
- Run time label: `RUN TIME`
- Auto-advance label: `Auto-advance` (toggle switch, no helper sub-text)
- Buttons: `Next ›` (advance), `Pause` / `Resume`, `■ Stop`
- Paused block: `PAUSED` (line 1) + `Resumes in MM:SS` (line 2, zero-padded), both amber
- Status (top-left): droplet `LV_SYMBOL_TINT` + status word (`Connected` teal · `Syncing...`/`Reconnecting...` amber · `Controller offline`/`Auth error` red). A paused run is **not** shown here (see the on-panel `PAUSED` block). Signal bars labelled `PANEL` / `CTRL`; CTRL shows `— —` when unreachable.
- **No transient toasts.** Completion is conveyed by the confirmed-state UI (return to idle, countdown at `0:00`, no highlight); errors by the top-bar banner. (An earlier toast affordance was removed as redundant.)

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

Targets: Next/Stop and the stepper buttons are large; grid pills are the
fast secondary jump. Everything finger-sized at both 14 and 24 stations.

---

## 6. Edge cases

- **Signal loss:** on any failed/timed-out request, enter the red top-bar state and stop issuing commands. The open station still auto-stops on the controller at its `RT`. When a poll succeeds again, clear the state and re-sync from `/jc` (see `02`).
- **Reconcile to reality:** the controller is the source of truth. The poll updates which station is highlighted and the countdown from the controller's own status — so if a station stops on the controller (cap reached, or someone used the app), the panel reflects it.
- **Missing/disabled stations:** never appear in the grid; Advance and jump skip them.
- **Empty list / can't reach controller at boot:** show a clear "can't reach controller" state (not a blank grid), with the configured host, and keep retrying.
