// Network layer: WiFi, clock, OAuth token refresh, and the usage endpoint.
//
// Each account's tokens are seeded from secrets.h only when that account's
// login changed since the last flash (ACCT_SEED differs). Otherwise the
// tokens in NVS win, because the device rotates its refresh token on every
// refresh and the copy baked into the firmware is already spent.
// Nothing in this file ever logs a token value.

#include "model.h"

#ifndef DEMO_DATA

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "certs.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing: run ./build.sh or ./deploy.sh, which generate it from the device logins"
#endif

static const char* USAGE_URL  = "https://api.anthropic.com/api/oauth/usage";
static const char* TOKEN_URL  = "https://platform.claude.com/v1/oauth/token";
static const char* USER_AGENT = "claude-status/1.0 (esp32)";

static const uint64_t EXPIRY_MARGIN_MS = 5ULL * 60ULL * 1000ULL;
static const int      WIFI_ATTEMPTS    = 3;
static const uint32_t WIFI_ATTEMPT_MS  = 12000;

struct Token {
  String   access;
  String   refresh;
  uint64_t expiresMs;
};

static Preferences prefs;
static Token       tokens[MAX_ACCOUNTS];
static bool        clockConfigured = false;

static String nvsKey(const char* base, int i) { return String(base) + i; }
static uint64_t nowMs() { return (uint64_t)time(nullptr) * 1000ULL; }

static void saveToken(int i) {
  prefs.putString(nvsKey("at", i).c_str(), tokens[i].access);
  prefs.putString(nvsKey("rt", i).c_str(), tokens[i].refresh);
  prefs.putULong64(nvsKey("ex", i).c_str(), tokens[i].expiresMs);
}

void apiInit() {
  strlcpy(wifiLink.ssid, WIFI_SSID, sizeof(wifiLink.ssid));
  // Namespace predates the project rename. A board's only live tokens are
  // stored under it, so changing it would strand every deployed login.
  prefs.begin("claudecounts", false);
  accountCount = min(ACCOUNT_COUNT, MAX_ACCOUNTS);

  for (int i = 0; i < accountCount; i++) {
    Account &a = accounts[i];
    strlcpy(a.label, ACCT_LABEL[i], sizeof(a.label));
    strlcpy(a.plan, ACCT_PLAN[i], sizeof(a.plan));

    String seedKey = nvsKey("seed", i);
    if (prefs.getString(seedKey.c_str(), "") != ACCT_SEED[i]) {
      // Expire the seeded access token at once: the first check refreshes,
      // which narrows the login to ACCT_SCOPE and retires the broader
      // tokens baked into this firmware image.
      tokens[i].access    = ACCT_ACCESS[i];
      tokens[i].refresh   = ACCT_REFRESH[i];
      tokens[i].expiresMs = 0;
      saveToken(i);
      prefs.putString(seedKey.c_str(), ACCT_SEED[i]);
      Serial.printf("[auth] %s: new login in this flash, seeded NVS\n", a.label);
    } else {
      tokens[i].access    = prefs.getString(nvsKey("at", i).c_str(), "");
      tokens[i].refresh   = prefs.getString(nvsKey("rt", i).c_str(), "");
      tokens[i].expiresMs = prefs.getULong64(nvsKey("ex", i).c_str(), 0);
      Serial.printf("[auth] %s: same login as before, kept NVS tokens\n", a.label);
    }
  }
}

// ---------------------------------------------------------------- WiFi + clock

