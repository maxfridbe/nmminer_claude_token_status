#!/usr/bin/env bash
# Build the generic firmware for every board (no config, no logins: boards set
# themselves up on screen) into firmware/:
#   claude-status-<board>.bin       whole image, flashes at offset 0
#   claude-status-<board>-app.bin   the app alone, for over-the-air updates
# plus the CYD build under the pre-per-board names (claude-status.bin,
# claude-status-app.bin), which boards on v26.09.02-04 still download.
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

rm -f include/secrets.h                     # generic images: never embed a config
BOOT_APP0="$(find ~/.platformio/packages/framework-arduinoespressif32/tools/partitions -name boot_app0.bin | head -1)"
ESPTOOL="$(find ~/.platformio/packages/tool-esptoolpy -name esptool.py | head -1)"
BOARDS="${BOARDS:-cyd nmtv}"
rm -rf firmware && mkdir -p firmware

for board in $BOARDS; do
  "$PIO" run -e "$board"
  B=".pio/build/$board"
  "$HERE/.toolchain/bin/python" "$ESPTOOL" --chip esp32 merge_bin -o "firmware/claude-status-$board.bin" \
    --flash_mode keep --flash_freq keep --flash_size 4MB \
    0x1000 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"
  cp "$B/firmware.bin" "firmware/claude-status-$board-app.bin"
done

if [[ " $BOARDS " == *" cyd "* ]]; then
  cp firmware/claude-status-cyd.bin firmware/claude-status.bin
  cp firmware/claude-status-cyd-app.bin firmware/claude-status-app.bin
fi
( cd firmware && for f in *.bin; do sha256sum "$f" > "$f.sha256"; done )
ls -l firmware/*.bin | awk '{print $5, $9}'
echo "$FW_VERSION for $UPDATE_REPO"
