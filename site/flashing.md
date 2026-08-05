---
title: Flashing
layout: default
nav_order: 3
---

# Flash it in your browser

The easiest way to install the firmware is the **hosted web flasher**. It writes
the firmware straight from your browser over USB, with nothing to install. This
site serves the latest stable release's merged firmware images directly, so the
browser does not need to fetch them from another origin. The flasher detects
whether the connected board is a classic ESP32 CYD or an ESP32-S3 Waveshare
panel and selects the matching image automatically.

[Open the web flasher →]({{ '/flash/' | relative_url }}){: .btn .btn-primary }

{: .warning }
The web flasher needs a **Chromium-based browser**, such as **Google Chrome** or
**Microsoft Edge**, because it uses the WebSerial API. Firefox and Safari are
not supported.

## Steps

1. Connect the panel to your computer with a **USB-C** cable.
2. Open the [web flasher]({{ '/flash/' | relative_url }}) in Chrome or Edge.
3. Click **Install** and pick the board's serial port when prompted. The
   connected chip determines which firmware image is used.
4. Wait for the flash to finish, then the panel reboots into
   [first-boot setup]({{ '/configuration/' | relative_url }}).

{: .note }
The **first install erases the flash** and installs a clean image. That's
expected. It clears any previous firmware and stored settings so you start
fresh.

## Port doesn't appear?

If a CYD serial port does not show up when you click Install, install its
USB-to-serial driver (**CH340** or **CP210x**). The Waveshare ESP32-S3 uses
native USB-CDC. See
[Supported hardware: Flashing drivers]({{ '/hardware/' | relative_url }}#flashing-drivers).

## Advanced: command-line flashing

For the CYD, the repository ships a
[`tools/flash.sh --release`](https://github.com/gregose/opensprinkler-panel/blob/main/tools/flash.sh)
helper that downloads the latest release and writes it over USB. See the
[`tools/` README](https://github.com/gregose/opensprinkler-panel/tree/main/tools)
for details. The S3 release can be installed with the browser flasher above or
flashed manually from its `s3-*` release assets.
