---
title: Troubleshooting
layout: default
nav_order: 6
---

# Troubleshooting

## The panel can't reach the controller

The top bar shows **Controller offline** and the **CTRL** signal meter reads
`— —`, while **PANEL** stays valid. That means the panel is on Wi-Fi but can't
talk to the controller. Check:

- The **OpenSprinkler host** you entered is correct and reachable on your
  network (try opening it in a browser).
- The controller is powered on and on the same network.
- Your **device password** is correct — a wrong password shows **Auth error**.

To fix the host or password, hold **BOOT** for about 3 seconds at startup to
reopen configuration; see [Reconfiguring or resetting the panel]({{ '/configuration/' | relative_url }}#reconfiguring-or-resetting-the-panel).

## Wi-Fi dropped / Reconnecting

If the panel loses Wi-Fi it shows **Reconnecting…** and keeps retrying. Any
station already running keeps its own safety timeout on the controller, so it
still stops on its own. When Wi-Fi returns, the panel re-syncs from the
controller automatically.

## The flasher can't see the board

If no serial port appears when you click Install in the
[web flasher]({{ '/flash/' | relative_url }}):

- Use a **Chromium** browser (Chrome or Edge).
- Try a different **USB-C** cable — some are power-only.
- Install the **CH340** or **CP210x** USB-serial driver
  ([Supported hardware]({{ '/hardware/' | relative_url }}#flashing-driver)).

## Recovering a wedged panel

If the firmware ever gets into a bad state, re-flash the latest release with the
[web flasher]({{ '/flash/' | relative_url }}). The first install erases the flash
and installs a clean image; you'll re-run
[first-boot setup]({{ '/configuration/' | relative_url }}) afterward.

## Debug interfaces (advanced)

The firmware has optional, **LAN-only** debug interfaces that are useful when
diagnosing a problem or capturing screenshots:

- An **HTTP screenshot server** that returns a pixel-exact capture of the
  current screen.
- An optional **TCP log** stream for runtime logs.

These are opt-in and stay on your local network. If you want to use them, see
the [repository](https://github.com/gregose/opensprinkler-panel) `tools/` and
`docs/` folders for setup details.
