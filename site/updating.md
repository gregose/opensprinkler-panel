---
title: Updating firmware
layout: default
nav_order: 7
---

# Updating firmware

The web flasher always installs the **latest release**, so updating is the same
as installing. It detects a classic ESP32 CYD or ESP32-S3 panel and selects the
matching release image automatically:

1. Connect the panel over **USB-C**.
2. Open the [web flasher]({{ '/flash/' | relative_url }}) in Chrome or Edge.
3. Click **Install** to write the latest firmware.

[Open the web flasher →]({{ '/flash/' | relative_url }}){: .btn .btn-primary }

{: .warning }
A USB re-flash **erases the flash**, so you'll re-run
[first-boot setup]({{ '/configuration/' | relative_url }}) afterward.

To change Wi-Fi or controller settings you do not need to re-flash; see [Reconfiguring or resetting the panel]({{ '/configuration/' | relative_url }}#reconfiguring-or-resetting-the-panel).

## Wireless updates (advanced)

The firmware also supports **over-the-air (OTA)** updates over Wi-Fi, which
preserve your saved settings. OTA is only active when you set an OTA password
during [setup]({{ '/configuration/' | relative_url }}), and it's driven from the
command line rather than the browser. The OTA workflow is documented in the
[`tools/`](https://github.com/gregose/opensprinkler-panel/tree/main/tools) and
[`docs/`](https://github.com/gregose/opensprinkler-panel/tree/main/docs) folders.
When downloading an app-only release image manually, use `firmware.bin` for the
CYD and `s3-firmware.bin` for the Waveshare ESP32-S3 board.
