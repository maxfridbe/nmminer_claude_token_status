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
- [ ] **Long names** truncate (a 13-letter alias shows 9 letters and "~"). Shrink the font instead?

## Ideas

- [ ] Tokens/day graph (like `/stats`). The data lives only on the desktop
      and guest networks block desktop-to-board traffic, so it needs a relay.
- [ ] Antigravity (`agy`) quota. Needs its live login, which isn't in
      `~/.gemini/antigravity-cli/antigravity-oauth-token`.
- [x] Over-the-air updates from GitHub releases (v26.09.02). Verified on the
      board: layout switch kept settings; installed the GitHub build of
      v26.09.02 via OTA_SELFTEST and rebooted into it with both accounts OK.
- [ ] Try **Update firmware** from the menu when the next release is out.
