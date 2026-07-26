# 06 — Hardware Validation Loop (bench runbook)

How a coding agent validates firmware on the **physical panel** and reports
results back. Read this once before your first bench session; the milestone
build order lives in `04-agent-kickoff.md`, the pin map and DoD in
`03-architecture.md`.

> **Split of responsibility.** All compilation happens in **cloud CI** — the
> bench machine has no PlatformIO/compiler, only a repo-local Python `.venv`
> (esptool + pyserial) plus `gh`. The bench's whole job is: pull a finished CI
> artifact, get it onto the board, drive/observe the running UI, and hand
> objective results back to the coding session. You never build here.

---

## The two things that make this fast (and safe)

Most of the speed and nearly all of the safety of this loop come from two tools.
Reach for them first.

### 1. The emulator — validate without a real controller

`docs/mock_os.py` is a faithful, runnable emulator of the OpenSprinkler HTTP
API (24 stations, 10 programs across 3 list pages, a live sequential run queue,
programs/pause). Point the panel's `os_host` at the machine running it and the
firmware can't tell it from a real controller.

```bash
python3 docs/mock_os.py            # serves 0.0.0.0:<port> (default 8080)
python3 docs/test_mock_os.py       # contract/connection tests for the emulator
```

Then set the panel's `os_host` to `<dev-machine-ip>:<port>` (config portal or
`seed-nvs.sh`). Why this is the default target for validation:

- **Safety.** Stations are fake — starting, advancing, and stopping actuate
  nothing. You can exercise every destructive path (run/advance/stop/program
  queue) with zero risk to real valves or a real yard.
- **No hardware dependency.** No OpenSprinkler controller needs to be present or
  reachable; the bench is self-contained.
- **Determinism.** It models the tricky contract exactly — most importantly the
  `en=1`-on-a-running-station **no-op**, so the firmware's off-then-on
  advance/jump/extend logic gets genuinely exercised. Edit the station/program
  fixtures at the top of the file to reproduce a specific layout.
- **Scriptable state.** Debug endpoints let a test drive controller state:
  `POST /_run` starts a scheduled-style run, `POST /_reset` clears state,
  `GET /_state` dumps current state. `--schedule` starts a program run at boot;
  `--require-pw` enforces the `pw` param.

Use a **real controller only for the final M7 pass** (point `os_host` at the
actual OpenSprinkler). Everything before that should be validated against the
emulator.

### 2. Screenshot + synthetic touch — validate without a camera or a finger

`tools/panel.py` drives the panel over the dev-log socket (TCP `2323`, so
"Enable remote debug log" must be on). It pulls a **pixel-exact screenshot** of
whatever LVGL last rendered and injects **synthetic touch** — no camera, no
finger, and results are objective pixels instead of "looks about right".

```bash
./tools/panel.py --host <panel-ip> shot -o screen.png   # capture to PNG
./tools/panel.py --host <panel-ip> tap 145 250          # tap at (x,y)
./tools/panel.py --host <panel-ip> down 40 40           # press-and-hold
./tools/panel.py --host <panel-ip> move 60 40           # drag while pressed
./tools/panel.py --host <panel-ip> up                   # release
./tools/panel.py --host <panel-ip> raw "TAP 10 20"      # send a raw line
```

Coordinates are in the panel's display space (480×320, origin top-left) — the
same values the `[TOUCH]` log lines print, so you can copy a coordinate straight
from a real tap. The PNG is assembled from raw RGB565 bands with the standard
library only (no Pillow), so `shot` works from inside the `.venv`.

**Auto-wake:** the panel blanks its screen after ~5 min idle. `shot` now detects
an all-dark frame, sends one wake tap at a safe corner (`TAP 2 2`, which the
firmware consumes as the wake and does **not** dispatch to the UI), and
re-captures — so a screenshot of a sleeping panel comes back live instead of
black. Pass `--no-wake` to disable. A wake tap only fires when the frame is
actually blank, so it can never actuate a control on a live screen.

---

## One-time bench setup

```bash
./tools/setup.sh    # creates repo-local .venv (Python >=3.10, prefers 3.11) with pinned tools
gh auth login       # or gh auth status to confirm
```

