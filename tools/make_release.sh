#!/usr/bin/env bash
# Build the generic firmware (no config, no logins: the board sets itself up
# on its touchscreen) and merge bootloader, partition table and app into one
# image that flashes at offset 0: firmware/claude-status.bin.
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$HERE"
PIO="$HERE/.toolchain/bin/pio"
[[ -x "$PIO" ]] || { echo "Run ./deploy_and_build.sh --build-only --web-setup once to install PlatformIO"; exit 1; }

# Version and repo stamped into the firmware, for on-device updates.
export FW_VERSION="${FW_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
if [[ -z "${UPDATE_REPO:-}" ]]; then
  UPDATE_REPO="$(git remote get-url origin 2>/dev/null | sed -E 's#(git@github.com:|https://github.com/)##; s#\.git$##')"
  export UPDATE_REPO
fi

rm -f include/secrets.h                     # generic image: never embed a config
"$PIO" run -e cyd

B=.pio/build/cyd
BOOT_APP0="$(find ~/.platformio/packages/framework-arduinoespressif32/tools/partitions -name boot_app0.bin | head -1)"
ESPTOOL="$(find ~/.platformio/packages/tool-esptoolpy -name esptool.py | head -1)"
mkdir -p firmware
"$HERE/.toolchain/bin/python" "$ESPTOOL" --chip esp32 merge_bin -o firmware/claude-status.bin \
  --flash_mode keep --flash_freq keep --flash_size 4MB \
  0x1000 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"
# The app alone is what the board downloads to update itself over the air.
cp "$B/firmware.bin" firmware/claude-status-app.bin
( cd firmware && sha256sum claude-status.bin > claude-status.bin.sha256 &&
  sha256sum claude-status-app.bin > claude-status-app.bin.sha256 )
echo "firmware/claude-status.bin ($(du -h firmware/claude-status.bin | cut -f1)) and claude-status-app.bin, $FW_VERSION for $UPDATE_REPO"
