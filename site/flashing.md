---
title: Flashing
layout: default
nav_order: 3
---

# Flash it in your browser

The easiest way to install the firmware is the **hosted web flasher** — it
writes the firmware straight from your browser over USB, with nothing to
download or install.

[Open the web flasher →]({{ '/flash/' | relative_url }}){: .btn .btn-primary }

{: .warning }
The web flasher needs a **Chromium-based browser** — **Google Chrome** or
**Microsoft Edge** — because it uses the WebSerial API. Firefox and Safari are
not supported.

## Steps

1. Connect the panel to your computer with a **USB-C** cable.
2. Open the [web flasher]({{ '/flash/' | relative_url }}) in Chrome or Edge.
3. Click **Install** and pick the board's serial port when prompted.
4. Wait for the flash to finish, then the panel reboots into
   [first-boot setup]({{ '/configuration/' | relative_url }}).

{: .note }
The **first install erases the flash** and installs a clean image. That's
expected — it clears any previous firmware and stored settings so you start
fresh.

## Port doesn't appear?

If no serial port shows up when you click Install, install the USB-to-serial
driver for your board (**CH340** or **CP210x**). See
[Supported hardware → Flashing driver]({{ '/hardware/' | relative_url }}#flashing-driver).

## Advanced: command-line flashing

If you'd rather flash from a terminal, the repository ships a
[`tools/flash.sh --release`](https://github.com/gregose/opensprinkler-panel/blob/main/tools/flash.sh)
helper that downloads the latest release and writes it over USB. See the
[`tools/` README](https://github.com/gregose/opensprinkler-panel/tree/main/tools)
for details.
