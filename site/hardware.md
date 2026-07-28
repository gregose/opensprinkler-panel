---
title: Supported hardware
layout: default
nav_order: 2
---

# Supported hardware

The firmware targets the **Hosyond 3.5″ ESP32 display**, a common "cheap yellow
display" (CYD) board with a resistive touchscreen. The same board is sold under
several brands; this firmware was validated on the Hosyond unit linked below.

- **Buy it:** [Amazon: B0D93MBWC2](https://www.amazon.com/dp/B0D93MBWC2)
- **Reference wiki (pin map, specs):** [lcdwiki.com: 3.5inch ESP32-32E Display](https://www.lcdwiki.com/3.5inch_ESP32-32E_Display)

You may also see this board sold under other brands, as **SKU E32R35T**, or by
the community name **ESP32-3248S035R**.

## Key specs

| Component | Detail |
|---|---|
| MCU | Classic **ESP32-WROOM-32E** (ESP32-D0WD-V3), dual-core |
| Memory | 520 KB SRAM, **no PSRAM**, **4 MB flash** |
| Display | **ST7796U** over SPI, **480×320** landscape |
| Touch | **XPT2046 resistive** (shares the display's SPI bus) |
| Power | **USB-C, 5 V** (wall-powered) |

{: .warning }
Make sure you get the **resistive** touch version (the "R" in E32R**35T** /
ESP32-3248S035**R**). Capacitive-touch variants are not supported yet.

## Flashing driver

To flash over USB you may need a USB-to-serial driver so the board's port shows
up on your computer:

- **CH340** driver, or
- **CP210x** (Silicon Labs) driver

Install the one that matches your board's USB chip if the serial port doesn't
appear during [flashing]({{ '/flashing/' | relative_url }}).

{: .note }
Capacitive-touch CYD variants may be added later. For now, stick to the
resistive board linked above.
