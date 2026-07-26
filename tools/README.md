# Local flash and debug tools

These scripts are the lightweight local "flash & debug" companion to the cloud
build. CI owns all compilation. The local machine only downloads a finished
artifact, flashes it over USB, and captures observations for the cloud session.

> **New to the bench?** Read [`docs/06-hardware-validation-loop.md`](../docs/06-hardware-validation-loop.md)
> first — it's the runbook for the whole validation loop (emulator-first,
> OTA-first, screenshot-driven checks, NVS backup/restore, reporting). This file
> is the per-tool reference.

## One-time setup

No PlatformIO toolchain is needed locally — CI owns all compilation. The local
side only needs esptool + pyserial (pinned to match CI) and the GitHub CLI.

```bash
./tools/setup.sh   # creates a repo-local .venv (Python 3.11) with the pinned tools
gh auth login
```

`setup.sh` requires Python >=3.10 on PATH (prefers 3.11 to match CI). On macOS:
`brew install python@3.11`, then re-run it. The venv lives at `.venv/` (git
-ignored); `flash.sh`, `monitor.sh` and `seed-nvs.sh` activate it automatically,
so you never need to `source` it or reference an absolute path. The pinned
versions live in `tools/requirements.txt`.

## Flash the latest CI artifact for a branch

```bash
./tools/flash.sh --branch my-branch
```

## Flash the latest CI artifact for a pull request

```bash
./tools/flash.sh --pr 10
```

Options:

- `--diag` flashes the diagnostic bring-up firmware
  (`cyd-35r-diag-firmware-<sha>`) instead of the production firmware.
- `--port /dev/ttyUSB0` overrides auto-detection.
- `--run-id 1234567890` flashes a specific workflow run.
- `--repo gregose/opensprinkler-panel` overrides the current repository remote.

Each CI run publishes two artifacts, suffixed with the short commit SHA so
multiple builds are distinguishable: `cyd-35r-firmware-<sha>` (production) and
`cyd-35r-diag-firmware-<sha>` (diagnostic). `flash.sh` resolves the right one
for a run by prefix, downloads it with `gh run download`, finds
`merged-firmware.bin`, then writes it at `0x0`. It prints the **resolved
artifact name** (`Flashing cyd-35r-firmware-<sha> ...`) so you can confirm you
flashed the build you intended.

