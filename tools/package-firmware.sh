#!/usr/bin/env bash
# Package a built PlatformIO ESP32 firmware env into a flashable artifact dir.
#
#   tools/package-firmware.sh <env> <out_dir> [--web]
#
# Produces, in <out_dir>:
#   bootloader.bin partitions.bin firmware.bin boot_app0.bin  (individual parts)
#   merged-firmware.bin                                        (single 0x0 image)
# With --web it also copies the ESP Web Tools browser-flash page into
# <out_dir>/flash/ (the manifest references ../merged-firmware.bin).
#
# Requires esptool importable via `python3 -m esptool` and a built
# .pio/build/<env> directory.
set -euo pipefail

env_name="${1:?usage: package-firmware.sh <env> <out_dir> [--web]}"
out_dir="${2:?usage: package-firmware.sh <env> <out_dir> [--web]}"
include_web="no"
if [[ "${3:-}" == "--web" ]]; then
  include_web="yes"
fi

build_dir=".pio/build/${env_name}"
framework_dir="$HOME/.platformio/packages/framework-arduinoespressif32"
boot_app0_bin="$framework_dir/tools/partitions/boot_app0.bin"

test -f "$boot_app0_bin"

rm -rf "$out_dir"
mkdir -p "$out_dir"

cp "$build_dir/bootloader.bin" "$out_dir/bootloader.bin"
cp "$build_dir/partitions.bin" "$out_dir/partitions.bin"
cp "$build_dir/firmware.bin"   "$out_dir/firmware.bin"
cp "$boot_app0_bin"            "$out_dir/boot_app0.bin"

python3 -m esptool --chip esp32 merge_bin \
  -o "$out_dir/merged-firmware.bin" \
  --flash_mode dio \
  --flash_freq 40m \
  --flash_size 4MB \
  0x1000 "$out_dir/bootloader.bin" \
  0x8000 "$out_dir/partitions.bin" \
  0xe000 "$out_dir/boot_app0.bin" \
  0x10000 "$out_dir/firmware.bin"

if [[ "$include_web" == "yes" ]]; then
  mkdir -p "$out_dir/flash"
  cp flash/index.html flash/manifest.json "$out_dir/flash/"
fi
