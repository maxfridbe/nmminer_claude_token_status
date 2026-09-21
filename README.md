# claude-status

A desk display for Claude usage limits. An ESP32 "Cheap Yellow Display" shows
the session window and per-model weekly limits for up to four Claude accounts,
checking every 15 minutes.

Set it up from your phone (join its hotspot, pick your WiFi, sign in to
Claude in the browser) or from a Linux machine with a config file and a script.

![claude-status running on a Cheap Yellow Display](screenshots/claude-monitor.jpg)

- **Ring**: the 5-hour session window, % used, with time until reset.
- **Meters**: the models you choose per account. A model with no limit of its
  own draws on the all-models weekly limit; that's the number shown, tagged
  `shared`.
- **Color**: meters run green until 50%, blend to amber by 75% and red by
  100%. Bars are gradients along that scale.
- **Right edge**: the WiFi name, written vertically, next to a live signal meter.
- A small blue dot shows while a check is running. A failed account keeps its
  last numbers, greyed out, with the error under the ring.

The layout follows the number of accounts:

| Accounts | Layout |
|---|---|
| 1 | Fills the screen: a big session ring on the left, up to four model meters on the right |
| 2 | Side by side, as in the photo |
| 3–4 | Two at a time, flipping to the next page every 12 seconds (`page_seconds`). Swipe sideways, or tap near an edge, to scroll by hand; the timer restarts after a manual scroll. Dots at the bottom and arrows at the edges show where you are |

## Hardware

An ESP32-2432S028 "Cheap Yellow Display": ESP32, 320x240 ILI9341 panel, CH340
USB serial. They're widely available for around $15.

Some units power up with inverted colors. This build sends `INVON` to correct
them (`TFT_INVERSION_ON` in `platformio.ini`). If your screen shows black text
on white, remove that flag.

## Requirements

- To flash the board: Linux with Python 3.11 or newer, and access to its serial
  port (`sudo usermod -aG dialout $USER`, then log out and back in).
  PlatformIO installs itself into `.toolchain/` on first build.
- For the script path only: the `claude` CLI (Claude Code), used to create the
  board's logins.

## Set up from your phone

    ./deploy_and_build.sh --web-setup

1. The board starts a setup hotspot and shows a QR code. Scan it to join; the
   hotspot password changes every boot, so joining means seeing the screen.
2. The setup page opens by itself (or open the address on the screen). Pick
   your WiFi, enter its password, and save. The board restarts and joins it.
3. Reconnect your phone to that WiFi and open the address the board now shows.
   Press **Show PIN on display**, enter the PIN, then **Add a Claude account**:
   name it, choose model meters, tap **Open claude.ai**, approve, and paste
   back the code claude.ai shows. Repeat for each account.

Hold the screen while powering on to get back to the hotspot, for example
after your WiFi changes.

## Set up with a script

    ./deploy_and_build.sh            # first run creates ~/.config/claude-status/config.toml
    $EDITOR ~/.config/claude-status/config.toml
    ./login.sh                       # one browser sign-in per account
    ./deploy_and_build.sh            # build and flash

To try the screen without an account or network, run
`./deploy_and_build.sh --demo`. The demo shows made-up numbers and labels
itself "DEMO - not real data". To preview the other layouts, run
`DEMO_ACCOUNTS=1 ./deploy_and_build.sh --demo`, with any count from 1 to 4.

Other options: `--build-only` validates your config and logins and compiles
without flashing. `--monitor` tails the serial console after flashing.
`--erase` wipes the board first; it asks you to type ERASE, because the board
holds the only live copy of each login. Set `PORT=/dev/ttyUSBx` to pick a
specific board.

With `enable_hosting = true` the setup page stays available on your network
after a script deploy too, for changing settings or adding accounts. Its
address appears on the right edge of the screen, next to the WiFi name.

### Script and website together

The board stores all its settings itself. A script deploy applies
`config.toml` only when the file changed since the last deploy, so settings
changed on the website survive a reflash. When `config.toml` does change,
its account list replaces the board's, and accounts added only on the website
are dropped.

## Configuration

`~/.config/claude-status/config.toml` (see `config.example.toml`):

