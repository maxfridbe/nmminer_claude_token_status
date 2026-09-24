#!/usr/bin/env bash
# Turn a stock board into a claude-status one, in one pass:
# back up what is on it, erase, write a release image, and read the banner
# back to prove it booted.
#
#   tools/convert.sh nmtv              # latest release, first serial port
#   tools/convert.sh nmtv v26.09.13 /dev/ttyUSB1
#
# The backup matters on a stock NMMiner: its per-device licence lives in
# flash and nothing else has a copy.
set -euo pipefail

BOARD="${1:-nmtv}"
VERSION="${2:-latest}"
PORT="${3:-}"

HERE="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$HERE"
PY="$HERE/.toolchain/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
REPO=maxfridbe/nmminer_claude_token_status
CACHE="$HERE/.cache"

log() { printf '\033[36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31m!!\033[0m %s\n' "$*" >&2; exit 1; }

[[ -x "$PY" ]] || die "no toolchain yet: run ./deploy_and_build.sh once first"
[[ -f "$ESPTOOL" ]] || die "esptool not found at $ESPTOOL"

if [[ -z "$PORT" ]]; then
  mapfile -t ports < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
  (( ${#ports[@]} )) || die "no serial port found - is the board plugged in?"
  (( ${#ports[@]} == 1 )) || die "several ports (${ports[*]}): name the one you mean"
  PORT="${ports[0]}"
fi
log "board $BOARD on $PORT"

esp() { "$PY" "$ESPTOOL" --port "$PORT" --baud 921600 "$@"; }

MAC=$(esp chip_id 2>/dev/null | sed -n 's/^MAC: //p' | tr -d ':' | tail -1)
[[ -n "$MAC" ]] || die "no answer from the board on $PORT"
log "chip $MAC"

# --- 1. back up whatever is on it, once per board
# Outside the repo: these are 4MB and carry the board's own licence.
BACKUP="$HERE/../backup/stock-$BOARD-$MAC-4MB.bin"
if [[ -f "$BACKUP" ]]; then
  log "already have $BACKUP, keeping it"
else
  log "reading 4MB off the board -> $BACKUP"
  mkdir -p "$(dirname "$BACKUP")"
  esp read_flash 0 0x400000 "$BACKUP" >/dev/null
  sha256sum "$BACKUP" > "$BACKUP.sha256"
  log "backed up $(du -h "$BACKUP" | cut -f1)"
fi

# --- 2. fetch the release image, cached
if [[ "$VERSION" == latest ]]; then
  VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
            "$PY" -c 'import sys,json; print(json.load(sys.stdin)["tag_name"])')
fi
IMG="$CACHE/claude-status-$BOARD-$VERSION.bin"
if [[ ! -f "$IMG" ]]; then
  mkdir -p "$CACHE"
  log "downloading $VERSION for $BOARD"
  curl -fsSL -o "$IMG" \
    "https://github.com/$REPO/releases/download/$VERSION/claude-status-$BOARD.bin" ||
    die "no claude-status-$BOARD.bin in release $VERSION"
fi
log "flashing $VERSION ($(stat -c%s "$IMG") bytes)"

# --- 3. erase and write. A stock board has nothing of ours to keep, so this
# takes the whole chip, NVS included.
esp erase_flash >/dev/null
esp write_flash 0x0 "$IMG" >/dev/null
log "written"

# --- 4. prove it came back
"$PY" - "$PORT" <<'EOF'
import sys, time, re, serial
s = serial.Serial(sys.argv[1], 115200, timeout=0.4)
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)
end = time.time() + 15
for _ in range(600):
    if time.time() > end: break
    line = re.sub(r'\x1b\[[0-9;]*m', '', s.readline().decode('utf-8', 'replace')).rstrip()
    if not line: continue
    if 'starting' in line or '[input]' in line or '[wifi]' in line or '[touch]' in line:
        print('   ', line, flush=True)
    if 'starting' in line: end = min(end, time.time() + 4)
s.close()
EOF
log "done: unplug it and plug in the next one"
