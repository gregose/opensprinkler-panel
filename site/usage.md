---
title: Using the panel
layout: default
nav_order: 5
---

# Using the panel

Once connected, the panel builds its station grid from your controller's own
station list: the count, names, and which stations are enabled all come from
the controller. Disabled and master/pump stations don't appear.

## Idle / connected

When nothing is running, the panel shows a **"Select a station"** prompt and the
full grid of your stations. The top bar shows a **Connected** status and two
signal meters: **PANEL** (the panel's own Wi-Fi) and **CTRL** (the controller's
Wi-Fi). If the controller is unreachable, the CTRL meter goes empty (all four
bars unfilled) while PANEL stays filled, a quick way to tell whether it's the
panel or the controller that's having trouble.

![Idle, connected screen]({{ '/assets/img/screenshots/home-connected.png' | relative_url }})

The right-hand settings column has a **Run time** stepper, an **Auto-advance**
toggle, and **Programs** and **History** buttons.

## Run a single station

- **Tap any station** in the grid to start it. It runs for the current
  **Run time** and the panel switches to the running view: the station number,
  its name, and a large countdown.
- **Run time − / +** adjusts the run time (0:15 to 10:00, in 15-second steps,
  default 1:00). Changing it applies to the **next** run; re-tap the running
  station to apply a new time immediately.
- **■ Stop** stops everything and returns to idle. No confirmation.

![Running a single station]({{ '/assets/img/screenshots/manual-run.png' | relative_url }})

## Advance and jump

- **Next ›** moves to the next station and runs it for the current run time.
  Manual Advance **wraps**: after the last station it goes back to the first,
  and skips disabled stations.
- **Tap a different station** in the grid to jump straight to it (the current
  one turns off, the new one turns on).

## Auto-advance

Toggle **Auto-advance** on to cycle the yard automatically:

- **Off (default):** when the run time elapses, the station stops and the panel
  returns to idle.
- **On:** when the run time elapses, the panel automatically starts the **next**
  station. A full auto pass **stops after the last station**; it does not loop,
  so an automatic pass is a bounded test that can't water forever.

## Programs

Tap **≡ Programs** to open the list of programs stored on your controller.

![Programs list]({{ '/assets/img/screenshots/programs-list.png' | relative_url }})

Each row shows the program name, when it next runs, the number of zones, and the
total minutes, plus an **Enable / Disable** toggle and a **Run ›** button. The
panel never creates, edits, or reschedules programs; that stays in the
OpenSprinkler app. It only runs them and toggles their enabled flag.

### Running a program

Tap **Run ›** to start a program now. The panel switches to the program-run
screen:

![A program running with its live queue]({{ '/assets/img/screenshots/program-running.png' | relative_url }})

- **Left:** the current station (`STATION N OF M`), its name, and a big
  countdown for the current station's remaining time.
- **Right:** a live **queue** of the program's stations: completed ones marked
  with a check, the current one marked with a play (or pause) glyph, and the
  rest upcoming, with a total time remaining at the top.

Controls during a program run:

- **Next ›**: advance to the next station in the program.
- **Pause / Resume**: pause toggles a fixed **10-minute** pause with automatic
  resume; the status bar shows **Program paused** and a **Resumes in M:SS**
  countdown. Tapping Pause again (or Resume) cancels the pause.
- **■ Stop**: stop everything immediately and return to idle.

{: .note }
The controller is always the source of truth. If a station stops on the
controller (a cap is reached, or someone uses the app), the panel updates to
match on its next poll.

## History

Tap **History** from the idle screen to see a log of recent runs and events
from the controller, most recent first.

![The run history log]({{ '/assets/img/screenshots/history-list.png' | relative_url }})

Each row shows what ran, how it was triggered, how long it ran, and when it
started:

- **Station and trigger** — the station name on the left, and how the run
  started in the middle: a program name for scheduled runs, or **Manual** for a
  run you started from the panel or the OpenSprinkler app.
- **Duration** — how long that station actually ran.
- **When** — a friendly timestamp, for example `Today 6:32a` or `Tue 8:15p`.

The log covers roughly the **last 30 days** and is paged: use **‹** and **›** to
move between pages, with the current page shown as **Page N / M**. **‹ Back**
returns to the idle screen. If the controller has no recent activity, History
shows an empty state instead of a list.

{: .note }
History is read from the controller's own log, so it reflects **every** run —
scheduled programs and runs started from the OpenSprinkler app included, not
just the ones you start from the panel.

## Sleep

After about **5 minutes** idle and untouched, the screen blanks to save the
display. Any touch wakes it (and that touch is consumed, so it won't also
trigger a control). The panel **never sleeps while a station or program is
running**.
