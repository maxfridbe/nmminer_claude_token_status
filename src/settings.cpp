#include "settings.h"
#include <Preferences.h>

#if __has_include("secrets.h") && !defined(DEMO_DATA)
#include "secrets.h"
#define HAVE_SECRETS 1
#endif

Settings cfg;
static Preferences prefs;

// Key names are short (NVS allows 15 chars). at/rt/ex/seed + index predate
// this file; a board's only live tokens sit under them, so they never change.
static String key(const char *base, int i) { return String(base) + i; }

static void getStr(const char *k, char *out, size_t n, const char *def = "") {
  strlcpy(out, prefs.getString(k, def).c_str(), n);
}

static void loadNvs() {
  getStr("ssid", cfg.ssid, sizeof(cfg.ssid));
  getStr("pass", cfg.pass, sizeof(cfg.pass));
  getStr("host", cfg.hostname, sizeof(cfg.hostname), "claude-status");
  getStr("tz", cfg.tz, sizeof(cfg.tz), "UTC0");
  getStr("rscope", cfg.refreshScope, sizeof(cfg.refreshScope), "user:profile");
  cfg.hosting     = prefs.getBool("hosting", false);
  cfg.lightMode   = prefs.getBool("light", false);
  getStr("relay", cfg.relay, sizeof(cfg.relay));
  cfg.pageSeconds    = prefs.getUShort("pagesec", 12);
  cfg.refreshMinutes = prefs.getUShort("refmin", 15);
  cfg.brightness     = prefs.getUChar("bright", 90);
  cfg.sleepMinutes   = prefs.getUShort("sleepmin", 0);
  cfg.dimMinutes     = prefs.getUShort("dimmin", 5);
  cfg.nAccounts   = constrain(prefs.getInt("n", 0), 0, MAX_ACCOUNTS);
  static const uint16_t calDefault[5] = TOUCH_CAL_DEFAULT;
  memcpy(cfg.touchCal, calDefault, sizeof(cfg.touchCal));
  cfg.touchCalOk  = prefs.getBytes("tcal", cfg.touchCal, sizeof(cfg.touchCal)) == sizeof(cfg.touchCal);

  for (int i = 0; i < cfg.nAccounts; i++) {
    AccountCfg &a = cfg.acct[i];
    getStr(key("al", i).c_str(), a.alias, sizeof(a.alias));
    getStr(key("em", i).c_str(), a.email, sizeof(a.email));
    getStr(key("pl", i).c_str(), a.plan, sizeof(a.plan), "?");
    getStr(key("md", i).c_str(), a.models, sizeof(a.models));
    getStr(key("sc", i).c_str(), a.scope, sizeof(a.scope), cfg.refreshScope);
    getStr(key("seed", i).c_str(), a.seed, sizeof(a.seed));
    a.access    = prefs.getString(key("at", i).c_str(), "");
    a.refresh   = prefs.getString(key("rt", i).c_str(), "");
    a.expiresMs = prefs.getULong64(key("ex", i).c_str(), 0);
  }
}

void settingsSaveTouchCal() {
  prefs.putBytes("tcal", cfg.touchCal, sizeof(cfg.touchCal));
  cfg.touchCalOk = true;
  Serial.printf("[touch] calibration saved: %u %u %u %u %u\n", cfg.touchCal[0], cfg.touchCal[1],
                cfg.touchCal[2], cfg.touchCal[3], cfg.touchCal[4]);
}

void settingsSaveTokens(int i) {
  AccountCfg &a = cfg.acct[i];
  prefs.putString(key("at", i).c_str(), a.access);
  prefs.putString(key("rt", i).c_str(), a.refresh);
  prefs.putULong64(key("ex", i).c_str(), a.expiresMs);
  prefs.putString(key("seed", i).c_str(), a.seed);
}

void settingsSave() {
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("host", cfg.hostname);
  prefs.putString("tz", cfg.tz);
  prefs.putString("rscope", cfg.refreshScope);
  prefs.putBool("hosting", cfg.hosting);
  prefs.putBool("light", cfg.lightMode);
  prefs.putString("relay", cfg.relay);
  prefs.putUShort("pagesec", cfg.pageSeconds);
  prefs.putUShort("refmin", cfg.refreshMinutes);
  prefs.putUChar("bright", cfg.brightness);
  prefs.putUShort("sleepmin", cfg.sleepMinutes);
  prefs.putUShort("dimmin", cfg.dimMinutes);
  prefs.putInt("n", cfg.nAccounts);
  for (int i = 0; i < cfg.nAccounts; i++) {
    AccountCfg &a = cfg.acct[i];
    prefs.putString(key("al", i).c_str(), a.alias);
    prefs.putString(key("em", i).c_str(), a.email);
    prefs.putString(key("pl", i).c_str(), a.plan);
    prefs.putString(key("md", i).c_str(), a.models);
    prefs.putString(key("sc", i).c_str(), a.scope);
    settingsSaveTokens(i);
  }
}

bool settingsHaveWifi() { return cfg.ssid[0] != 0; }

int settingsFindAccount(const char *alias) {
  for (int i = 0; i < cfg.nAccounts; i++)
    if (!strcmp(cfg.acct[i].alias, alias)) return i;
  return -1;
}

