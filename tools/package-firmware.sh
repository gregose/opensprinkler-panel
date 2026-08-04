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

chip="esp32"
flash_mode="dio"
flash_freq="40m"
flash_size="4MB"
bootloader_offset="0x1000"
chip_family="ESP32"
if [[ "$env_name" == *s3* ]]; then
  chip="esp32s3"
  flash_mode="qio"
  flash_size="16MB"
  bootloader_offset="0x0"
  chip_family="ESP32-S3"
fi

test -f "$boot_app0_bin"

rm -rf "$out_dir"
mkdir -p "$out_dir"

cp "$build_dir/bootloader.bin" "$out_dir/bootloader.bin"
cp "$build_dir/partitions.bin" "$out_dir/partitions.bin"
cp "$build_dir/firmware.bin"   "$out_dir/firmware.bin"
cp "$boot_app0_bin"            "$out_dir/boot_app0.bin"

# Include espota.py (from the arduino-esp32 framework tools) so that
# tools/ota.sh can push OTA updates without needing PlatformIO installed locally.
espota_src="$framework_dir/tools/espota.py"
if [[ -f "$espota_src" ]]; then
  cp "$espota_src" "$out_dir/espota.py"
else
  printf 'Warning: espota.py not found at %s — OTA push will not work from this artifact.\n' \
    "$espota_src" >&2
fi

python3 -m esptool --chip "$chip" merge-bin \
  -o "$out_dir/merged-firmware.bin" \
  --flash-mode "$flash_mode" \
  --flash-freq "$flash_freq" \
  --flash-size "$flash_size" \
  "$bootloader_offset" "$out_dir/bootloader.bin" \
  0x8000 "$out_dir/partitions.bin" \
  0xe000 "$out_dir/boot_app0.bin" \
  0x10000 "$out_dir/firmware.bin"

if [[ "$include_web" == "yes" ]]; then
  mkdir -p "$out_dir/flash"
  cp flash/index.html flash/manifest.json "$out_dir/flash/"
  if [[ "$chip_family" != "ESP32" ]]; then
    sed -i.bak "s/\"chipFamily\": \"ESP32\"/\"chipFamily\": \"$chip_family\"/" \
      "$out_dir/flash/manifest.json"
    rm "$out_dir/flash/manifest.json.bak"
  fi
fi