> `flash.sh` writes the merged image at `0x0`, which **wipes NVS** (Wi-Fi + OS
> config + touch calibration + `ota_pass` + `dev_log`). Snapshot NVS with
> [`nvs.sh backup`](#back-up--restore-nvs-nvssh) first, or prefer OTA (below)
> for routine iteration since it preserves NVS.

## OTA (wireless firmware updates)

ArduinoOTA is compiled into the production `cyd-35r` firmware. It is activated
at runtime only when the NVS `ota_pass` key is non-empty, so a panel with no
OTA password never exposes an unauthenticated update endpoint on the LAN.

After the one-time USB bootstrap you can push new firmware over Wi-Fi. NVS
(Wi-Fi creds, OpenSprinkler config, OTA password, dev_log flag) survives every
OTA because OTA rewrites only the app partition. See
`docs/03-architecture.md` §"Wireless updates" for the full design.

### Bootstrap (once per device)

Flash the production firmware with `merged-firmware.bin` at `0x0`:

```bash
./tools/flash.sh
```

On first boot with an empty NVS, the panel starts the Wi-Fi provisioning
captive portal (AP `OSPanel-Setup`). Set the Wi-Fi network, OpenSprinkler host,
device password, and OTA password there. The OTA password is stored in NVS
under the `ota_pass` key. OTA is disabled if you leave the OTA password blank.

To also enable the TCP log server (port 2323), check "Enable remote debug log"
in the config portal. The log server is off by default.

### Push a firmware update over Wi-Fi

```bash
./tools/ota.sh
```

`ota.sh` downloads the latest successful `cyd-35r-firmware-<sha>` artifact
for the current branch, extracts `firmware.bin` (app partition only) and the
bundled `espota.py`, then calls:

```
python3 espota.py -i ospanel.local -a <ota_pass> -f firmware.bin
```

Options:

- `--host <ip-or-hostname>` overrides the default `ospanel.local` mDNS name.
- `--ota-pass <password>` provides the OTA password (required).
- `--branch <name>`, `--pr <number>`, `--run-id <id>` select the CI run
  (same semantics as `flash.sh`).

`ota.sh` prints the **resolved artifact name** (`OTA: cyd-35r-firmware-<sha> ...`)
so you can confirm the build, and does a best-effort `ping` reachability check
first — if the host doesn't answer it warns about a likely stale route / mDNS
issue (espota rides UDP/TCP 3232). **Drive OTA by IP, not mDNS**, to sidestep
intermittent `ospanel.local` responder flakiness. OTA preserves NVS, so it's the
preferred loop for routine iteration.

### Stream logs over Wi-Fi

```bash
./tools/logs.sh
```

`logs.sh` connects to the TCP log port (2323) on `ospanel.local`,
tees every line to `logs/serial.log`, and auto-reconnects after OTA reboots.
The TCP log server must first be enabled via the "Enable remote debug log"
checkbox in the config portal.

```bash
./tools/logs.sh &                          # background; logs to logs/serial.log
grep "WiFi OK" logs/serial.log | tail -5
```

Options:

- `--host <ip-or-hostname>` overrides `ospanel.local`.
- `--port <port>` overrides port 2323.
- `--log <path>` changes the log file (default `logs/serial.log`).
- `--retry-window <secs>` bounds reconnection (default 30; `0` = exit on the
  first disconnect). After a drop, `logs.sh` retries only for this window — long
  enough to ride an OTA reboot — then **exits** instead of looping forever, so a
  backgrounded reader can't become immortal and wedge the single `:2323` slot.
  Ctrl-C or `kill <pid>` (SIGTERM) tears it down cleanly and frees the slot.

### Full OTA iteration cycle

1. Push a branch. CI builds `cyd-35r-firmware-<sha>`.
2. Run `./tools/logs.sh` (once) to stream runtime logs.
3. Run `./tools/ota.sh` to push the new firmware — the panel reboots, `logs.sh`
   reconnects automatically.
4. Observe the boot + runtime logs in the terminal (or grep `logs/serial.log`).
5. Repeat from step 1.

### Dependency notes

`ota.sh` needs no PlatformIO toolchain locally — it uses the `espota.py`
bundled in the CI artifact and the Python venv created by `setup.sh`. The
only Python dependency is the standard library (`socket`), which `logs.sh` also
uses exclusively.

### Screen capture & synthetic touch (`panel.py`)

`tools/panel.py` drives the panel over the same dev-log socket (port 2323, so
"Enable remote debug log" must be on). It can pull a **pixel-exact screenshot**
of whatever LVGL last drew and inject **synthetic touch** — no camera or finger
needed, so layouts and tap flows can be verified programmatically.

```bash
./tools/panel.py --host <panel-ip> shot -o screen.png   # capture to PNG
./tools/panel.py --host <panel-ip> tap 145 250          # click at (x,y)
./tools/panel.py --host <panel-ip> down 40 40           # press-and-hold
./tools/panel.py --host <panel-ip> move 60 40           # drag while pressed
./tools/panel.py --host <panel-ip> up                   # release
./tools/panel.py --host <panel-ip> raw "TAP 10 20"      # send a raw line
```

How it works: `shot` asks the firmware to force a full-screen redraw and streams
each rendered band back as raw RGB565; `panel.py` reassembles them into a PNG
(standard library only — no Pillow). Touch coordinates are in the panel's
display space (480×320, origin top-left), i.e. the same values the `[TOUCH]`
log lines report, so you can copy a coordinate straight from a real tap. The
protocol lives in `lib/bench_probe`; parsing is covered by `pio test -e native`
(bench_probe) and `tools/test_panel.py`.

Notes:

- **Auto-wake:** the panel blanks after ~5 min idle. `shot` detects an all-dark
  frame, sends one wake tap at a safe corner (`TAP 2 2`, consumed by the
  firmware as the wake — not dispatched to the UI), and re-captures, so a
  sleeping panel still yields a live screenshot. `--no-wake` disables it. The
  wake tap only fires on a genuinely blank frame, so it can't actuate a control.
- Port 2323 is a **single-client slot**: connecting `panel.py` drops any running
  `logs.sh` reader (and vice-versa). Take screenshots between log streams, or
  reconnect `logs.sh` afterwards.
- `--port` / `--timeout` override the defaults (2323 / 10 s).
- Capture only reflects what the firmware itself renders — it never reads the
  ILI9488's GRAM (this SPI module doesn't wire MISO for readback), so it is
  immune to display-readback limitations.
- Pixel analysis of the PNG (measuring a region/label) needs Pillow, which is
  **not** in the `.venv` — use the system `python3` (it has PIL) or the editor's
  image viewer. `panel.py` itself stays stdlib-only so it runs inside the venv.

## Back up & restore NVS (`nvs.sh`)

Because `flash.sh` wipes NVS, `tools/nvs.sh` snapshots and restores the NVS
partition over USB so a USB reflash doesn't cost you a re-provision:

