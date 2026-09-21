#!/usr/bin/env bash
# Create the device's own Claude logins, one per [[account]] in your config.
#
# Why separate logins: Claude OAuth refresh tokens rotate, so when two clients
# share a login, whichever refreshes first logs the other out. Each login made
# here lives in ~/.config/claude-status/logins/<alias>/ and is only ever read
# by deploy_and_build.sh; the board takes it over on its first refresh. Never
# point Claude Code at those directories yourself.
#
#   ./login.sh           log in every account that has no device login yet
#   ./login.sh ALIAS     (re)do one account, e.g. after "Login expired"
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$HERE"
CONFIG_DIR="${CLAUDE_STATUS_CONFIG_DIR:-$HOME/.config/claude-status}"
CONFIG="$CONFIG_DIR/config.toml"

log()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }

[[ -f "$CONFIG" ]] || die "No config at $CONFIG. Run ./deploy_and_build.sh once to create it."
command -v claude >/dev/null || die "The claude CLI isn't on PATH"
ONLY="${1:-}"
did=0

while IFS=$'\t' read -r -u 3 alias email; do
  [[ -n "$ONLY" && "$ONLY" != "$alias" ]] && continue
  dir="$CONFIG_DIR/logins/$alias"

  if [[ -z "$ONLY" && -s "$dir/.credentials.json" && -s "$dir/verified-email" ]]; then
    log "$alias: already logged in as $(cat "$dir/verified-email"). Use ./login.sh $alias to redo it."
    continue
  fi

  mkdir -p "$dir" && chmod 700 "$dir"
  echo
  log "Device login for '$alias': sign in as $email"
  warn "If your browser is signed in to a different Claude account, open the link in a private window."
  CLAUDE_CONFIG_DIR="$dir" claude auth login </dev/tty

  got="$(CLAUDE_CONFIG_DIR="$dir" claude auth status 2>/dev/null \
         | python3 -c 'import json,sys; print(json.load(sys.stdin).get("email",""))' 2>/dev/null || true)"
  if [[ "${got,,}" == "${email,,}" ]]; then
    echo "$got" > "$dir/verified-email" && chmod 600 "$dir/verified-email"
    log "$alias: verified as $got"
  else
    rm -f "$dir/.credentials.json" "$dir/verified-email"
    die "'$alias' signed in as '${got:-unknown}', but the config says $email. Discarded that login; run ./login.sh $alias and pick the right account."
  fi
  did=1
done 3< <(python3 tools/config.py accounts "$CONFIG")

[[ -n "$ONLY" && $did -eq 0 ]] && die "No account with alias '$ONLY' in $CONFIG"
echo
log "Done. Run ./deploy_and_build.sh to send the logins to the board."
