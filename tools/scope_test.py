#!/usr/bin/env python3
"""Test whether a device login can be narrowed to only the scope usage needs.

Refreshes the device login for ACCOUNT asking for just SCOPE, then reads the
usage endpoint with the narrowed token. Prints HTTP statuses and scope names,
never token values.

A successful refresh retires the old refresh token, so the new tokens are
saved back to the login file. Run ./deploy_and_build.sh afterwards so the
device gets the live ones. Run this before the device has taken the login
over; once it has, the copy on disk is spent.

    tools/scope_test.py personal
    tools/scope_test.py work "user:profile user:inference"
"""
import json, os, sys, time, urllib.error, urllib.request

CLIENT_ID = os.environ.get("OAUTH_CLIENT_ID", "9d1c250a-e61b-44d9-88ed-5944d1962f5e")
TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
UA        = "claude-status/1.0 (scope-test)"

def request(url, body=None, token=None):
    headers = {"User-Agent": UA}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
        headers["anthropic-beta"] = "oauth-2025-04-20"
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")

def save(path, data):
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
    os.replace(tmp, path)

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    label = sys.argv[1]
    want  = sys.argv[2] if len(sys.argv) > 2 else "user:profile"
    base  = os.environ.get("CLAUDE_STATUS_CONFIG_DIR", os.path.expanduser("~/.config/claude-status"))
    path  = os.path.join(base, "logins", label, ".credentials.json")

    try:
        data = json.load(open(path))
        o = data["claudeAiOauth"]
    except (OSError, KeyError, ValueError) as e:
        sys.exit(f"xx no device login for '{label}' at {path} ({e}); run ./login.sh {label}")

    print(f"login     {label}")
    print(f"has       {' '.join(o.get('scopes') or [])}")
    print(f"asking    {want}")

    code, body = request(TOKEN_URL, {"grant_type": "refresh_token", "refresh_token": o["refreshToken"],
                                     "client_id": CLIENT_ID, "scope": want})
    if code != 200:
        print(f"refresh   HTTP {code}  {body[:200]}")      # error bodies carry no tokens
        if "invalid_grant" in body:
            print(f"\nThis login's refresh token is already spent, most likely because the device "
                  f"took it over.\nRun ./login.sh {label} for a fresh one, then run this test again.")
        elif "scope" in body:
            print("\nThe server refused the narrower scope. Set refresh_scope = \"\" in config.toml.")
        return 1

    r = json.loads(body)
    granted = (r.get("scope") or "").split()
    print(f"refresh   HTTP 200  (reply fields: {', '.join(r)})")
    print(f"granted   {' '.join(granted) if granted else '(not reported)'}")

    # The refresh just retired the old tokens; keep the new ones.
    o["accessToken"] = r["access_token"]
    if r.get("refresh_token"):
        o["refreshToken"] = r["refresh_token"]
    o["expiresAt"] = int(time.time() * 1000) + int(r.get("expires_in", 3600)) * 1000
    if granted:
        o["scopes"] = granted
    save(path, data)
    print("saved     rotated tokens written back to the login file")

    code, body = request(USAGE_URL, token=o["accessToken"])
    if code != 200:
        print(f"usage     HTTP {code}  {body[:200]}")
        print(f"\nFAIL: the narrowed token cannot read usage. Restore this login with "
              f"./login.sh {label}, and set refresh_scope = \"\" in config.toml.")
        return 1
    rows = [f"{l.get('kind')}={l.get('percent')}%" for l in json.loads(body).get("limits", [])]
    print(f"usage     HTTP 200  {'  '.join(rows)}")

    if granted and set(granted) != set(want.split()):
        print(f"\nINCONCLUSIVE: usage works, but the server kept scopes you didn't ask for, "
              f"so narrowing isn't taking effect.")
        return 2
    print(f"\nPASS: usage works with only '{want}'.\n"
          f"Set refresh_scope = \"{want}\" in config.toml, then ./deploy_and_build.sh.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