```bash
./tools/nvs.sh backup                              # -> .nvs-backups/nvs-<timestamp>.bin
./tools/flash.sh --pr 10                           # reflash (wipes NVS)
./tools/nvs.sh restore .nvs-backups/nvs-<ts>.bin   # creds/cal/ota_pass back
```

It reads/writes the NVS partition at `0x9000` (`0x5000` B) and pins the CH340 to
**115200** (higher bauds fail read-flash). `restore` refuses a file that isn't
exactly the partition size. Options: `--port` overrides auto-detect; `backup
--out <file>` picks the output path.

> **Security:** an NVS backup holds your Wi-Fi PSK, OpenSprinkler password hash,
> and OTA password **in the clear**. Backups go to `.nvs-backups/` (git-ignored)
> and must never be committed, uploaded, or shared. Treat them like passwords.

## Wire-level capture probe (`probe.py`)

`tools/probe.py` inspects the raw screen-capture protocol on the wire — use it
when `panel.py shot` hangs, returns black, or only decodes via the coverage
fallback, to tell a firmware "bytes never sent" bug from a host decode bug:

```bash
python3 tools/probe.py --host <panel-ip>
```

It sends one `SHOT` and reports total bytes, STRIP band count, any non-STRIP
control lines, the trailing bytes (hex + ascii), and whether the `\x02END`
terminator (`02 45 4e 44`) reached the wire (exit 0 if present, 1 if not).
Stdlib only.



```bash
./tools/monitor.sh
./tools/monitor.sh --port /dev/ttyUSB0
```

This streams serial output to both stdout and a log file (`logs/serial.log` by
default) at `115200`, pulsing DTR/RTS on connect to capture a full boot banner.
It runs happily in the foreground for a human, or in the background for an agent
that greps the log:

```bash
./tools/monitor.sh &           # background; logs to logs/serial.log
grep -n "tick=" logs/serial.log | tail -20
```

You do not need to stop the monitor before flashing. When `tools/flash.sh`
runs, the monitor automatically releases the serial port for the duration of the
flash, then reconnects and re-captures the boot. Coordination happens through a
small state directory (`.serial-monitor/` by default; override with
`--state-dir` or `MON_STATE_DIR`).

## Feedback loop

1. Push or update the branch. Cloud CI builds the firmware artifact.
2. Start `tools/monitor.sh` (once), then run `tools/flash.sh` whenever a new
   artifact is ready — the monitor yields the port and resumes on its own.
3. Capture flash outcome, boot logs, RGB heartbeat, and UI behavior.
4. Report those observations back to the cloud coding session so the next change
   can build on real hardware feedback.

## Seed NVS credentials after a full flash

`tools/flash.sh` writes `merged-firmware.bin` at `0x0`, which wipes NVS.
Use `tools/seed-nvs.sh` to seed Wi-Fi and OpenSprinkler credentials into the
`cyd-35r-diag` firmware so the production `cyd-35r` firmware can connect and
load `/jn` before M4 provisioning exists.

Flash the diagnostic firmware first with `./tools/flash.sh --diag` (or build
locally with `pio run -e cyd-35r-diag`), then:

```bash
./tools/seed-nvs.sh \
  --ssid  "MyNetwork" \
  --pass  "wifi-password" \
  --host  "192.168.1.100" \
  --ospw  "openspr-device-password"
```

The `--ospw` value is the plaintext OpenSprinkler device password. The
diagnostic firmware hashes it to MD5 on-chip via `MD5Builder` before storing it
in NVS — plaintext is never persisted.

Options:

- `--port /dev/ttyUSB0` overrides auto-detection (same logic as `flash.sh`).
- All credential flags are optional; omit any you do not want to change.

After seeding, flash the production `cyd-35r` firmware. It reads the same NVS
namespace (`osp-panel`) and connects automatically.

## Verify connectivity on the diag firmware (`w` / `o`)

Once creds are seeded, the diagnostic firmware can validate the whole network
path independently of the production UI — useful for isolating a fault to a
single layer (link → Wi-Fi → DNS → HTTP → auth → JSON parse). Over the serial
monitor (`./tools/monitor.sh`), send a single character:

- `w` — Wi-Fi connect test: associates using the NVS creds and prints IP,
  RSSI, gateway, subnet, and DNS.
- `o` — OpenSprinkler API test (**read-only**): connects Wi-Fi if needed, then
  fetches `/jn` and `/jc` through `lib/os_client` (the same builders/parsers the
  production firmware uses) and prints the station list, disabled flags,
  controller time/RSSI, and any currently running stations. It never actuates a
  station.

If `o` reports PASS, the M5 API client and the controller credentials are good,
so a blank production grid points at the UI rather than the network.

