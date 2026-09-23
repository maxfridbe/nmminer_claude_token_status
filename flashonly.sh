#!/usr/bin/env bash
# Flash the ready-made claude-status firmware onto a Cheap Yellow Display.
# No build tools and no config file: setup happens on the board afterwards
# (WiFi on its touchscreen, Claude accounts from your phone).
#
#   ./flashonly.sh             pick the board and USB device, flash the latest release
#   ./flashonly.sh --board nmtv  skip the board question (cyd, crow28, e32r40t or nmtv)
#   ./flashonly.sh --local     flash the image from tools/make_release.sh
#   ./flashonly.sh --erase     wipe the board first (its WiFi, accounts and logins)
#   PORT=/dev/ttyUSB0 ./flashonly.sh --yes     no questions (not with --erase)
#
# A board that was already set up keeps its WiFi, accounts and logins.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$HERE"
REPO="${CLAUDE_STATUS_REPO:-maxfridbe/nmminer_claude_token_status}"
VENV="$HERE/.flashtool"

log()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }
ask()  { local a; read -r -p "$1" a </dev/tty; printf '%s' "$a"; }

LOCAL=0; ERASE=0; YES=0; BOARD="${BOARD:-}"
while [[ $# -gt 0 ]]; do
  arg="$1"; shift
  case "$arg" in
    --board)   BOARD="${1:-}"; shift ;;
    --local)   LOCAL=1 ;;
    --erase)   ERASE=1 ;;
    --yes|-y)  YES=1 ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)         die "unknown option: $arg (try --help)" ;;
  esac
done

# --- which board. Both can use the same USB chip, so ask rather than guess.
if [[ -z "$BOARD" ]]; then
  if [[ $YES -eq 1 ]]; then BOARD=cyd
  else
    echo "Which board?"
    echo "  1) CYD     ESP32-2432S028 Cheap Yellow Display, 2.8in touchscreen"
    echo "  2) crow28  CrowPanel ESP32 Miner LCD 2.8in (SKU DHM04728D)"
    echo "  3) e32r40t LCDWiki 4.0in ESP32-32E display (E32R40T), 480x320"
    echo "  4) NM-TV   NMMiner NM-TV, 1.54in square screen with a touch button"
    case "$(ask "Board [1]: ")" in
      2|crow28)  BOARD=crow28 ;;
      3|e32r40t) BOARD=e32r40t ;;
      4|nmtv)    BOARD=nmtv ;;
      *)         BOARD=cyd ;;
    esac
  fi
fi
[[ "$BOARD" =~ ^(cyd|crow28|e32r40t|nmtv)$ ]] || die "--board must be cyd, crow28, e32r40t or nmtv"
URL="https://github.com/$REPO/releases/latest/download/claude-status-$BOARD.bin"
IMG="$HERE/firmware/claude-status-$BOARD.bin"

# --- esptool, in a local venv
if [[ ! -x "$VENV/bin/esptool" ]]; then
  log "Installing esptool into .flashtool/ (first run only)"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet 'esptool>=5,<6'
fi
ESPTOOL="$VENV/bin/esptool"

# --- firmware image
if [[ $LOCAL -eq 1 ]]; then
  [[ -f "$IMG" ]] || die "No $IMG. Build one with tools/make_release.sh"
  log "Using local image $(basename "$IMG") ($BOARD)"
else
  mkdir -p firmware
  log "Downloading the latest $BOARD release from github.com/$REPO"
  curl -fL --progress-bar -o "$IMG.part" "$URL" \
    || die "Download failed. Offline? Build locally with tools/make_release.sh, then use --local"
  curl -fsL -o "$IMG.sha256.part" "$URL.sha256" || die "Couldn't download the checksum"
  want="$(cut -d' ' -f1 < "$IMG.sha256.part")"
  got="$(sha256sum "$IMG.part" | cut -d' ' -f1)"
  [[ "$want" == "$got" ]] || die "Checksum mismatch; not flashing"
  mv "$IMG.part" "$IMG"; mv "$IMG.sha256.part" "$IMG.sha256"
  log "Checksum OK"
fi

