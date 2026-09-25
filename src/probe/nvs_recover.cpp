// Put back an account list that was lost while the logins themselves
// survived (env:cyd-recover, and any other board: it only touches NVS).
//
// settingsSave() writes the account count under "n" and each account's
// fields under al/em/pl/md/sc/seed/at/rt/ex + index. Applying a compiled
// config with no accounts sets the count to zero but leaves every per-account
// key where it was, so the logins are still on the board with nothing
// pointing at them. This counts the accounts that still have a refresh token
// and writes that count back.
//
// It prints what it finds with the tokens masked, so a serial log of a
// recovery never carries the credentials it recovered.

#include <Arduino.h>
#include <Preferences.h>

#include "../model.h"     // MAX_ACCOUNTS, so this can't drift from the firmware

static Preferences prefs;

static String key(const char *base, int i) { return String(base) + i; }

// First and last few characters only: enough to tell two tokens apart.
static String masked(const String &s) {
  if (s.length() < 12) return s.length() ? "(short)" : "(empty)";
  return s.substring(0, 12) + "..." + s.substring(s.length() - 4) + "  [" + s.length() + " chars]";
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n[recover] opening the claudecounts namespace");
  if (!prefs.begin("claudecounts", false)) {
    Serial.println("[recover] no such namespace: nothing to recover");
    return;
  }

  int stored = prefs.getInt("n", 0);
  Serial.printf("[recover] account count says %d\n", stored);
  Serial.printf("[recover] wifi '%s', host '%s'\n",
                prefs.getString("ssid", "").c_str(), prefs.getString("host", "").c_str());

  int found = 0;
  for (int i = 0; i < MAX_ACCOUNTS; i++) {
    String rt = prefs.getString(key("rt", i).c_str(), "");
    if (!rt.length()) continue;
    found = i + 1;                       // accounts are packed from 0
    Serial.printf("[recover]   %d: alias '%s' email '%s' plan '%s'\n", i,
                  prefs.getString(key("al", i).c_str(), "").c_str(),
                  prefs.getString(key("em", i).c_str(), "").c_str(),
                  prefs.getString(key("pl", i).c_str(), "?").c_str());
    Serial.printf("[recover]      refresh %s\n", masked(rt).c_str());
  }

  if (!found) {
    Serial.println("[recover] no logins left in NVS: they have to be added again");
  } else if (found == stored) {
    Serial.printf("[recover] count already matches the %d login(s): nothing to do\n", found);
  } else {
    prefs.putInt("n", found);
    Serial.printf("[recover] count %d -> %d, logins are back\n", stored, found);
  }

#ifdef RECOVER_SSID
  // The same compiled config overwrote the network; put the old one back.
  prefs.putString("ssid", RECOVER_SSID);
  prefs.putString("pass", RECOVER_PASS);
  Serial.printf("[recover] wifi set back to '%s'\n", RECOVER_SSID);
#endif
#ifdef RECOVER_HOST
  prefs.putString("host", RECOVER_HOST);
#endif

  prefs.end();
  Serial.println("[recover] done - flash the board's normal firmware now");
}

void loop() { delay(1000); }