void settingsRemoveAccount(int i) {
  if (i < 0 || i >= cfg.nAccounts) return;
  for (int j = i; j < cfg.nAccounts - 1; j++) cfg.acct[j] = cfg.acct[j + 1];
  cfg.nAccounts--;
  cfg.acct[cfg.nAccounts] = AccountCfg();
  settingsSave();
}

void settingsRequestSetup() { prefs.putBool("setupnext", true); }

bool settingsTakeSetupFlag() {
  if (!prefs.getBool("setupnext", false)) return false;
  prefs.remove("setupnext");          // one boot only: power-cycling cancels
  return true;
}

void settingsFactoryReset() {
  prefs.clear();
  Serial.println("[settings] factory reset: all settings and logins erased");
}

#ifdef HAVE_SECRETS
// A script deploy carries a full config. It is applied only when it differs
// from the one applied last (CONFIG_SEED), so a plain reflash keeps anything
// changed on the website. Tokens follow each account by alias: if the login
// on the board is the one this image was built from, the board's (possibly
// rotated) tokens are kept; otherwise the image's tokens replace them.
static void applyCompiledConfig() {
  // Before settings lived in NVS, accounts were only indexed; the build that
  // seeded them used the same order as this one.
  Settings old = cfg;
  bool legacy = !prefs.isKey("n");
  if (legacy) {
    old.nAccounts = min(ACCOUNT_COUNT, MAX_ACCOUNTS);
    for (int i = 0; i < old.nAccounts; i++) {
      strlcpy(old.acct[i].alias, ACCT_LABEL[i], sizeof(old.acct[i].alias));
      getStr(key("seed", i).c_str(), old.acct[i].seed, sizeof(old.acct[i].seed));
      old.acct[i].access    = prefs.getString(key("at", i).c_str(), "");
      old.acct[i].refresh   = prefs.getString(key("rt", i).c_str(), "");
      old.acct[i].expiresMs = prefs.getULong64(key("ex", i).c_str(), 0);
    }
  }

  bool configChanged = prefs.getString("cfgseed", "") != CONFIG_SEED;
  if (configChanged) {
    strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid));
    strlcpy(cfg.pass, WIFI_PASS, sizeof(cfg.pass));
    strlcpy(cfg.hostname, DEVICE_HOSTNAME, sizeof(cfg.hostname));
    strlcpy(cfg.tz, TZ_POSIX, sizeof(cfg.tz));
    strlcpy(cfg.refreshScope, REFRESH_SCOPE, sizeof(cfg.refreshScope));
    cfg.hosting     = ENABLE_HOSTING;
    cfg.pageSeconds    = PAGE_SECONDS;
    cfg.refreshMinutes = REFRESH_MINUTES;
    cfg.brightness     = BRIGHTNESS;
    cfg.sleepMinutes   = SLEEP_MINUTES;
    cfg.dimMinutes     = DIM_MINUTES;
    cfg.nAccounts   = min(ACCOUNT_COUNT, MAX_ACCOUNTS);
  }

  for (int i = 0; i < min(ACCOUNT_COUNT, MAX_ACCOUNTS); i++) {
    int j = configChanged ? i : settingsFindAccount(ACCT_LABEL[i]);
    if (j < 0) continue;   // removed on the website since; leave it removed
    AccountCfg &a = cfg.acct[j];

    const AccountCfg *prev = nullptr;
    for (int k = 0; k < old.nAccounts; k++)
      if (!strcmp(old.acct[k].alias, ACCT_LABEL[i])) prev = &old.acct[k];

    if (configChanged) {
      strlcpy(a.alias, ACCT_LABEL[i], sizeof(a.alias));
      strlcpy(a.email, ACCT_EMAIL[i], sizeof(a.email));
      strlcpy(a.plan, ACCT_PLAN[i], sizeof(a.plan));
      strlcpy(a.models, ACCT_MODELS[i], sizeof(a.models));
      strlcpy(a.scope, ACCT_SCOPE[i], sizeof(a.scope));
    }
    if (prev && !strcmp(prev->seed, ACCT_SEED[i])) {
      a.access = prev->access; a.refresh = prev->refresh; a.expiresMs = prev->expiresMs;
      strlcpy(a.seed, prev->seed, sizeof(a.seed));
    } else {
      // Expire at once: the first check refreshes, narrowing the login to its
      // scope and retiring the broader tokens baked into this image.
      a.access = ACCT_ACCESS[i]; a.refresh = ACCT_REFRESH[i]; a.expiresMs = 0;
      strlcpy(a.seed, ACCT_SEED[i], sizeof(a.seed));
      Serial.printf("[settings] %s: new login in this flash\n", a.alias);
    }
  }

  settingsSave();
  prefs.putString("cfgseed", CONFIG_SEED);
  Serial.printf("[settings] script config %s\n", configChanged ? "applied" : "unchanged, kept NVS");
}
#endif

void settingsLoad() {
  // Namespace predates the project rename; see the note on key names above.
  prefs.begin("claudecounts", false);
  loadNvs();
#ifdef HAVE_SECRETS
  applyCompiledConfig();
#endif
  Serial.printf("[settings] wifi '%s', host %s, hosting %s, %d account(s)\n", cfg.ssid,
                cfg.hostname, cfg.hosting ? "on" : "off", cfg.nAccounts);
}