| Key | Meaning |
|---|---|
| `wifi.ssid`, `wifi.password` | the network the board joins (2.4 GHz) |
| `device.hostname` | how the board appears on your network |
| `device.refresh_scope` | OAuth scope the board keeps; default `user:profile` |
| `device.timezone` | POSIX TZ string; defaults to this computer's |
| `device.page_seconds` | with 3+ accounts, seconds per page; `0` flips only by touch. Default 12 |
| `device.refresh_minutes` | how often to check usage. Default 15 |
| `device.brightness` | backlight %, 5 to 100. Default 90 |
| `device.sleep_minutes` | dim out after this long without a touch or a change; `0` never. Default 0 |
| `device.dim_minutes` | length of that fade before the screen turns off. Default 5 |
| `device.enable_hosting` | keep the setup page on your network. Default false |
| `[[account]]` | one block per account, 1 to 4 |
| `[[account]] alias` | name shown on screen |
| `[[account]] email` | the account's email; `login.sh` refuses a sign-in as anyone else |
| `[[account]] models` | model meters to show, e.g. `["Opus", "Fable"]`; empty shows every limit |

## Screen and power

- The backlight stays at `brightness` while the screen is on.
- With `sleep_minutes` set, the screen fades over `dim_minutes` after that long
  without a touch or a change, then turns off. New numbers, a touch, or a
  change made on the setup page bring it back. While the screen is off, the
  first touch only wakes it.
- WiFi and the setup page stay up while the screen is off.

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

### Signing in on the board

The setup page uses the copy-paste sign-in Claude Code offers for machines
without a browser. The board creates a PKCE challenge, the link takes you to
claude.ai to approve, and claude.ai shows a code that the board exchanges for
tokens. The login belongs to the board alone. Afterwards the board reads the
account's email and plan from the profile endpoint.

### Least privilege

Signing in on the board asks for `user:profile` alone. `claude auth login`
(the script path) always grants Claude Code's full scope set, including running
models, so the board asks for only `user:profile` on every refresh. From its first refresh on, its tokens can read usage
and nothing else. To confirm narrowing works for an account before you
deploy, run `tools/scope_test.py <alias>`.

## Keeping it running

- If a column reads **"Login expired: sign in again"**, the board's login
  chain broke: the login was revoked, the refresh token outlived its lifetime,
  or the board sat unplugged too long. Press **Sign in again** on the setup
  page, or run `./login.sh <alias>` then `./deploy_and_build.sh`.
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
- The setup hotspot is WPA2 with a password that changes every boot and is
  shown only on the screen.
- On your network, any change on the setup page needs a PIN that the board
  displays on request. It's valid for 2 minutes, five wrong guesses cancel it,
  and a correct one signs in one browser.
- The page is plain HTTP. The page never shows tokens, and a sign-in code is
  useless without the PKCE verifier that stays on the board. But someone who
  can watch your network traffic could take over the page session. On a shared
  network, turn hosting off once setup is done.
- Flash encryption is off, so anyone with the board in hand can read its token.
  That token is limited to `user:profile`, meaning your usage numbers. Revoke it
  from your Claude account settings if the board is lost.

## Project layout

| Path | Purpose |
|---|---|
| `deploy_and_build.sh` | build and flash |
| `login.sh` | create and verify the board's own Claude logins |
| `config.example.toml` | config template |
| `src/main.cpp` | boot modes, check scheduler, brightness, dimming and sleep, touch |
| `src/settings.cpp` | settings and logins in the board's flash; applies a script config |
| `src/web.cpp` | setup hotspot, captive portal, setup page API, PIN |
| `src/web_page.h` | the setup page itself |
| `src/api.cpp` | WiFi, NTP, sign-in, token refresh, usage fetch and parsing, demo data |
| `src/ui.cpp` | full-screen, side-by-side and scrolling layouts; ring, meters, WiFi strip |
| `tools/gen_secrets.py` | config + logins → `include/secrets.h` |
| `tools/config.py` | config loading and validation |
| `tools/scope_test.py` | checks that a login can be narrowed to `user:profile` |
| `tools/svg_to_alpha.py` | rasterizes `assets/claude-mark.svg` into `include/claude_mark.h` |
| `include/certs.h` | pinned root CAs for the two TLS endpoints |
| `screenshots/` | photos for this README |

## Caveats

This project isn't affiliated with Anthropic. It relies on an undocumented
endpoint and on Claude Code's OAuth client, either of which can change without
notice. The Claude mark is Anthropic's trademark.
