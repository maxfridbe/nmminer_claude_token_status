# claude-status

A desk display for Claude usage limits. An ESP32 "Cheap Yellow Display" shows
the session window and per-model weekly limits for up to two Claude accounts.
It's always on and refreshes every 15 minutes.

```
┌──────────────────────┬──────────────────────┬─┐
│ ✳ personal           │ ✳ work               │K│
│   PRO                │   MAX 5x             │L│▐
│       ╭────╮         │       ╭────╮         │ │▐
│      │ 54% │         │      │  6% │         │ │▐
│       ╰────╯         │       ╰────╯         │ │▐
│    resets in 2h 14m  │    resets in 47m     │ │▐
│ Opus  shared  Wed 9p │ Opus  shared  Thu 5a │ │▐
│ ▰▰▰▰▰▰▰▰▱▱▱▱   75%   │ ▰▱▱▱▱▱▱▱▱▱▱▱    7%   │ │▐
│                      │ Fable         Thu 5a │ │▐
│                      │ ▰▱▱▱▱▱▱▱▱▱▱▱    9%   │ │▐
└──────────────────────┴──────────────────────┴─┘
```

- **Ring**: the 5-hour session window, % used, with time until reset.
- **Meters**: the models you choose per account. A model with no limit of its
  own draws on the all-models weekly limit; that's the number shown, tagged
  `shared`.
- **Color**: meters run green until 50%, blend to amber by 75% and red by
  100%. Bars are gradients along that scale.
- **Right edge**: the WiFi name, written vertically, next to a live signal meter.
- A small blue dot shows while a check is running. A failed account keeps its
  last numbers, greyed out, with the error under the ring.

## Hardware

An ESP32-2432S028 "Cheap Yellow Display": ESP32, 320x240 ILI9341 panel, CH340
USB serial. They're widely available for around $15.

Some units power up with inverted colors. This build sends `INVON` to correct
them (`TFT_INVERSION_ON` in `platformio.ini`). If your screen shows black text
on white, remove that flag.

## Requirements

- Linux, with Python 3.11 or newer
- The `claude` CLI (Claude Code), used only to create the device's logins
- Access to the board's serial port: `sudo usermod -aG dialout $USER`, then log
  out and back in

PlatformIO is installed automatically into `.toolchain/` on first build.

## Quick start

    ./deploy_and_build.sh            # first run creates ~/.config/claude-status/config.toml
    $EDITOR ~/.config/claude-status/config.toml
    ./login.sh                       # one browser sign-in per account
    ./deploy_and_build.sh            # build and flash

To try the screen without an account or network, run
`./deploy_and_build.sh --demo`. The demo shows made-up numbers and labels
itself "DEMO - not real data".

Other options: `--build-only` validates your config and logins and compiles
without flashing. `--monitor` tails the serial console after flashing. Set
`PORT=/dev/ttyUSBx` to pick a specific board.

## Configuration

`~/.config/claude-status/config.toml` (see `config.example.toml`):

| Key | Meaning |
|---|---|
| `wifi.ssid`, `wifi.password` | the network the board joins (2.4 GHz) |
| `device.hostname` | how the board appears on your network |
| `device.refresh_scope` | OAuth scope the board keeps; default `user:profile` |
| `device.timezone` | POSIX TZ string; defaults to this computer's |
| `[[account]] alias` | name shown on screen |
| `[[account]] email` | the account's email; `login.sh` refuses a sign-in as anyone else |
| `[[account]] models` | model meters to show, e.g. `["Opus", "Fable"]`; empty shows every limit |

## How it works

The board calls the endpoint behind Claude Code's `/usage`:

    GET https://api.anthropic.com/api/oauth/usage

Its `limits` array describes every meter: the `session` window, `weekly_all`,
and `weekly_scoped` rows carrying a model's display name. Rows are classified
by kind, the same way Claude Code does it.

Access tokens last a few hours. The board refreshes its own, 5 minutes before
expiry and again on any 401:

    POST https://platform.claude.com/v1/oauth/token

### Why the board has its own logins

Claude's refresh tokens **rotate**: each refresh retires the previous token.
If the board and your desktop share a login, whichever refreshes first logs
the other out. `login.sh` therefore creates a separate login per account,
stored in `~/.config/claude-status/logins/<alias>/`. Only the build reads it.

After its first refresh, the board holds the only live token for that login,
and the copy on disk is spent. Each build stamps every account with a hash of
its login; the board replaces its stored tokens only when that hash changes,
i.e. after you run `login.sh` again. Rebuilding or reflashing keeps the board's
live tokens.

### Least privilege

`claude auth login` always grants Claude Code's full scope set, including
running models. Usage needs only `user:profile`, so the board asks for exactly
that on every refresh. From its first refresh on, its tokens can read usage
and nothing else. To confirm narrowing works for an account before you
deploy, run `tools/scope_test.py <alias>`.

## Keeping it running

- If a column reads **"Login expired: ./login.sh"**, the board's login chain
  broke: the login was revoked, the refresh token outlived its lifetime, or the
  board sat unplugged too long. Fix it with `./login.sh <alias>`, then
  `./deploy_and_build.sh`.
- Refresh tokens report a lifetime of about 30 days. The serial log prints it on
  every refresh (`refresh token valid N days`), which shows whether it resets
  each time or counts down.
- WiFi stays connected with auto-reconnect. Each check makes 3 join attempts;
  after a failure, retries come at 2, 4 and 8 minutes, then every 15.

## Security

- The repository holds no secrets. Your config and logins live in
  `~/.config/claude-status/`, with mode 700 on directories and 600 on files.
- `include/secrets.h` exists only during a build. It's deleted afterwards,
  together with the compiled objects that embed the tokens.
- Token values are never printed or logged, by the scripts or the firmware.
- The board verifies TLS against pinned roots (`include/certs.h`).
- Flash encryption is off, so anyone with the board in hand can read its token.
  That token is limited to `user:profile`, meaning your usage numbers. Revoke it
  from your Claude account settings if the board is lost.

## Project layout

| Path | Purpose |
|---|---|
| `deploy_and_build.sh` | build and flash |
| `login.sh` | create and verify the board's own Claude logins |
| `config.example.toml` | config template |
| `src/main.cpp` | check scheduler, backlight, redraw tick |
| `src/api.cpp` | WiFi, NTP, token refresh, usage fetch and parsing, demo data |
| `src/ui.cpp` | columns, ring, gradient meters, WiFi strip |
| `tools/gen_secrets.py` | config + logins → `include/secrets.h` |
| `tools/config.py` | config loading and validation |
| `tools/scope_test.py` | checks that a login can be narrowed to `user:profile` |
| `tools/svg_to_alpha.py` | rasterizes `assets/claude-mark.svg` into `include/claude_mark.h` |
| `include/certs.h` | pinned root CAs for the two TLS endpoints |

## Caveats

This project isn't affiliated with Anthropic. It relies on an undocumented
endpoint and on Claude Code's OAuth client, either of which can change without
notice. The Claude mark is Anthropic's trademark.
