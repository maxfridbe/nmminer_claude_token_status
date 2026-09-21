// Network layer: WiFi, clock, OAuth (sign-in, refresh), and the usage endpoint.
//
// Accounts and tokens live in the settings (settings.cpp). The board refreshes
// its own tokens and stores each rotated refresh token, so its logins must
// never be shared with another client. Nothing here ever logs a token value.

#include "model.h"
#include "settings.h"

#ifndef DEMO_DATA

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>
#include "certs.h"

static const char* USAGE_URL     = "https://api.anthropic.com/api/oauth/usage";
static const char* PROFILE_URL   = "https://api.anthropic.com/api/oauth/profile";
static const char* TOKEN_URL     = "https://platform.claude.com/v1/oauth/token";
static const char* AUTHORIZE_URL = "https://claude.com/cai/oauth/authorize";
static const char* REDIRECT_URL  = "https://platform.claude.com/oauth/code/callback";
static const char* CLIENT_ID     = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";  // Claude Code's
static const char* USER_AGENT    = "claude-status/1.0 (esp32)";

static const uint64_t EXPIRY_MARGIN_MS = 5ULL * 60ULL * 1000ULL;
static const int      WIFI_ATTEMPTS    = 3;
static const uint32_t WIFI_ATTEMPT_MS  = 12000;

static bool clockConfigured = false;
static bool mdnsUp = false;

static uint64_t nowMs() { return (uint64_t)time(nullptr) * 1000ULL; }

// Mirror the settings' accounts into the display model.
void apiSyncAccounts() {
  accountCount = cfg.nAccounts;
  for (int i = 0; i < MAX_ACCOUNTS; i++) {
    Account &a = accounts[i];
    if (i >= cfg.nAccounts) { a = Account(); continue; }
    if (strcmp(a.label, cfg.acct[i].alias)) {   // new or moved: drop stale numbers
      a = Account();
      strlcpy(a.label, cfg.acct[i].alias, sizeof(a.label));
    }
    strlcpy(a.plan, cfg.acct[i].plan, sizeof(a.plan));
  }
}

void apiInit() {
  strlcpy(wifiLink.ssid, cfg.ssid, sizeof(wifiLink.ssid));
  apiSyncAccounts();
}

// ---------------------------------------------------------------- WiFi + clock

static bool wifiUp(String &err) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.persistent(false);
  wl_status_t st = WL_IDLE_STATUS;

  for (int attempt = 1; attempt <= WIFI_ATTEMPTS; attempt++) {
    // The hostname is applied when the STA interface starts, so set it while off.
    WiFi.mode(WIFI_OFF);
    WiFi.setHostname(cfg.hostname);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);

    uint32_t start = millis();
    while ((st = WiFi.status()) != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      if (st == WL_NO_SSID_AVAIL && millis() - start > 6000) break;
      delay(100);
    }
    if (st == WL_CONNECTED) {
      WiFi.setAutoReconnect(true);
      if (!mdnsUp && MDNS.begin(cfg.hostname)) {
        MDNS.addService("http", "tcp", 80);
        mdnsUp = true;
      }
      Serial.printf("[wifi] %s connected as %s (attempt %d), ip %s, rssi %d\n", cfg.ssid,
                    cfg.hostname, attempt, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.printf("[wifi] attempt %d/%d failed (status %d)\n", attempt, WIFI_ATTEMPTS, (int)st);
    WiFi.disconnect(true);
    delay(400);
  }

  if (st == WL_NO_SSID_AVAIL)       err = String(cfg.ssid) + " not in range";
  else if (st == WL_CONNECT_FAILED) err = String("Wrong password for ") + cfg.ssid;
  else                              err = String("Can't join ") + cfg.ssid;
  return false;
}

bool apiWifiUp() {
  String err;
  return wifiUp(err);
}

void apiPollLink() {
  wifiLink.up = WiFi.status() == WL_CONNECTED;
  if (wifiLink.up) wifiLink.rssi = WiFi.RSSI();
}

