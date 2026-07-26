# 05 — Programs (M9)

**Status: shipped.** This document describes the Programs feature **as it was
actually built and hardware-validated** (milestone M9, PRs #84 → #87 → #89 →
#91 → #95). Where the early design mockup or issue #82 differ from what shipped,
this file is authoritative and the divergences are called out in
[§9](#9-divergences-from-the-original-design). The firmware source
(`src/main.cpp`, `lib/program_model`, `lib/os_client`, `lib/panel_state`) is the
final source of truth; this doc is the human-readable contract for it.

Screen is **480×320 landscape**. Read `01-ux-spec.md` first (manual run/idle),
`02-opensprinkler-api.md` for the raw controller API, and `03-architecture.md`
for the concurrency model. The Programs feature reuses all of those.

---

## 1. What Programs is (and isn't)

A **program** is a schedule the OpenSprinkler controller stores: a named set of
station run-times plus a schedule (which days, what start time). The panel is a
**thin client** — the controller remains the source of truth. Programs gives the
tech two things:

1. **See** the programs the controller has: name, when each next runs, how many
   zones and total minutes, and whether it's enabled.
2. **Act** on them without a phone: **run** a program now, **enable/disable** it,
   and once a program is running, **advance / pause / stop** it.

**Out of scope (deliberately):** no create, edit, delete, reorder, or
reschedule of programs. Those stay in the OpenSprinkler app/web UI. The panel
never mutates a program's definition — only its enabled flag and its live run
state.

---

## 2. Where it lives / navigation

- **Entry:** a **`≡ Programs`** button (leading `LV_SYMBOL_LIST` glyph, no
  chevron) in the right-hand settings panel, below the Run-time stepper and
  Auto-advance toggle. Styled like the other secondary buttons — `CLR_LINE`
  fill, `CLR_TEXT` label — and sized to the Run-time stepper height (`STEP_H`,
  44 px) so the right column reads as one family of controls. It is shown when
  idle or during a **manual** station run; it is hidden while a **program** is
  running.
- Tapping it swaps the station grid for the full-width **Programs list** panel.
- **`‹ Back`** (top-right of the list header) returns to the previous screen.
- The list header title is **`PROGRAMS`** (caps, left).

The Programs list and the program-run queue **replace** the interactive station
grid — you cannot tap-to-run an individual station from these screens.

---

## 3. Programs list screen

Full-width panel below the top bar. Header row (`PROGRAMS` + `‹ Back`) then up to
**4 program rows per page** (`MAX_PROG_ROWS = 4`).

Each row:

| Element | Detail |
|---|---|
| **State icon** | `LV_SYMBOL_OK` (✓, teal) when enabled, `LV_SYMBOL_CLOSE` (✗, muted) when disabled. |
| **Program name** | montserrat_16, one line. Ellipsized (`…`) if it overflows — the label is height-pinned to a single line so a long name never wraps into the meta line. Dimmed (`CLR_MUTED`) when the program is disabled. |
| **Meta line** | `<when> • <N> zones • <M> min` (montserrat_12, muted). See below. |
| **Enable / Disable** button | Toggles the program's enabled flag on the controller. Label reflects current state: `Disable` when enabled, `Enable` when disabled. |
| **`Run ›`** button | Runs the program **now** (teal). |

### The meta line

- **`<N> zones`** — count of stations with a non-zero duration in the program.
- **`<M> min`** — total program run-time (sum of durations), rounded up to whole
  minutes.
- **`<when>`** — the computed **next run**, formatted by calendar-day delta from
  the controller's clock (`devt`):
  - same day → `Today H:MM AM/PM`
  - next day → `Tomorrow H:MM AM/PM`
  - 2–6 days out → weekday abbrev, e.g. `Wed 6:00 AM`
  - ≥7 days out → `+N days H:MM AM/PM`
  - never scheduled → `Not scheduled`
- **Disabled programs still show a next run.** The next-run is computed from the
  schedule regardless of the enabled flag (an enabled copy is evaluated), so the
  meta line always reads "when it *would* run." Disabled state is conveyed by the
  ✗ icon and dimming, **not** by a "Disabled" word in the text.

### Pagination

Programs beyond 4 paginate (`MAX_PROG_PAGES = 6`, i.e. up to 24 programs shown).
The pager is a row of **dots** flanked by **`‹` / `›` arrow buttons**:

- Dots are plain indicators (not tappable); the current page's dot is teal, the
  rest `CLR_LINE`. The dot cluster is **re-centred every frame** to the live page
  count so it stays centred on screen regardless of how many pages exist.
- `‹` / `›` step pages (clamped to range). At the first/last page the relevant
  arrow's glyph dims to `CLR_MUTED` but stays tappable.
- The whole pager is **hidden when there is only one page**.
- Navigation is **paged only** — there is no vertical scrolling and **no
  "Page N of M" text** (see [§9](#9-divergences-from-the-original-design)).

### When the list refreshes

The controller's programs are fetched via `/jp` at startup and re-fetched after
an enable/disable toggle. The **next-run times are recomputed locally every
tick** from the cached `/jp` data plus the fresh `devt`/`sunrise`/`sunset` in the
regular `/jc` poll — so the "when" column stays live without re-hitting `/jp`.

---

## 4. Running a program → the program-run screen

Tapping `Run ›` sends the run intent and the controller starts the program. When
the panel's next `/jc` poll shows a program running, the UI switches to the
**program-run screen** (`Phase::ProgramRunning`). It shares the top bar with
every other screen, and is laid out as two columns:

### Left column — current station (mirrors the manual run screen)

- **Eyebrow:** `STATION N OF M` (mono caps, teal) — N counts **up** through a
  **fixed** M (the program's full station count), so completed stations keep the
  denominator stable. If the counter has no active station yet it reads
  `FINISHING`.
- **Station name**, large (montserrat_24), one line, ellipsized if long.
- **Countdown**, large (`ui_font_countdown_48`) — the current station's
  remaining time. This is the single live per-second ticker on the screen.
- **Action row** (bottom): **`Next ›`** (advance), **`Pause`** / **`Resume`**,
  **`■ Stop`** (red). These **replace** the manual screen's Advance/Stop pair.

Run-time stepper and Auto-advance are **hidden** during a program run: a program
carries its own per-station durations and is inherently a sequence, so those
manual controls don't apply.

### Right column — the live queue

A **non-interactive** list derived from the program's stations + the live `/jc`
queue (`ps[]`):

- **Header:** the program name; below it, the total time remaining as
  `M:SS left`.
- **Rows** (up to `MAX_QROWS = 9`), each: a marker, station name, and the
  station's **full configured duration** (static — it does **not** count down;
  the only ticking number is the big countdown on the left).
  - **Current** station: `▶` (`LV_SYMBOL_PLAY`, teal) while running; the glyph
    flips to `⏸` (`LV_SYMBOL_PAUSE`, teal) while the queue is **paused** so the
    row mirrors the paused state. Name in `CLR_TEXT`, duration teal.
  - **Completed** stations: `✓` (`LV_SYMBOL_OK`, muted), name + duration dimmed.
  - **Upcoming** stations: no marker, name in `CLR_TEXT`, duration muted.
- **Windowing:** when a program has more than 9 stations, the list windows around
  the current station, keeping ~2 completed rows visible above it for context and
  sliding as stations finish. Alpha-gradient **fade masks** at the top/bottom edge
  indicate more rows are hidden in that direction (`qfade_top` / `qfade_bottom`,
  toggled by `more_above` / `more_below`).

### Controls during a program run

| Control | Behavior |
|---|---|
| **`Next ›`** | Advance to the next station in the program. Implemented as a controller *skip* (`/cm?...&ssta=1`) which shifts the sequential group forward. |
| **`Pause` / `Resume`** | Pause toggles the controller-wide pause via `/pq?dur=600` — a **fixed 10-minute** pause with auto-resume. Any subsequent `/pq` cancels an active pause. There is no configurable pause duration. |
| **`■ Stop`** | Stops everything immediately (`/cv?rsn=1`) → back to idle. No confirmation. |

### Paused state

- Top-bar status becomes **`Program paused`** (amber).
- The big countdown **freezes** (dead-reckoned by `tick()` but held while paused).
- A **`Resumes in M:SS`** line appears (from the controller's pause countdown
  `pt`), showing time until auto-resume.
- The `Pause` button label becomes `Resume`.
- In the queue list, the current station's `▶` marker flips to a `⏸` pause glyph.

---

## 5. Which program is running? (identification)

The controller does **not** always tell the panel which program is running:
`/jc` reports `pid = 254` for **any** manually/app-triggered program run (both a
panel `Run ›` *and* a run started from the OpenSprinkler app), and `pid = 99` for
a manual single-station run. Only scheduled runs carry a real 1-based `pid`.

So the panel identifies the running program by **matching the live station set
against the `/jp` program definitions**
(`panel_state.cpp::infer_program_index_by_stations`): it collects the stations
live in the run's queue and picks the program whose full station set (durations
> 0) contains all of them with the fewest already-completed. It falls back to a
remembered panel-launched index, then to the legacy `pid`.

**Design consequence:** for reliable identification, **enabled programs must have
disjoint station sets** (disabled programs may overlap). The mock controller
fixtures (`docs/mock_os.py`) honour this on purpose so the identification path is
exercised in tests.

The `run_initiated_by_panel` flag also lets an **externally scheduled** program
run allow the screen to sleep, while a **panel-initiated** run keeps the display
lit (see `03` sleep rules).

---

## 6. API contract used

All verified against OpenSprinkler firmware / API ref; see `02` for the full
detail and the auth/`result` conventions. `pw` = MD5 hex of the device password;
`result == 1` means success. **Program indices are 0-based in write commands.**

| Purpose | Request | Notes |
|---|---|---|
| List programs | `GET /jp` | Returns `nprogs, nboards, mnp, mnst, pnsize, pd[]`. Each `pd` entry = `[flag, days0, days1, [start×4], [dur×N], name, [endr, from, to]]`. Decoded by `program_model::load_program`. |
| Live queue / clock | `GET /jc` | Already the panel's 2 s poll. Fields used: `devt`, `sunrise`, `sunset` (min since midnight), `ps[]` (per-station `[pid, rem, start, gid]`), `pq`/`pt` (pause + countdown). |
| Run program now | `GET /mp?pid=<0-based>&uwt=0&qo=2` | `uwt=0` = ignore weather (100% durations); `qo=2` = replace queue (reset + start). |
| Enable / disable | `GET /cp?pid=<0-based>&en=<0\|1>` | When `en` is present all other params are ignored — a surgical toggle. |
| Pause / resume | `GET /pq?dur=600` | Toggles a fixed 10-minute pause; any `/pq` while paused cancels it. |
| Advance within program | `GET /cm?sid=<current>&en=0&ssta=1` | Shifts the sequential group forward (skip current station). |
| Stop everything | `GET /cv?rsn=1` | Full stop → idle. |

Not used by the panel: `/cp` add/modify, `/dp` delete, `/up` reorder.

URL builders and parsers live in `lib/os_client` (`build_jp_url`,
`build_mp_url`, `build_cp_url`, `build_pq_url`, `parse_jp`, plus the extended
`/jc` fields and the `to_program_ps` adapter). They are covered by mock-transport
native tests.

---

## 7. Next-run computation (`lib/program_model`)

`osp::next_run(program, devt, sunrise_min, sunset_min)` returns the earliest
schedule datetime strictly **after** `now`, as a Unix-epoch second, or `< 0` if
the program never runs (or is disabled).

It walks days `0..400` from `devt` using civil-calendar math (Howard Hinnant's
algorithms, no timezone/DST modelling), and for each day:

- **Day match** by schedule type (flag bits 4–5): weekly (bits 0–6 = Mon…Sun via
  `wd = (weekday + 6) % 7`), interval (`dayNumber % days1 == days0`), monthly
  (day-of-month, with leap/last-day handling), or single-run — combined with the
  odd/even restriction (skips the 31st / Feb 29) and an optional date range
  (`(month<<5)+day`, wraps the year when `from > to`).
- **Start-time resolution** (flag bit 6): fixed start times (16-bit, incl.
  sunrise/sunset ± signed offset, sign bit, disabled bit) or a repeating window
  (`first`, `count`, `interval-min`) expanded into individual starts.

### Known limitations (schedule-only, same as the upstream dashboard math)

- **No weather.** Rain delay, sensor suppression, and weather-adjusted durations
  are **not** modelled — next-run is the raw schedule. The real controller may
  suppress or shift a run the panel still shows.
- **Sunrise/sunset drift.** Future sunrise/sunset are approximated by today's
  values (~1 min/day drift over the horizon).
- **Repeating starts crossing midnight** (`t ≥ 1440`) are dropped.
- **Started-station total** (`remaining + elapsed`) slightly overstates during a
  pause.

These are acceptable for a "next run" hint; the controller remains authoritative
for what actually happens.

---

## 8. Visual tokens & copy strings

Reuses the panel's existing palette and fonts (see `01` §5). Programs-specific:

- **Colors:** `CLR_TEAL` current/enabled/active, `CLR_MUTED` done/disabled/dim,
  `CLR_TEXT` normal, `CLR_AMBER` paused status, `CLR_RED` Stop, `CLR_LINE`
  inactive dots / neutral buttons, `CLR_BG` panel.
- **Fonts:** montserrat 12 (meta), 16 (name/list title/queue), 20 (Back / pager
  arrows), 24 (running station name), `ui_font_countdown_48` (big ticker).
- **Icons:** `LV_SYMBOL_OK` ✓, `LV_SYMBOL_CLOSE` ✗, `LV_SYMBOL_PLAY` ▶,
  `LV_SYMBOL_BULLET` • (meta separator), `LV_SYMBOL_RIGHT` › (Run/Next),
  `LV_SYMBOL_LEFT` ‹ (Back), `LV_SYMBOL_STOP` ■. All built into the montserrat
  symbol font (no custom glyphs, no `\u…` literals that render as tofu).
- **Casing:** eyebrow/caption micro-labels are UPPERCASE (`PROGRAMS`,
  `STATION N OF M`); content/buttons are sentence case (`Run ›`, `Pause`,
  `Resume`, `Enable`, `Disable`, `Program running`, `Program paused`,
  `Resumes in M:SS`, `Not scheduled`).
- **Top-bar status strings** (with a link glyph prefix): `Program running`
  (teal), `Program paused` (amber) — alongside the shared `Running`, `Connected`,
  `Syncing...`, `Auth error`, `Controller offline`, `Reconnecting...`.

---

## 9. Divergences from the original design

The early UX mockup (`programs-screens.html`, embedded in issue #82) and issue
spec were refined during on-hardware validation. The shipped UI differs as
follows — this list is why the original mockup is **not** committed as reference:

| Original design | As shipped |
|---|---|
| Pager with **"Page N of M"** text label | Dots + `‹` / `›` arrows, no page text; dots re-centre live. |
| **"Disabled"** word tag on disabled rows | ✗ icon + dimmed name only; no word. Next-run still computed and shown. |
| Row action **`View ›` / `● Running now`** for the active program | Row button is always **`Run ›`**; the running program is reflected by the screen switching to the program-run view, not by a per-row state. |
| Status **"Running program"** | **"Program running"** (and "Program paused"). |
| Queue rows counting down per-row | Queue rows show **static** full durations; only the left-column big countdown ticks. |
| Configurable pause | **Fixed 10-minute** pause (`/pq?dur=600`) with auto-resume. |
| `STATION N OF M` with a shrinking M | M is the program's **fixed** total; N counts up; completed stations stay in the (windowed) queue as `✓`. |

Behavioral notes that were under-specified originally and are now settled:

- **Advance is a controller skip** (`ssta=1`, off-then-on semantics), not a
  client-side jump.
- **Program identification is by station-set matching**, not by `pid` (which is
  254 for all app/panel runs) — see [§5](#5-which-program-is-running-identification).
- Fade masks on the queue are near-invisible on the dark theme against a matching
  background; they read during scroll and were kept as-is (Greg's call).

---

## 10. Testing

- **`lib/program_model`** — pure C++, full native Unity coverage of `/jp` decode,
  `next_run` across all schedule types + edge cases, and `ps[]` classification /
  queue / N-of-M resolution. Runs under `pio test -e native` (the CI gate).
- **`lib/os_client`** — mock-transport native tests assert exact URL strings for
  `/jp`, `/mp`, `/cp`, `/pq` and the parse of `/jp` + extended `/jc` fields.
- **`lib/panel_state`** — native tests cover screen classification, paused
  fallback + frozen countdown, panel-launched `pid=254` index memory,
  station-set identification of external runs, and pagination clamping.
- **`docs/mock_os.py`** — a full controller emulator (`/jn /jo /jc /js /jp` +
  `/cm /cv /mp /cp /pq`) with fixtures of 24 stations / multiple programs
  (disjoint enabled station sets, a >9-station program for queue overflow, and a
  long program name for the ellipsis path). `docs/test_mock_os.py` is its
  stdlib-only contract suite. Point the panel's `os_host` at the emulator to
  drive the whole Programs UX without the live controller.
- Firmware must build with `pio run -e cyd-35r`; every logic change keeps
  `pio test -e native` green.