# --- pick the board
describe() {       # /dev/ttyUSB0 -> "1a86:7523 QinHeng USB Serial"
  local d; d="$(readlink -f "/sys/class/tty/$(basename "$1")/device" 2>/dev/null || true)"
  while [[ -n "$d" && "$d" != "/" && ! -f "$d/idVendor" ]]; do d="$(dirname "$d")"; done
  [[ -f "$d/idVendor" ]] || { echo "unknown device"; return; }
  local vid pid what
  vid="$(cat "$d/idVendor")"; pid="$(cat "$d/idProduct")"
  what="$(cat "$d/manufacturer" 2>/dev/null) $(cat "$d/product" 2>/dev/null)"
  case "$vid:$pid" in
    1a86:*|10c4:ea60|0403:6001|303a:*) what="$what  <- likely an ESP32 board" ;;
  esac
  echo "$vid:$pid ${what# }"
}

if [[ -z "${PORT:-}" ]]; then
  ports=()
  for p in /dev/ttyUSB* /dev/ttyACM*; do [[ -e "$p" ]] && ports+=("$p"); done
  [[ ${#ports[@]} -gt 0 ]] || die "No USB serial devices. Plug the board in with a data cable (charge-only cables won't work)."
  if [[ ${#ports[@]} -eq 1 && $YES -eq 1 ]]; then
    PORT="${ports[0]}"
  else
    echo "USB serial devices:"
    for i in "${!ports[@]}"; do printf '  %d) %-14s %s\n' $((i + 1)) "${ports[$i]}" "$(describe "${ports[$i]}")"; done
    choice="$(ask "Flash which one? [1] ")"
    choice="${choice:-1}"
    [[ "$choice" =~ ^[0-9]+$ && $choice -ge 1 && $choice -le ${#ports[@]} ]] || die "No such device"
    PORT="${ports[$((choice - 1))]}"
  fi
fi
if [[ ! -r "$PORT" || ! -w "$PORT" ]]; then
  warn "No read/write access to $PORT"
  warn "Permanent fix: sudo usermod -aG dialout \$USER   (then log out and back in)"
  warn "Until replug:  sudo chmod 666 $PORT"
  die  "Cannot flash without port access"
fi

# --- flash
if [[ $ERASE -eq 1 ]]; then
  warn "--erase wipes the board's WiFi, accounts, and its only copy of each login."
  [[ "$(ask "Type ERASE to continue: ")" == "ERASE" ]] || die "Not erased"
elif [[ $YES -eq 0 ]]; then
  [[ "$(ask "Flash claude-status to $PORT? Anything already set up on the board is kept. [Y/n] ")" =~ ^[Nn] ]] && die "Cancelled"
fi

# The image is one blob from offset 0, and the gap between its pieces is
# padding that covers the NVS partition, where the board keeps its WiFi,
# accounts and its only copy of each login. Write around it. The partition
# table inside the image says where it is.
read -r NVS_START NVS_END < <(python3 - "$IMG" <<'PY2'
import struct, sys
d = open(sys.argv[1], "rb").read()
for i in range(0x8000, 0x8000 + 0xC00, 32):
    e = d[i:i + 32]
    if e[:2] != b"\xaa\x50":
        break
    ptype, sub, off, size = e[2], e[3], *struct.unpack("<II", e[4:12])
    if ptype == 1 and sub == 2:          # data / nvs
        print(off, off + size)
        break
PY2
)
[[ -n "${NVS_START:-}" ]] || die "Couldn't find the settings partition in the image; not flashing"
PARTS="$(mktemp -d)"
trap 'rm -rf "$PARTS"' EXIT
head -c "$NVS_START" "$IMG" > "$PARTS/boot.bin"
tail -c +"$((NVS_END + 1))" "$IMG" > "$PARTS/app.bin"

if [[ $ERASE -eq 1 ]]; then
  "$ESPTOOL" --chip esp32 -p "$PORT" -b 460800 erase-flash
else
  log "Keeping the board's settings (0x$(printf %x "$NVS_START")-0x$(printf %x "$NVS_END"))"
fi
"$ESPTOOL" --chip esp32 -p "$PORT" -b 460800 write-flash \
  0x0 "$PARTS/boot.bin" "$(printf '0x%x' "$NVS_END")" "$PARTS/app.bin"

log "Done. On the board:"
echo "   1. WiFi: tap 'Use this screen' and type your WiFi password, or scan the QR with a phone."
echo "   2. Accounts: scan the new QR with your phone, open the page, add your Claude accounts."
echo "   3. Any time: press and hold the screen for the menu."
echo "   A board that was already set up just restarts into its dashboard."