// TLS certificate checks need a real clock, and reset times are shown locally.
static bool clockUp() {
  if (!clockConfigured) {
    configTzTime(cfg.tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
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
  String wanted = cfg.acct[acct].models;
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

// POST JSON to the token endpoint; returns the HTTP status and the reply.
static int postToken(JsonDocument &req, String &resp) {
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CAS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, TOKEN_URL)) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", USER_AGENT);
  String body;
  serializeJson(req, body);
  int code = http.POST(body);
  resp = (code > 0) ? http.getString() : String();
  http.end();
  return code;
}

static int apiGet(const char *url, const String &token, String &body) {
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CAS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  http.addHeader("User-Agent", USER_AGENT);
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

// Store a token reply (from a refresh or a sign-in) on account i.
static void takeTokens(int i, JsonDocument &doc) {
  AccountCfg &a = cfg.acct[i];
  a.access = doc["access_token"].as<const char *>();
  if (doc["refresh_token"].is<const char *>()) {
    String rt = doc["refresh_token"].as<const char *>();
    if (rt != a.refresh && a.refresh.length())
      Serial.printf("[auth] %s refresh token rotated, stored new one\n", a.alias);
    a.refresh = rt;
  }
  uint32_t ttl = doc["expires_in"] | 3600;
  a.expiresMs = nowMs() + (uint64_t)ttl * 1000ULL;
  settingsSaveTokens(i);
  Serial.printf("[auth] %s access token valid %lu min, scope '%s'\n", a.alias,
                (unsigned long)(ttl / 60), doc["scope"] | "?");
  // Whether this lifetime resets on each refresh or counts down to a fixed
  // end decides how long the board runs without a new sign-in.
  if (doc["refresh_token_expires_in"].is<uint32_t>())
    Serial.printf("[auth] %s refresh token valid %.1f days\n", a.alias,
                  doc["refresh_token_expires_in"].as<uint32_t>() / 86400.0f);
}

static bool refreshToken(int i, String &err) {
  JsonDocument req;
  req["grant_type"]    = "refresh_token";
  req["refresh_token"] = cfg.acct[i].refresh;
  req["client_id"]     = CLIENT_ID;
  req["scope"]         = cfg.acct[i].scope;
  String resp;
  int code = postToken(req, resp);

  if (code != 200) {
    // Error bodies carry no tokens; a short excerpt helps diagnose rejections.
    Serial.printf("[auth] %s refresh failed: %s %.120s\n", cfg.acct[i].alias,
                  httpError(code).c_str(), resp.c_str());
    err = (code == 400 || code == 401) ? "Login expired: sign in again" : "Refresh " + httpError(code);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp) || !doc["access_token"].is<const char *>()) {
    err = "Refresh reply unreadable";
    return false;
  }
  takeTokens(i, doc);
  return true;
}

static void fail(Account &a, const String &err) {
  a.ok = false;
  strlcpy(a.error, err.c_str(), sizeof(a.error));
}

static void checkAccount(int i) {
  Account &a = accounts[i];
  AccountCfg &c = cfg.acct[i];
  String err, body;

  if (c.access.isEmpty() || nowMs() + EXPIRY_MARGIN_MS >= c.expiresMs) {
    if (!refreshToken(i, err)) { fail(a, err); return; }
  }

  int code = apiGet(USAGE_URL, c.access, body);
  if (code == 401) {                          // revoked early: one refresh, one retry
    if (!refreshToken(i, err)) { fail(a, err); return; }
    code = apiGet(USAGE_URL, c.access, body);
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

// ---------------------------------------------------------------- sign-in

// Claude Code's copy-paste sign-in for machines without a browser: the user
// approves on claude.ai, which then shows a code to paste back here.
struct Pending {
  bool     active;
  String   url;
  char     alias[24];
  char     models[48];
  char     verifier[64];
  char     state[64];
  uint32_t startedMs;
};
static Pending pending;
static void clearPending() { pending = Pending(); }

static void base64url(const uint8_t *in, size_t n, char *out) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = A[(v >> 18) & 63];
    out[o++] = A[(v >> 12) & 63];
    if (i + 1 < n) out[o++] = A[(v >> 6) & 63];
    if (i + 2 < n) out[o++] = A[v & 63];
  }
  out[o] = 0;
}

static void randomB64(char *out, size_t bytes) {
  uint8_t buf[48];
  esp_fill_random(buf, bytes);
  base64url(buf, bytes, out);
}

static String urlEncode(const char *s) {
  String o;
  for (; *s; s++) {
    if (isalnum((unsigned char)*s) || strchr("-_.~", *s)) o += *s;
    else { char h[4]; snprintf(h, sizeof(h), "%%%02X", (unsigned char)*s); o += h; }
  }
  return o;
}

String apiSigninStart(const char *alias, const char *models) {
  clearPending();
  pending.active = true;
  strlcpy(pending.alias, alias, sizeof(pending.alias));
  strlcpy(pending.models, models, sizeof(pending.models));
  randomB64(pending.verifier, 32);
  randomB64(pending.state, 32);
  pending.startedMs = millis();

  uint8_t digest[32];
  mbedtls_sha256((const uint8_t *)pending.verifier, strlen(pending.verifier), digest, 0);
  char challenge[48];
  base64url(digest, 32, challenge);

  pending.url = String(AUTHORIZE_URL) + "?code=true&client_id=" + CLIENT_ID +
                "&response_type=code&redirect_uri=" + urlEncode(REDIRECT_URL) +
                "&scope=" + urlEncode(cfg.refreshScope) + "&code_challenge=" + challenge +
                "&code_challenge_method=S256&state=" + pending.state;
  return pending.url;
}

// A sign-in in progress, so the page can pick it up again after the phone
// left the hotspot to reach claude.ai.
bool apiSigninPending(String &alias, String &url) {
  if (!pending.active || millis() - pending.startedMs > 15UL * 60UL * 1000UL) return false;
  alias = pending.alias;
  url = pending.url;
  return true;
}

// "claude_max" + "default_claude_max_5x" -> "MAX 5x"
static void planFromProfile(JsonDocument &p, char *out, size_t n) {
  String type = p["organization"]["organization_type"] | "";
  String tier = p["organization"]["rate_limit_tier"] | "";
  if (type.indexOf("max") >= 0) {
    snprintf(out, n, "MAX%s", tier.indexOf("20x") >= 0 ? " 20x" : tier.indexOf("5x") >= 0 ? " 5x" : "");
  } else if (type.length()) {
    type.replace("claude_", "");
    type.toUpperCase();
    strlcpy(out, type.c_str(), n);
  } else {
    strlcpy(out, "?", n);
  }
}

bool apiSigninFinish(String pasted, String &err, String &email) {
  if (!pending.active || millis() - pending.startedMs > 15UL * 60UL * 1000UL) {
    err = "That sign-in expired; start again";
    return false;
  }
  pasted.trim();
  String code = pasted, state = pending.state;
  int hash = pasted.indexOf('#');                 // claude.ai shows "code#state"
  if (hash >= 0) {
    code = pasted.substring(0, hash);
    if (pasted.substring(hash + 1) != pending.state) { err = "That code is from a different sign-in"; return false; }
  }
  if (!code.length()) { err = "Paste the code shown after approving"; return false; }
  if (!apiWifiUp() || !clockUp()) { err = "No network"; return false; }

  JsonDocument req;
  req["grant_type"]    = "authorization_code";
  req["code"]          = code;
  req["redirect_uri"]  = REDIRECT_URL;
  req["client_id"]     = CLIENT_ID;
  req["code_verifier"] = pending.verifier;
  req["state"]         = state;
  String resp;
  int status = postToken(req, resp);
  if (status != 200) {
    Serial.printf("[signin] exchange failed: %s %.160s\n", httpError(status).c_str(), resp.c_str());
    err = status == 400 ? "Claude rejected the code; start again" : "Sign-in " + httpError(status);
    return false;
  }
  JsonDocument tok;
  if (deserializeJson(tok, resp) || !tok["access_token"].is<const char *>()) {
    err = "Sign-in reply unreadable";
    return false;
  }

  // Slot: replace an account with this alias, or add one.
  int i = settingsFindAccount(pending.alias);
  if (i < 0) {
    if (cfg.nAccounts >= MAX_ACCOUNTS) { err = "Already at the maximum of 4 accounts"; return false; }
    i = cfg.nAccounts++;
    cfg.acct[i] = AccountCfg();
  }
  AccountCfg &a = cfg.acct[i];
  strlcpy(a.alias, pending.alias, sizeof(a.alias));
  strlcpy(a.models, pending.models, sizeof(a.models));
  strlcpy(a.scope, cfg.refreshScope, sizeof(a.scope));
  strlcpy(a.seed, "web", sizeof(a.seed));
  a.refresh = "";
  takeTokens(i, tok);

  // Who signed in, and on which plan.
  String body;
  if (apiGet(PROFILE_URL, a.access, body) == 200) {
    JsonDocument prof;
    if (!deserializeJson(prof, body)) {
      strlcpy(a.email, prof["account"]["email"] | "", sizeof(a.email));
      planFromProfile(prof, a.plan, sizeof(a.plan));
    }
  }
  settingsSave();
  apiSyncAccounts();
  clearPending();
  email = a.email;
  Serial.printf("[signin] %s signed in as %s (%s)\n", a.alias, a.email, a.plan);
  return true;
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
void apiSyncAccounts() {}
bool apiWifiUp() { return false; }
String apiSigninStart(const char *, const char *) { return ""; }
bool apiSigninPending(String &, String &) { return false; }
bool apiSigninFinish(String, String &err, String &) { err = "Demo mode"; return false; }

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
