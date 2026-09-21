#!/usr/bin/env bash
# Build claude-status from your config and flash it to the board.
#
#   ./deploy_and_build.sh               build and flash
#   ./deploy_and_build.sh --build-only  validate config and logins, compile, don't flash
#   ./deploy_and_build.sh --monitor     flash, then tail the serial console
#   ./deploy_and_build.sh --demo        offline demo: no config, logins or network
#   DEMO_ACCOUNTS=3 ./deploy_and_build.sh --demo   preview the layout for 1-4 accounts
#
# Config and device logins live in ~/.config/claude-status/ (override with
# CLAUDE_STATUS_CONFIG_DIR). Tokens are written to include/secrets.h only for
# the length of the build; it and the compiled objects that embed them are
# deleted afterwards. Set PORT=/dev/ttyUSBx to pick a board.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$HERE"
CONFIG_DIR="${CLAUDE_STATUS_CONFIG_DIR:-$HOME/.config/claude-status}"
CONFIG="$CONFIG_DIR/config.toml"
VENV="$HERE/.toolchain"
PIO="$VENV/bin/pio"

log()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }

BUILD_ONLY=0; DEMO=0; MONITOR=0
for arg in "$@"; do
  case "$arg" in
    --build-only) BUILD_ONLY=1 ;;
    --demo)       DEMO=1 ;;
    --monitor|-m) MONITOR=1 ;;
    -h|--help)    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)            die "unknown option: $arg (try --help)" ;;
  esac
done
ENV_NAME=cyd
if [[ $DEMO -eq 1 ]]; then
  ENV_NAME=cyd-demo
  # Demo builds must rebuild when the account count changes.
  export PLATFORMIO_BUILD_FLAGS="-D DEMO_ACCOUNTS=${DEMO_ACCOUNTS:-2}"
fi

# --- toolchain: PlatformIO in a local venv, nothing installed system-wide
if [[ ! -x "$PIO" ]]; then
  log "Installing PlatformIO into .toolchain/ (first run only, a few minutes)"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet platformio
fi

# --- secrets: generated for this build only
scrub() {
  rm -f include/secrets.h
  rm -rf .pio/build/cyd/src .pio/build/cyd/firmware.*
}

if [[ $DEMO -eq 0 ]]; then
  if [[ ! -f "$CONFIG" ]]; then
    mkdir -p "$CONFIG_DIR" && chmod 700 "$CONFIG_DIR"
    cp config.example.toml "$CONFIG" && chmod 600 "$CONFIG"
    die "Created $CONFIG. Fill in your WiFi and accounts, run ./login.sh, then run this again."
  fi
  trap scrub EXIT
  log "Reading $CONFIG and device logins"
  python3 tools/gen_secrets.py --config "$CONFIG" --out include/secrets.h
fi

if [[ $BUILD_ONLY -eq 1 ]]; then
  log "Building env:$ENV_NAME"
  "$PIO" run -e "$ENV_NAME"
  log "Build OK"
  [[ $DEMO -eq 0 ]] && log "Output removed: it embeds your tokens. Run without --build-only to flash."
  exit 0
fi

# --- flash
PORT="${PORT:-}"
if [[ -z "$PORT" ]]; then
  for p in /dev/serial/by-id/*USB_Serial* /dev/serial/by-id/*CH340* /dev/ttyUSB*; do
    [[ -e "$p" ]] && { PORT="$p"; break; }
  done
fi
[[ -n "$PORT" ]] || die "No board found. Plug it in over USB, or set PORT=/dev/ttyUSBx"
if [[ ! -r "$PORT" || ! -w "$PORT" ]]; then
  warn "No read/write access to $PORT"
  warn "Permanent fix: sudo usermod -aG dialout \$USER   (then log out and back in)"
  warn "Until replug:  sudo chmod 666 $PORT"
  die  "Cannot flash without port access"
fi

log "Flashing env:$ENV_NAME to $PORT"
"$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT"
log "Flashed"

if [[ $MONITOR -eq 1 ]]; then
  scrub; trap - EXIT
  log "Serial monitor at 115200 (ctrl-c to exit)"
  "$PIO" device monitor -p "$PORT" -b 115200
fi
