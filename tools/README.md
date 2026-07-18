# Local flash and debug tools

These scripts are the lightweight local "flash & debug" companion to the cloud
build. CI owns all compilation. The local machine only downloads a finished
artifact, flashes it over USB, and captures observations for the cloud session.

## Install only two Python packages

No PlatformIO toolchain is needed locally.

```bash
python3 -m pip install "esptool==4.11.0" "pyserial==3.5"
gh auth login
```

## Flash the latest CI artifact for a branch

```bash
./tools/flash.sh --branch my-branch
```

## Flash the latest CI artifact for a pull request

```bash
./tools/flash.sh --pr 10
```

Options:

- `--port /dev/ttyUSB0` overrides auto-detection.
- `--run-id 1234567890` flashes a specific workflow run.
- `--repo gregose/opensprinkler-panel` overrides the current repository remote.

The script downloads the `cyd-35r-firmware` artifact with `gh run download`,
finds `merged-firmware.bin`, then writes it at `0x0`.

## Open a serial monitor

```bash
./tools/monitor.sh
./tools/monitor.sh --port /dev/ttyUSB0
```

This opens `python3 -m serial.tools.miniterm` at `115200`.

## Feedback loop

1. Push or update the branch. Cloud CI builds the firmware artifact.
2. Run `tools/flash.sh` locally, then `tools/monitor.sh`.
3. Capture flash outcome, boot logs, RGB heartbeat, and UI behavior.
4. Report those observations back to the cloud coding session so the next change
   can build on real hardware feedback.