static bool wifiUp(String &err) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.persistent(false);
  wl_status_t st = WL_IDLE_STATUS;

  for (int attempt = 1; attempt <= WIFI_ATTEMPTS; attempt++) {
    // The hostname is applied when the STA interface starts, so set it while off.
    WiFi.mode(WIFI_OFF);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t start = millis();
    while ((st = WiFi.status()) != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      if (st == WL_NO_SSID_AVAIL && millis() - start > 6000) break;
      delay(100);
    }
    if (st == WL_CONNECTED) {
      WiFi.setAutoReconnect(true);
      Serial.printf("[wifi] %s connected as %s (attempt %d), ip %s, rssi %d\n", WIFI_SSID,
                    DEVICE_HOSTNAME, attempt, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.printf("[wifi] attempt %d/%d failed (status %d)\n", attempt, WIFI_ATTEMPTS, (int)st);
    WiFi.disconnect(true);
    delay(400);
  }

  if (st == WL_NO_SSID_AVAIL)       err = String(WIFI_SSID) + " not in range";
  else if (st == WL_CONNECT_FAILED) err = String("Wrong password for ") + WIFI_SSID;
  else                              err = String("Can't join ") + WIFI_SSID;
  return false;
}

void apiPollLink() {
  wifiLink.up = WiFi.status() == WL_CONNECTED;
  if (wifiLink.up) wifiLink.rssi = WiFi.RSSI();
}

// TLS certificate checks need a real clock, and reset times are shown locally.
static bool clockUp() {
  if (!clockConfigured) {
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    clockConfigured = true;
  }
  uint32_t start = millis();
  while (time(nullptr) < 1700000000 && millis() - start < 15000) delay(100);
  return time(nullptr) >= 1700000000;
}

// ---------------------------------------------------------------- parsing

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// "2026-09-21T18:30:00.548316+00:00" -> UTC epoch seconds.
static time_t parseIso8601(const char *s) {
  if (!s) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int64_t t = daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
  const char *tz = strpbrk(s + 19, "+-Z");
  if (tz && *tz != 'Z') {
    int oh = 0, om = 0;
    sscanf(tz + 1, "%2d:%2d", &oh, &om);
    int off = oh * 3600 + om * 60;
    t -= (*tz == '+') ? off : -off;
  }
  return (time_t)t;
}

static bool containsNoCase(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++)
    if (!strncasecmp(hay, needle, n)) return true;
  return false;
}

struct Meter {
  char   name[24];
  float  pct;
  time_t resets;
};

static void setBucket(Bucket &b, const char *label, const Meter &m, bool shared) {
  strlcpy(b.label, label, sizeof(b.label));
  b.util     = constrain(m.pct, 0.0f, 100.0f);
  b.resetsAt = m.resets;
  b.shared   = shared;
}

// The `limits` array is the server's own description of every meter: kind
// "session", "weekly_all", or "weekly_scoped" with scope.model.display_name.
// Rows are classified by kind, never by label, as the CLI itself does.
static bool parseUsage(const String &body, int acct, Account &a, String &err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, body);
  if (e) { err = String("Bad JSON: ") + e.c_str(); return false; }

  Meter session = {}, weekly = {}, scoped[MAX_BUCKETS];
  bool haveSession = false, haveWeekly = false;
  int ns = 0;

  JsonArrayConst limits = doc["limits"];
  for (JsonObjectConst r : limits) {
    const char *kind  = r["kind"] | "";
    const char *model = r["scope"]["model"]["display_name"].as<const char *>();
    Meter m = {};
    m.pct    = r["percent"] | 0.0f;
    m.resets = parseIso8601(r["resets_at"].as<const char *>());
    Serial.printf("[usage]   %s: kind=%s model=%s %.0f%%\n", a.label, kind,
                  model ? model : "-", m.pct);

    if (!strcmp(kind, "session"))         { session = m; haveSession = true; }
    else if (!strcmp(kind, "weekly_all")) { weekly = m;  haveWeekly = true; }
    else if (model && ns < MAX_BUCKETS)   { strlcpy(m.name, model, sizeof(m.name)); scoped[ns++] = m; }
  }

  // Older responses carry only the top-level windows.
  if (limits.isNull()) {
    if (doc["five_hour"]["utilization"].is<float>()) {
      session.pct    = doc["five_hour"]["utilization"];
      session.resets = parseIso8601(doc["five_hour"]["resets_at"].as<const char *>());
      haveSession = true;
    }
    if (doc["seven_day"]["utilization"].is<float>()) {
      weekly.pct    = doc["seven_day"]["utilization"];
      weekly.resets = parseIso8601(doc["seven_day"]["resets_at"].as<const char *>());
      haveWeekly = true;
    }
  }
  if (!haveSession && !haveWeekly && !ns) { err = "No usage limits returned"; return false; }

  Bucket out[MAX_BUCKETS];
  int n = 0;
  if (haveSession) setBucket(out[n++], "Session", session, false);

  // Show the models configured for this account. A model without a limit of
  // its own draws on the all-models weekly limit, so that is what it shows.
  String wanted = ACCT_MODELS[acct];
  if (wanted.length() == 0) {
    if (haveWeekly) setBucket(out[n++], "Weekly", weekly, false);
    for (int s = 0; s < ns && n < MAX_BUCKETS; s++) setBucket(out[n++], scoped[s].name, scoped[s], false);
  } else {
    int from = 0;
    while (from <= (int)wanted.length() && n < MAX_BUCKETS) {
      int comma = wanted.indexOf(',', from);
      if (comma < 0) comma = wanted.length();
      String name = wanted.substring(from, comma);
      name.trim();
      from = comma + 1;
      if (!name.length()) continue;

      int hit = -1;
      for (int s = 0; s < ns; s++)
        if (containsNoCase(scoped[s].name, name.c_str())) { hit = s; break; }
      if (hit >= 0)        setBucket(out[n++], scoped[hit].name, scoped[hit], false);
      else if (haveWeekly) setBucket(out[n++], name.c_str(), weekly, true);
    }
  }

  memcpy(a.buckets, out, sizeof(Bucket) * n);
  a.nBuckets = n;
  return true;
}