`setup.sh` needs Python ≥3.10 on `PATH` (3.11 matches CI; on macOS
`brew install python@3.11`). The venv lives at `.venv/` (git-ignored) and the
`tools/*.sh` scripts activate it automatically — you never `source` it. Pinned
versions are in `tools/requirements.txt`.

Everything the bench needs is committed to the repo: a fresh clone + `setup.sh`
is a complete working bench. No `/tmp` scratch files, no machine-specific state.

<details>
<summary><b>Why the bench uses a <code>.venv</code>, not Docker</b></summary>

Docker doesn't help here and actively hurts the two most important paths:

- **USB flashing can't work in a Mac/Windows container.** `flash.sh`,
  `monitor.sh`, `nvs.sh`, and `seed-nvs.sh` drive a USB serial bridge (CH340).
  Docker Desktop runs a Linux VM with no native USB passthrough; the USB/IP
  workaround is immature for serial adapters. The most environment-sensitive
  half of the toolchain simply can't see the board.
- **OTA breaks behind Docker's NAT.** `ota.sh` (espota) uses a UDP invitation
  and a reverse connection back to the host; `--network host` isn't real on
  Docker Desktop for Mac/Windows, so that path is fragile-to-broken.
- **There's nothing to isolate.** The entire dependency footprint is two
  pure-Python packages, and the emulator is standard-library-only. `setup.sh`
  already gives clean, pinned, non-polluting isolation.

A native-Linux box with `--device` + `--network host` could containerize the
network-only tools, but it's not worth maintaining a second path. Stick with the
`.venv`.
</details>

---

## Primary loop: OTA against the emulator

This is the routine inner loop for validating a PR. It preserves NVS (so you
provision creds once) and needs no USB cable after the first bootstrap.

```
┌─ cloud ─────────────┐        ┌─ bench (this machine) ──────────────────────┐
│ push branch / PR    │        │ 1. python3 docs/mock_os.py        (emulator) │
│ CI builds artifact  │──SHA──▶│ 2. ./tools/logs.sh                (stream)   │
│ cyd-35r-firmware-<sha>       │ 3. ./tools/ota.sh --pr N ...      (flash OTA) │
└─────────────────────┘        │ 4. ./tools/panel.py ... shot/tap  (validate) │
                               │ 5. report objective results back            │
                               └──────────────────────────────────────────────┘
```

