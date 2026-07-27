# Screenshot capture checklist

The images in this folder are **1×1 transparent placeholders**. The real
screenshots must be captured on the physical panel (they can't be generated
without hardware) and dropped in here at the exact same filenames, replacing the
placeholders.

Capture them with the bench screenshot tool
([`tools/panel.py`](../../../../tools/README.md)), which pulls a pixel-exact
480×320 PNG over the panel's debug socket:

```bash
tools/panel.py --host <panel-ip> shot -o <name>.png
```

The panel's remote debug log (port 2323) must be enabled first (the "Enable
remote debug log" checkbox in the setup portal). Drive the UI into each state
below — either by hand or with `tools/panel.py tap <x> <y>` — then capture.

The fastest way to reach the program / multi-station states deterministically is
to point the panel at the **mock controller** (`docs/mock_os.py`), which serves
24 stations and several programs, instead of a live controller.

| Filename | UI state to show | Fixture / how to get there |
|---|---|---|
| `home-connected.png` | Idle, connected: "Select a station" prompt, full station grid, top bar showing **Connected** with PANEL + CTRL signal meters. | Boot connected to a controller (or `docs/mock_os.py`). Don't start any station. |
| `manual-run.png` | A single station running: `STATION N` eyebrow, station name, large amber countdown, **Next ›** and **■ Stop** buttons, active pill highlighted. | Tap a station pill from idle. Capture while the countdown is a clean value (e.g. after setting Run time to 1:00). |
| `programs-list.png` | Programs list: `PROGRAMS` header, several program rows each with name, next-run/zones/minutes meta line, Enable/Disable + **Run ›** buttons. | Tap **≡ Programs** from idle. Use `docs/mock_os.py` so multiple programs (incl. a disabled one) are present. |
| `program-running.png` | Program running: left column `STATION N OF M`, station name, big countdown; right column live queue with a ✓ completed row, the current ▶ row, and upcoming rows; **Next ›**, **Pause**, **■ Stop**. | From the programs list tap **Run ›** on a multi-station program (the mock's >9-station program shows the windowed queue nicely). Capture a few stations in. |
| `setup-portal.png` | First-boot WiFiManager captive portal: the config form with Wi-Fi network, OpenSprinkler host, and device password fields. | Boot a freshly-flashed panel (empty NVS). Join the `OSPanel-Setup` AP and screenshot the portal page from the connecting device's browser (this one is a browser screenshot, not `panel.py`). |

Notes:

- Keep captures at the native **480×320**; don't upscale or add device frames.
- Use descriptive, real content (real-ish station/program names) so the manual
  reads well.
- After replacing a placeholder, verify the image renders on the built site and
  that its alt text in the referencing page still describes it accurately.
