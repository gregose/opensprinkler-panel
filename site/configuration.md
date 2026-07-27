---
title: First-boot setup
layout: default
nav_order: 4
---

# First-boot setup

The first time the panel boots (or after a fresh flash), it has no Wi-Fi or
controller settings, so it starts a **captive-portal setup** so you can enter
them from your phone or laptop.

![The OSPanel-Setup captive-portal menu]({{ '/assets/img/screenshots/setup-portal.png' | relative_url }})

## Connect and configure

1. On boot with no settings, the panel creates a temporary Wi-Fi access point
   named **`OSPanel-Setup`**.
2. Join that network from a phone or laptop. A configuration page opens
   automatically (a captive portal). Tap **Configure** to open the settings
   form.
3. Fill in:
   - **SSID + Password** — the Wi-Fi network the panel will join.
   - **OpenSprinkler host** — the IP address or hostname of your controller
     (for example `192.168.1.100`).
   - **OpenSprinkler device password** — your OpenSprinkler device password.

   ![The captive-portal configuration form]({{ '/assets/img/screenshots/setup-portal-config.png' | relative_url }})

4. Save. The panel stores the settings in its onboard flash (**NVS**), reboots,
   joins your Wi-Fi, and connects to the controller.

{: .note }
Settings persist across reboots and firmware updates, so you only enter them
once. To change them later, see [Reconfiguring or resetting the panel](#reconfiguring-or-resetting-the-panel).

## Reconfiguring or resetting the panel

You do not need to re-flash to change settings. The panel's **BOOT** button (one of the two small side buttons; the other is **RESET**) reopens configuration when it is held during startup.

To reopen configuration:

1. Hold the **BOOT** button and press **RESET** once (or power the panel on while holding **BOOT**).
2. Keep holding **BOOT**. The screen shows **Keep holding BOOT** with two options: **Configure** at 3 seconds and **Factory reset** at 10 seconds.
3. Release at about **3 seconds**, while **Configure** is highlighted.
4. The panel rejoins your Wi-Fi and shows an address such as `http://192.168.1.42`. Open that address in a browser on the same network to change the Wi-Fi network, OpenSprinkler host, or device password, then Save.

{: .note }
If the panel cannot rejoin your Wi-Fi (for example, the network changed), it falls back to the `OSPanel-Setup` access point instead. Join that network and open `192.168.4.1`, the same as first-boot setup. The configuration portal times out after 10 minutes.

### Factory reset

To erase all settings (Wi-Fi, OpenSprinkler host and password, and touch calibration) and start over:

1. Start the panel while holding **BOOT**, as above.
2. Keep holding past the 3-second mark until **Factory reset** highlights at **10 seconds**, then release.
3. The panel erases everything and restarts in first-boot setup with the `OSPanel-Setup` portal.

A full [web-flasher install]({{ '/flashing/' | relative_url }}) also erases stored settings, but you only need that to recover a wedged panel, not to change settings.

{: .note }
Optional advanced settings (such as a firmware over-the-air update password or a
remote debug log) can also be set in the portal. Leave them blank if you don't
need them; see [Troubleshooting]({{ '/troubleshooting/' | relative_url }}) and
[Updating]({{ '/updating/' | relative_url }}).