1. **Emulator up.** `python3 docs/mock_os.py`, panel's `os_host` pointed at it.
2. **Logs streaming (optional, once).** `./tools/logs.sh --host <panel-ip>` —
   tees to `logs/serial.log`, rides the OTA reboot, then exits if it can't
   reconnect within its retry window (so it can't wedge the log slot).
3. **Push OTA.** `./tools/ota.sh --pr <N> --host <panel-ip> --ota-pass <pw>`
   (or `--branch <name>` / `--run-id <id>`). It prints the **resolved artifact
   SHA** so you can confirm you're flashing the build you think you are, then
   uploads only the app partition — **NVS is preserved**, no re-provisioning.
   Drive OTA **by IP**, not mDNS, to sidestep intermittent `ospanel.local`
   responder flakiness.
4. **Validate.** `panel.py shot` + `tap` to walk the UI and measure (below).
5. **Report** objective results to the coordinator.

**Bootstrap once per device** before OTA can work: `./tools/flash.sh` (USB)
writes the merged image, then provision Wi-Fi + `os_host` + device password +
**`ota_pass`** (OTA is disabled while `ota_pass` is blank) and tick "Enable
remote debug log" in the captive portal — or seed with `seed-nvs.sh`.

---

## Fallback loop: USB flash + NVS restore

Use USB when OTA can't: first bootstrap, empty/corrupt NVS, a **partition-table
change**, brick recovery, or an OTA responder that's gone unreachable.

```bash
./tools/nvs.sh backup                       # snapshot NVS first (creds/cal/ota_pass)
./tools/flash.sh --pr <N>                    # USB flash; prints the artifact SHA; WIPES NVS
./tools/nvs.sh restore .nvs-backups/nvs-*.bin   # put creds/cal back — no re-provisioning
```

`flash.sh` writes `merged-firmware.bin` at `0x0`, which **wipes NVS** every
time. `nvs.sh backup`/`restore` snapshots the NVS partition (`0x9000`,
`0x5000` B) over USB so you get straight back to a working state instead of
re-running the portal. Backups land in `.nvs-backups/` (git-ignored).

> **Security.** An NVS backup holds your Wi-Fi PSK, the OpenSprinkler password
> hash, and the OTA password **in the clear**. Never commit, upload, or share a
> `.nvs-backups/*.bin`. Treat it like a password file.

If you're bootstrapping from scratch instead of restoring, seed creds with:

```bash
./tools/seed-nvs.sh --ssid ... --pass ... --host <dev-machine-ip>:<port> --ospw ...
```

(`--ospw` is the plaintext OS password; it's MD5-hashed on-chip, never stored in
the clear.)

---

## Objective-measurement discipline

The point of `shot` is to replace subjective judgement with numbers. When a task
asks you to verify a layout, **measure the PNG** and report coordinates, not
impressions:

- Report pixel positions, widths, centers, and colors (e.g. "pager dots span
  x=217..262, center 239.5, matches the arrow-gap center 240 ±1" or "nav buttons
  all 36px tall").
- Pixel analysis (finding a colored region, measuring a label's extent) needs
  Pillow, which is **not** in the `.venv`. Use the system `python3` (it has PIL)
  for analysis, or open the PNG with the `view` tool. `panel.py` itself stays
  stdlib-only so it runs in the venv.
- Drive multi-step flows with `tap`/`down`/`move`/`up`, capturing a `shot`
  between steps, so a reviewer can see each transition.

---

## Wire-level capture probe

When a `shot` hangs, returns black, or only ever comes back via the coverage
fallback, use `tools/probe.py` to look at the raw capture protocol on the wire —
it tells a "bytes never sent" firmware bug apart from a "bytes sent, decoded
wrong" host bug:

```bash
python3 tools/probe.py --host <panel-ip>
```

It sends one `SHOT`, then reports the total bytes, how many STRIP bands arrived,
any non-STRIP control lines, the trailing bytes (hex + ascii), and — the key
signal — whether the `\x02END` terminator (`02 45 4e 44`) actually reached the
wire. Exit status is 0 when `END` is present, 1 otherwise. Stdlib only.

---

## Bench quirks (these will bite)

| Symptom | Cause / fix |
|---|---|
| `esptool` read-flash → "Invalid head of packet" | The CH340 bridge is only reliable at **115200** for reads; higher bauds fail. `nvs.sh` already pins 115200. |
| Flash fails with "multiple access on port" / port busy | Another process holds the serial port. Stop the monitor (`flash.sh` auto-yields `monitor.sh`, but a stray `python`/`esptool` won't); kill it by its **numeric PID**. |
| `panel.py`/`logs.sh` can't connect, or one drops the other | TCP `2323` is a **single-client slot**. `panel.py` and `logs.sh` evict each other — screenshot between log streams, or reconnect `logs.sh` after. |
| `shot` returns an all-black PNG | Panel blanked after ~5 min idle. `shot` now auto-wakes and retries; if still black the board may be off/mid-boot. `--no-wake` disables. |
| OTA reports "failed"/"Host Not Found" or hangs | Drive OTA **by IP**, not mDNS. `ota.sh` pings the host first and warns on no answer — a stale host route or a wedged log/OTA slot is the usual cause. Reboot the board or fall back to USB. |
| Pixel analysis: "No module named PIL" | PIL isn't in the `.venv`. Use the system `python3` for analysis; keep `panel.py` (stdlib) in the venv. |
| `logs.sh` seems to "never die" in the background | It no longer loops forever — it exits after `--retry-window` seconds without a connection, and a `kill <pid>` (SIGTERM) tears it down cleanly and frees the `:2323` slot. |

---

## Reporting back to the coordinator

Close the loop with a report the coding session can act on:

- The **artifact SHA** you actually flashed (from the `flash.sh`/`ota.sh`
  output), and how (OTA vs USB, emulator vs real controller).
- **Per-item PASS/FAIL** with the objective evidence — pixel coordinates,
  measured heights/colors, the wire-probe `END` result, log excerpts.
- Attach or reference the `shot` PNGs for anything visual.
- Any bench anomaly (a quirk above) that could confound the result, so it isn't
  mistaken for a firmware bug.

Do **not** merge PRs from the bench — validation results go to the coordinator;
merges are the human's call.
