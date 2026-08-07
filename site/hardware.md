---
title: Supported hardware
layout: default
nav_order: 2
---

# Supported hardware

The firmware supports these two boards:

| Board | MCU | Display | Touch | Battery | PlatformIO env |
|---|---|---|---|---|---|
| CYD E32R35T | ESP32-WROOM-32 | ST7796 SPI | XPT2046 resistive | ADC divider on GPIO34 | `cyd-35r` |
| Waveshare ESP32-S3-Touch-LCD-3.5 non-B | ESP32-S3 | ST7796 SPI | FT6336 capacitive | AXP2101 fuel gauge | `s3-touch-35` |

Both boards are included in every release and supported by the browser flasher.

## CYD E32R35T

The Hosyond 3.5″ ESP32 display is a common "cheap yellow display" (CYD) board
with a resistive touchscreen. The same board is sold under several brands; this
firmware was validated on the Hosyond unit linked below.

- **Buy it (affiliate link, supports the project):** [Amazon (affiliate)](https://link.amazon/B072rCpB0)
- **Buy it (non-affiliate):** [Amazon: B0D93MBWC2](https://www.amazon.com/dp/B0D93MBWC2)
- **Reference wiki (pin map, specs):** [lcdwiki.com: 3.5inch ESP32-32E Display](https://www.lcdwiki.com/3.5inch_ESP32-32E_Display)

You may also see this board sold under other brands, as **SKU E32R35T**, or by
the community name **ESP32-3248S035R**.

### Key specs

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

## Waveshare ESP32-S3-Touch-LCD-3.5 non-B

The Waveshare board has an ESP32-S3, 16 MB flash, 8 MB PSRAM, an ST7796 SPI
display, and an FT6336 capacitive touchscreen. Its AXP2101 PMIC reports battery
and external-power state. It flashes over the ESP32-S3 native USB-CDC port.

- **Buy it (affiliate link, supports the project):** [Amazon (affiliate)](https://amzn.to/3S0PVHd)
- **Buy it (non-affiliate):** [Amazon: B0F3WS2W5R](https://www.amazon.com/dp/B0F3WS2W5R)
- **Reference (pin map, specs):** [Waveshare wiki: ESP32-S3-Touch-LCD-3.5](https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.5)

## Flashing drivers

To flash over USB you may need a USB-to-serial driver so the board's port shows
up on your computer:

- **CH340** driver, or
- **CP210x** (Silicon Labs) driver

Install the one that matches your board's USB chip if the serial port doesn't
appear during [flashing]({{ '/flashing/' | relative_url }}). This applies to the
CYD. The Waveshare ESP32-S3 board uses native USB-CDC.

{: .note }
Capacitive-touch CYD variants may be added later. For now, stick to the
resistive board linked above.