// ---------------------------------------------------------------- HTTP

static String httpError(int code) {
  if (code > 0) return String("HTTP ") + code;
  return HTTPClient::errorToString(code);
}

static bool refreshToken(int i, String &err) {
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CAS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, TOKEN_URL)) { err = "Token URL rejected"; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", USER_AGENT);

  String body;
  {
    JsonDocument req;
    req["grant_type"]    = "refresh_token";
    req["refresh_token"] = tokens[i].refresh;
    req["client_id"]     = OAUTH_CLIENT_ID;
    req["scope"]         = ACCT_SCOPE[i];
    serializeJson(req, body);
  }
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : String();
  http.end();

  if (code != 200) {
    // Error bodies carry no tokens; a short excerpt helps diagnose rejections.
    Serial.printf("[auth] %s refresh failed: %s %.120s\n", accounts[i].label,
                  httpError(code).c_str(), resp.c_str());
    err = (code == 400 || code == 401) ? "Login expired: ./login.sh" : "Refresh " + httpError(code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp) || !doc["access_token"].is<const char *>()) {
    err = "Refresh reply unreadable";
    return false;
  }

  // Field names only, never values: shows what the server sends back, e.g.
  // whether it reports a refresh-token lifetime.
  String fields;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) { fields += ' '; fields += kv.key().c_str(); }
  Serial.printf("[auth] %s refresh reply fields:%s\n", accounts[i].label, fields.c_str());

  tokens[i].access = doc["access_token"].as<const char *>();
  if (doc["refresh_token"].is<const char *>()) {
    String rt = doc["refresh_token"].as<const char *>();
    if (rt != tokens[i].refresh) {
      tokens[i].refresh = rt;
      Serial.printf("[auth] %s refresh token rotated, stored new one\n", accounts[i].label);
    }
  }
  uint32_t ttl = doc["expires_in"] | 3600;
  tokens[i].expiresMs = nowMs() + (uint64_t)ttl * 1000ULL;
  saveToken(i);
  Serial.printf("[auth] %s access token refreshed, valid %lu min, scope '%s'\n", accounts[i].label,
                (unsigned long)(ttl / 60), doc["scope"] | "?");

  // Whether this lifetime resets on each refresh or counts down to a fixed
  // end decides how long the device runs without a new login.
  if (doc["refresh_token_expires_in"].is<uint32_t>()) {
    uint32_t rtl = doc["refresh_token_expires_in"];
    Serial.printf("[auth] %s refresh token valid %.1f days\n", accounts[i].label, rtl / 86400.0f);
  }
  return true;
}

static int getUsage(int i, String &body) {
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CAS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, USAGE_URL)) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.addHeader("Authorization", "Bearer " + tokens[i].access);
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  http.addHeader("User-Agent", USER_AGENT);
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

static void fail(Account &a, const String &err) {
  a.ok = false;
  strlcpy(a.error, err.c_str(), sizeof(a.error));
}

static void checkAccount(int i) {
  Account &a = accounts[i];
  String err, body;

  if (tokens[i].access.isEmpty() || nowMs() + EXPIRY_MARGIN_MS >= tokens[i].expiresMs) {
    if (!refreshToken(i, err)) { fail(a, err); return; }
  }

  int code = getUsage(i, body);
  if (code == 401) {                          // revoked early: one refresh, one retry
    if (!refreshToken(i, err)) { fail(a, err); return; }
    code = getUsage(i, body);
  }
  if (code != 200) {
    Serial.printf("[usage] %s: %s\n", a.label, httpError(code).c_str());
    fail(a, "Usage " + httpError(code));
    return;
  }
  if (!parseUsage(body, i, a, err)) { fail(a, err); return; }

  a.ok = true;
  a.everOk = true;
  a.error[0] = 0;
  a.fetchedAt = time(nullptr);
  Serial.printf("[usage] %s:", a.label);
  for (int b = 0; b < a.nBuckets; b++)
    Serial.printf(" %s=%.0f%%%s", a.buckets[b].label, a.buckets[b].util,
                  a.buckets[b].shared ? "(shared)" : "");
  Serial.println();
}

