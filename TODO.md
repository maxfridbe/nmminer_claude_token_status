# TODO

## Test on the hardware

- [ ] **Sign the first account back in.** Its login was lost when an early `flashonly.sh`
      overwrote the settings area (fixed since). Hold the screen, choose
      **Phone setup page**, then **Sign in again** on it. Or run
      `./login.sh <alias>`, then `./deploy_and_build.sh`.
- [ ] **On-screen keyboard.** Hold, then **WiFi on this screen**, pick the
      network, type the password, **Join**. If keys register wrong, note which:
      every keyboard tap logs `[kb] tap x,y` over serial for recalibrating
      `TS_MINY`/`TS_MAXY` in `src/main.cpp`.
- [ ] **Sign-in from the phone page.** First full run of the copy-paste flow.
      Check that claude.ai accepts the `user:profile`-only request at
      authorize time, and that the page resumes at the paste step after
      leaving and rejoining the hotspot.
- [ ] **WiFi with a phone** from the menu: setup hotspot, pick a network,
      accounts kept. Power-cycle to cancel.
- [ ] **First boot on a blank board.** `./flashonly.sh --erase`, then the
      setup screen and **Use this screen**.
- [ ] **Sleep and dim.** Set `sleep_minutes` on the page. Check the fade,
      that a touch wakes it (and doesn't also scroll), and that new numbers
      wake it.
- [ ] **Layouts on real data.** 1 account (full screen) and 3 accounts
      (page flip every 12 s, swipe).

## CrowPanel 2.8" (SKU DHM04728D)

- [x] Display: backlight GPIO 27, MISO GPIO 4 (not 12 as on the CYD).
- [x] Touch: XPT2046 on the display bus, chip select 33, pressure threshold 90.
- [x] Calibration at boot, saved in settings, redoable from the menu.
- [ ] Check taps land correctly with the built-in defaults, and after a
      calibration run.

## Remote setup relay

- [x] Relay over HiveMQ and Mosquitto, tested from a guest network; replays dropped.
- [ ] Watch the public brokers' reliability; add a self-hosted option to the docs if they flake.
- [ ] Sign-in flow end to end over the relay on a phone.

## LCDWiki 4.0" E32R40T

- [x] Pins from the factory test firmware (disassembly), matching LCDWiki's table.
- [x] Display, touch and calibration working on the board.
- [ ] Check the scaled 480x320 layout on real data (two accounts, one account).
- [ ] RGB LED (22/16/17), speaker (26) and battery voltage (34) are unused so far.

## NM-TV (NMMiner 1.54")

- [x] Button found: capacitive touch pad on GPIO 32.
- [x] Display: power enable GPIO 21 and backlight GPIO 19 are both active low
      (NMTech's guide says the backlight is 19 "active low" but that pin also
      drives the LEDs; the power enable's polarity is undocumented).
- [x] One-button menu: tap moves, double tap opens/closes, hold activates,
      with a fill showing how far the hold has got.
- [ ] Run the dashboard with real accounts on it.


## Watch

- [ ] **Refresh-token lifetime.** The first reading was 27.7 days, right after
      a sign-in. Compare `[auth] ... refresh token valid N days` a few days
      later. If it keeps counting down, the board needs a new sign-in about
      every 4 weeks: consider showing a warning on screen a few days ahead.
- [ ] **WiFi joins on a guest network.** Once all 3 attempts failed with status 6,
      followed by ~150 s with no serial output before a manual reset. Watch
      for a repeat.
- [ ] **Colors.** In the README photo the Claude mark reads blue, not coral,
      and the 75% bar blue, not amber. If that's true in person, the panel
      swaps red and blue: add `-D TFT_RGB_ORDER=TFT_BGR`.

## Decide

- [ ] **License.** The repo is public with none, so others can't reuse the code.
- [ ] **Screenshot.** It shows real aliases and the WiFi name.
- [ ] **Claude mark** in a public repo (Anthropic trademark).
- [x] **Long names** drop to a smaller font before truncating (v26.09.03).

## Ideas

- [ ] Tokens/day graph (like `/stats`). The data lives only on the desktop
      and guest networks block desktop-to-board traffic, so it needs a relay.
- [ ] Antigravity (`agy`) quota. Needs its live login, which isn't in
      `~/.gemini/antigravity-cli/antigravity-oauth-token`.
- [x] Over-the-air updates from GitHub releases (v26.09.02). Verified on the
      board: layout switch kept settings; installed the GitHub build of
      v26.09.02 via OTA_SELFTEST and rebooted into it with both accounts OK.
- [x] **Update firmware** from the menu: v26.09.02 to v26.09.03 on the board, accounts kept.
- [x] **Versions** list on the board shows v26.09.07, v26.09.06 and the older
      releases under their pre-per-board file name.
- [x] **Per-board releases** (v26.09.06+): `flashonly.sh --board cyd` flashed
      v26.09.06 from GitHub; the board then updated itself to v26.09.07.
- [ ] Downgrade from **Versions** to an older release, then update back.
