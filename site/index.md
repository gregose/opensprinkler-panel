---
title: Overview
layout: default
nav_order: 1
---

# OpenSprinkler Panel

A 3.5″ touch panel for running and stepping your [OpenSprinkler](https://opensprinkler.com/) stations and
programs. It provides local control directly to your OpenSprinkler controller
over its HTTP API, on your own network. New to OpenSprinkler? It's an
open-source sprinkler/irrigation controller; learn more at
[opensprinkler.com](https://opensprinkler.com/).

![The panel on its idle, connected screen]({{ '/assets/img/screenshots/home-connected.png' | relative_url }})

[Flash it in your browser →]({{ '/flashing/' | relative_url }}){: .btn .btn-primary }
[Supported hardware]({{ '/hardware/' | relative_url }}){: .btn }

## What it does

- **Run a single station** for an adjustable run time, then Stop or Advance to
  the next one.

  ![Manually running a single station]({{ '/assets/img/screenshots/manual-run.png' | relative_url }})

- **Auto-advance** through your stations for a bounded test pass of the whole
  yard.

- **See and run programs** stored on the controller: name, next run, zone
  count, and total minutes at a glance.

  ![The programs list]({{ '/assets/img/screenshots/programs-list.png' | relative_url }})

- **Drive a running program** with a live queue: advance to the next station,
  pause/resume, or stop, all reflected against the controller's real state.

  ![A program running with its live queue]({{ '/assets/img/screenshots/program-running.png' | relative_url }})

## Get started

1. [Flash the firmware]({{ '/flashing/' | relative_url }}) onto a supported board
   from your browser.
2. [Configure it]({{ '/configuration/' | relative_url }}) on first boot: Wi-Fi
   plus your OpenSprinkler host and device password.
3. [Use the panel]({{ '/usage/' | relative_url }}) to run stations and programs.

Everything runs on your LAN. No credentials leave your network.