void apiCheckAll() {
  netStatus = NET_CHECKING;
  uiDrawStatus();

  String err;
  networkDown = !wifiUp(err);
  if (!networkDown && !clockUp()) { err = "No network time (NTP)"; networkDown = true; }
  apiPollLink();
  if (networkDown) {
    for (int i = 0; i < accountCount; i++) fail(accounts[i], err);
    netStatus = NET_FAILED;
    return;
  }

  int good = 0;
  for (int i = 0; i < accountCount; i++) {
    checkAccount(i);
    if (accounts[i].ok) good++;
  }
  apiPollLink();

  if (good) lastGoodCheck = time(nullptr);
  netStatus = good == accountCount ? NET_OK : good ? NET_PARTIAL : NET_FAILED;
  Serial.printf("[check] %d/%d accounts ok, free heap %u\n", good, accountCount,
                (unsigned)ESP.getFreeHeap());
}

#else  // DEMO_DATA ------------------------------------------------------------

// Offline stand-in shaped like the real display. DEMO_ACCOUNTS (1-4) picks how
// many accounts to fake, to preview the full-screen, side-by-side and
// scrolling layouts. Values change on every other check.
#include <sys/time.h>

#ifndef DEMO_ACCOUNTS
#define DEMO_ACCOUNTS 2
#endif

static int demoTick = 0;

static void demoBucket(Account &a, const char *label, float util, time_t resetsAt, bool shared) {
  Bucket &b = a.buckets[a.nBuckets++];
  strlcpy(b.label, label, sizeof(b.label));
  b.util = util;
  b.resetsAt = resetsAt;
  b.shared = shared;
}

void apiInit() {
  setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
  tzset();
  struct timeval tv = { 1790000000, 0 };
  settimeofday(&tv, nullptr);

  static const char *LABELS[] = {"personal", "work", "team", "lab"};
  static const char *PLANS[]  = {"PRO", "MAX 5x", "MAX 20x", "PRO"};
  accountCount = constrain(DEMO_ACCOUNTS, 1, MAX_ACCOUNTS);
  for (int i = 0; i < accountCount; i++) {
    strlcpy(accounts[i].label, LABELS[i], sizeof(accounts[i].label));
    strlcpy(accounts[i].plan, PLANS[i], sizeof(accounts[i].plan));
  }
  strlcpy(wifiLink.ssid, "DEMO - not real data", sizeof(wifiLink.ssid));
  wifiLink.rssi = -52;
  wifiLink.up = true;
  Serial.printf("[demo] %d account(s), offline demo data, no network\n", accountCount);
}

void apiPollLink() {}

void apiCheckAll() {
  netStatus = NET_CHECKING;
  uiDrawStatus();
  delay(1200);

  int step = demoTick / 2;
  static time_t base = time(nullptr);   // anchor so resets don't drift per check
  const time_t week = base + 2 * 86400 + 11 * 3600;

  for (int i = 0; i < accountCount; i++) {
    Account &a = accounts[i];
    a.nBuckets = 0;
    demoBucket(a, "Session", fmodf(25 + i * 23 + step * 17, 100), base + (47 + i * 70) * 60 + step * 600, false);
    demoBucket(a, "Opus", fminf(71 - i * 20 + step * 4, 100), week, i != 2);
    if (i >= 1) demoBucket(a, "Fable",  fminf(9 + i * 30 + step * 2, 100), week + 86400, false);
    if (i == 2) demoBucket(a, "Sonnet", fminf(44 + step * 3, 100), week, false);
    a.ok = a.everOk = true;
    a.error[0] = 0;
    a.fetchedAt = time(nullptr);
  }
  networkDown = false;
  lastGoodCheck = time(nullptr);
  netStatus = NET_OK;
  demoTick++;
  Serial.printf("[demo] check %d, step %d\n", demoTick, step);
}

#endif
