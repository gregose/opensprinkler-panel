# Local flash and debug tools

These scripts are the lightweight local "flash & debug" companion to the cloud
build. CI owns all compilation. The local machine only downloads a finished
artifact, flashes it over USB, and captures observations for the cloud session.

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
`merged-firmware.bin`, then writes it at `0x0`.

## Open a serial monitor

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

