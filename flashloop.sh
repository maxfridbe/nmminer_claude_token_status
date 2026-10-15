#!/usr/bin/env bash
# Flash a pile of boards one after another: plug one in, it flashes, unplug
# it, plug in the next. Fetches the firmware once and reuses it. No backups.
#
#   ./flashloop.sh                 pick the board, then flash every board plugged in
#   ./flashloop.sh --board crow28  skip the board question (cyd, crow28, e32r40t or nmtv)
#   ./flashloop.sh --local         flash the image from tools/make_release.sh
#
# Ctrl-C to stop. Boards that were already set up keep their settings, as
# with flashonly.sh. Keep unrelated USB serial devices unplugged: anything
# that shows up while this runs gets flashed.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$HERE"

log()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }
ask()  { local a; read -r -p "$1" a </dev/tty; printf '%s' "$a"; }

LOCAL=0; BOARD="${BOARD:-}"
while [[ $# -gt 0 ]]; do
  arg="$1"; shift
  case "$arg" in
    --board)   BOARD="${1:-}"; shift ;;
    --local)   LOCAL=1 ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)         die "unknown option: $arg (try --help)" ;;
  esac
done

if [[ -z "$BOARD" ]]; then
  echo "Which firmware?"
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
[[ "$BOARD" =~ ^(cyd|crow28|e32r40t|nmtv)$ ]] || die "--board must be cyd, crow28, e32r40t or nmtv"

# Fetch once; every board below gets this same image.
if [[ $LOCAL -eq 0 ]]; then
  ./flashonly.sh --board "$BOARD" --download
fi
[[ -f "firmware/claude-status-$BOARD.bin" ]] || die "No firmware/claude-status-$BOARD.bin. Build one with tools/make_release.sh"

ports() { local p; for p in /dev/ttyUSB* /dev/ttyACM*; do [[ -e "$p" ]] && echo "$p"; done; return 0; }

# Ports to leave alone until they're unplugged: ones just flashed or failed,
# and ones the user said not to touch at the start.
declare -A skip=()
mapfile -t present < <(ports)
if [[ ${#present[@]} -gt 0 ]]; then
  echo "Already plugged in: ${present[*]}"
  if [[ "$(ask "Flash these too? [Y/n] ")" =~ ^[Nn] ]]; then
    for p in "${present[@]}"; do skip[$p]=1; done
  fi
fi

ok=0; failed=0
trap 'echo; log "Stopped. Flashed $ok, failed $failed."; exit 0' INT

log "Plug in a board. Ctrl-C when you're done."
while true; do
  for p in "${!skip[@]}"; do [[ -e "$p" ]] || unset "skip[$p]"; done
  for p in $(ports); do
    [[ -n "${skip[$p]:-}" ]] && continue
    sleep 2                               # let the USB chip settle after plug-in
    [[ -e "$p" ]] || continue
    log "Board on $p: flashing #$((ok + failed + 1)) ($BOARD)"
    if PORT="$p" ./flashonly.sh --board "$BOARD" --local --yes </dev/null; then
      ok=$((ok + 1)); log "OK ($ok done). Unplug it and plug in the next one."
    else
      failed=$((failed + 1)); warn "Failed on $p. Unplug it and plug it back in to retry."
    fi
    skip[$p]=1
  done
  sleep 1
done
