#!/usr/bin/env python3
"""Load and validate the claude-status config (TOML).

Also a tiny CLI for the shell scripts:
    config.py accounts CONFIG      one "alias<TAB>email" line per account
"""
import os, re, sys, tomllib

DEFAULT_DIR = os.environ.get("CLAUDE_STATUS_CONFIG_DIR",
                             os.path.expanduser("~/.config/claude-status"))

# Claude Code's production OAuth client, as used by its own token refresh.
CLAUDE_CODE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
MAX_ACCOUNTS = 2


class ConfigError(Exception):
    pass


def load(path=None):
    path = path or os.path.join(DEFAULT_DIR, "config.toml")
    try:
        with open(path, "rb") as f:
            raw = tomllib.load(f)
    except FileNotFoundError:
        raise ConfigError(f"no config at {path}")
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(f"{path}: {e}")

    wifi = raw.get("wifi", {})
    dev = raw.get("device", {})
    accts = raw.get("account", [])

    ssid = wifi.get("ssid", "")
    if not ssid:
        raise ConfigError("[wifi] ssid is required")
    hostname = dev.get("hostname", "claude-status")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,31}", hostname):
        raise ConfigError(f"[device] hostname '{hostname}': letters, digits and '-' only, max 32")

    if not 1 <= len(accts) <= MAX_ACCOUNTS:
        raise ConfigError(f"need 1 to {MAX_ACCOUNTS} [[account]] blocks, found {len(accts)}")
    accounts, seen = [], set()
    for n, a in enumerate(accts, 1):
        alias, email = a.get("alias", ""), a.get("email", "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,23}", alias):
            raise ConfigError(f"account {n}: alias '{alias}': 1-23 of letters, digits, '_', '.', '-'")
        if alias in seen:
            raise ConfigError(f"account {n}: alias '{alias}' is used twice")
        if "@" not in email:
            raise ConfigError(f"account '{alias}': email '{email}' doesn't look like an address")
        models = a.get("models", [])
        if not isinstance(models, list) or not all(isinstance(m, str) for m in models):
            raise ConfigError(f"account '{alias}': models must be a list of names")
        seen.add(alias)
        accounts.append({"alias": alias, "email": email, "models": models})

    return {
        "path": path,
        "dir": os.path.dirname(os.path.abspath(path)),
        "ssid": ssid,
        "password": wifi.get("password", ""),
        "hostname": hostname,
        "refresh_scope": dev.get("refresh_scope", "user:profile"),
        "timezone": dev.get("timezone", ""),
        "client_id": dev.get("oauth_client_id", CLAUDE_CODE_CLIENT_ID),
        "accounts": accounts,
    }


def login_dir(cfg, alias):
    return os.path.join(cfg["dir"], "logins", alias)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "accounts":
        sys.exit("usage: config.py accounts CONFIG")
    try:
        cfg = load(sys.argv[2])
    except ConfigError as e:
        sys.exit(f"xx {e}")
    for a in cfg["accounts"]:
        print(f"{a['alias']}\t{a['email']}")
