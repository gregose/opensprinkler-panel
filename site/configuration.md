---
title: First-boot setup
layout: default
nav_order: 4
---

# First-boot setup

The first time the panel boots (or after a fresh flash), it has no Wi-Fi or
controller settings, so it starts a **captive-portal setup** so you can enter
them from your phone or laptop.

![The Wi-Fi setup captive portal]({{ '/assets/img/screenshots/setup-portal.png' | relative_url }})

## Connect and configure

1. On boot with no settings, the panel creates a temporary Wi-Fi access point
   named **`OSPanel-Setup`**.
2. Join that network from a phone or laptop. A configuration page opens
   automatically (a captive portal).
3. Fill in:
   - **Wi-Fi network + password** — the network the panel will join.
   - **OpenSprinkler host** — the IP address or hostname of your controller
     (for example `192.168.1.100`).
   - **Device password** — your OpenSprinkler device password.
4. Save. The panel stores the settings in its onboard flash (**NVS**), reboots,
   joins your Wi-Fi, and connects to the controller.

{: .note }
Settings persist across reboots and firmware updates — you only enter them
once. A [full re-flash]({{ '/flashing/' | relative_url }}) erases them, so you'd
set them again after that.

## Re-entering configuration

To change Wi-Fi or controller settings later, re-flash the panel with the
[web flasher]({{ '/flash/' | relative_url }}); the first install erases stored
settings and the panel starts the setup portal again on next boot.

{: .note }
Optional advanced settings (such as a firmware over-the-air update password or a
remote debug log) can also be set in the portal. Leave them blank if you don't
need them — see [Troubleshooting]({{ '/troubleshooting/' | relative_url }}) and
[Updating]({{ '/updating/' | relative_url }}).
