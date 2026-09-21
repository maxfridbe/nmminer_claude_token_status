#include "web.h"
#include "web_page.h"
#include "model.h"
#include "settings.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_random.h>

#define PIN_SECONDS   120
#define PIN_TRIES     5
#define HOTSPOT_IDLE_MS (15UL * 60UL * 1000UL)

static WebServer server(80);
static DNSServer dns;
static bool active = false, apMode = false, lanUp = false, hotspotUp = false, routesUp = false;
static uint32_t hotspotSeen;
static char apSsid[33], apPass[12];
static char session[33];                 // the one signed-in browser, "" = none
static char pin[7];
static uint32_t pinUntil, rebootAt;
static int pinTries;

bool webWantsCheck = false;

bool webActive()          { return active; }
bool webSetupMode()       { return apMode; }
const char *webApSsid()   { return apSsid; }
const char *webApPass()   { return apPass; }
bool webHotspotUp()       { return hotspotUp; }
String webHotspotUrl()    { return "http://" + WiFi.softAPIP().toString(); }
String webUrl() {
  if (lanUp) return "http://" + WiFi.localIP().toString();
  if (hotspotUp || apMode) return webHotspotUrl();
  return "";
}

static bool fromHotspot() {
  return (hotspotUp || apMode) && server.client().localIP() == WiFi.softAPIP();
}

static void randomHex(char *out, int bytes) {
  for (int i = 0; i < bytes; i++) sprintf(out + i * 2, "%02x", (uint8_t)esp_random());
}

// On the hotspot, anyone connected scanned the QR on the screen. On the LAN,
// the browser must hold the session cookie that a correct PIN hands out.
static bool authed() {
  if (fromHotspot()) { hotspotSeen = millis(); return true; }
  if (!session[0]) return false;
  String c = server.header("Cookie");
  return c.indexOf(String("cs=") + session) >= 0;
}

static void sendJson(JsonDocument &d, int code = 200) {
  String out;
  serializeJson(d, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

static void sendError(int code, const char *msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(d, code);
}

static bool body(JsonDocument &d) {
  if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Bad request"); return false; }
  return true;
}

static bool requireAuth() {
  if (authed()) return true;
  sendError(401, "Enter the PIN from the display first");
  return false;
}

static void scheduleReboot() { rebootAt = millis() + 1500; }

static bool validAlias(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 23) return false;
  for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("_.-", *s)) return false;
  return true;
}

// ---------------------------------------------------------------- handlers

static void handleState() {
  JsonDocument d;
  d["mode"]        = apMode ? "setup" : "lan";
  d["via"]         = fromHotspot() ? "hotspot" : "lan";
  d["hotspot"]     = hotspotUp ? webApSsid() : "";
  d["authed"]      = authed();
  d["hostname"]    = cfg.hostname;
  d["ssid"]        = cfg.ssid;
  d["url"]         = webUrl();
  d["mdns"]        = String("http://") + cfg.hostname + ".local";
  d["hosting"]     = cfg.hosting;
  d["pageSeconds"]    = cfg.pageSeconds;
  d["refreshMinutes"] = cfg.refreshMinutes;
  d["brightness"]     = cfg.brightness;
  d["sleepMinutes"]   = cfg.sleepMinutes;
  d["dimMinutes"]     = cfg.dimMinutes;
  d["maxAccounts"] = MAX_ACCOUNTS;
  d["pinPending"]  = pin[0] && millis() < pinUntil;
  if (authed()) {
    String pAlias, pUrl;
    if (apiSigninPending(pAlias, pUrl)) { d["pending"]["alias"] = pAlias; d["pending"]["url"] = pUrl; }
    JsonArray list = d["accounts"].to<JsonArray>();
    for (int i = 0; i < cfg.nAccounts; i++) {
      JsonObject a = list.add<JsonObject>();
      a["alias"]  = cfg.acct[i].alias;
      a["email"]  = cfg.acct[i].email;
      a["plan"]   = cfg.acct[i].plan;
      a["models"] = cfg.acct[i].models;
      a["ok"]     = accounts[i].ok;
      a["error"]  = accounts[i].error;
      JsonArray m = a["meters"].to<JsonArray>();
      for (int b = 0; b < accounts[i].nBuckets; b++) {
        JsonObject o = m.add<JsonObject>();
        o["label"]  = accounts[i].buckets[b].label;
        o["pct"]    = (int)(accounts[i].buckets[b].util + 0.5f);
        o["shared"] = accounts[i].buckets[b].shared;
      }
    }
  }
  sendJson(d);
}

static void handlePinRequest() {
  if (fromHotspot()) { sendError(400, "No PIN needed on the board's hotspot"); return; }
  snprintf(pin, sizeof(pin), "%06u", (unsigned)(esp_random() % 1000000));
  pinUntil = millis() + PIN_SECONDS * 1000UL;
  pinTries = 0;
  uiShowPin(pin);
  Serial.println("[web] PIN shown on display");
  JsonDocument d;
  d["seconds"] = PIN_SECONDS;
  sendJson(d);
}

static void handlePinVerify() {
  JsonDocument in;
  if (!body(in)) return;
  const char *given = in["pin"] | "";
  if (!pin[0] || millis() > pinUntil) { sendError(400, "No PIN is showing; request a new one"); return; }
  if (strcmp(given, pin) != 0) {
    if (++pinTries >= PIN_TRIES) { pin[0] = 0; uiHidePin(); }
    sendError(403, pinTries >= PIN_TRIES ? "Too many tries; request a new PIN" : "Wrong PIN");
    return;
  }
  pin[0] = 0;
  uiHidePin();
  randomHex(session, 16);
  server.sendHeader("Set-Cookie", String("cs=") + session + "; Path=/; HttpOnly; SameSite=Strict");
  Serial.println("[web] browser signed in");
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
}

static void handleScan() {
  if (!requireAuth()) return;
  int n = WiFi.scanNetworks(false, false);
  JsonDocument d;
  JsonArray list = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (JsonObject o : list) if (s == (const char *)o["ssid"]) { dup = true; break; }
    if (dup) continue;
    JsonObject o = list.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void handleWifi() {
  if (!requireAuth()) return;
  JsonDocument in;
  if (!body(in)) return;
  const char *ssid = in["ssid"] | "";
  const char *host = in["hostname"] | cfg.hostname;
  if (!ssid[0] || strlen(ssid) > 32) { sendError(400, "Pick a network"); return; }
  if (strlen(in["password"] | "") > 64) { sendError(400, "Password too long"); return; }
  bool hostOk = strlen(host) >= 1 && strlen(host) <= 32;
  for (const char *p = host; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') hostOk = false;
  if (!hostOk) { sendError(400, "Device name: letters, digits and '-' only"); return; }

  strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
  strlcpy(cfg.pass, in["password"] | "", sizeof(cfg.pass));
  strlcpy(cfg.hostname, host, sizeof(cfg.hostname));
  if (in["hosting"].is<bool>()) cfg.hosting = in["hosting"];
  settingsSave();
  Serial.printf("[web] WiFi set to '%s', rebooting\n", cfg.ssid);
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
  scheduleReboot();
}

static void handleSigninStart() {
  if (!requireAuth()) return;
  if (apMode) { sendError(400, "Join your WiFi first"); return; }
  JsonDocument in;
  if (!body(in)) return;
  const char *alias = in["alias"] | "";
  if (!validAlias(alias)) { sendError(400, "Name: 1-23 letters, digits, '_', '.', '-'"); return; }
  if (settingsFindAccount(alias) < 0 && cfg.nAccounts >= MAX_ACCOUNTS) {
    sendError(400, "Already at the maximum of 4 accounts");
    return;
  }
  JsonDocument d;
  d["url"] = apiSigninStart(alias, in["models"] | "");
  sendJson(d);
}

static void handleSigninFinish() {
  if (!requireAuth()) return;
  JsonDocument in;
  if (!body(in)) return;
  String err, email;
  if (!apiSigninFinish(in["code"] | "", err, email)) { sendError(400, err.c_str()); return; }
  webWantsCheck = true;
  JsonDocument d;
  d["email"] = email;
  sendJson(d);
}

static void handleRemove() {
  if (!requireAuth()) return;
  JsonDocument in;
  if (!body(in)) return;
  int i = settingsFindAccount(in["alias"] | "");
  if (i < 0) { sendError(404, "No such account"); return; }
  settingsRemoveAccount(i);
  apiSyncAccounts();
  webWantsCheck = true;
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
}

static void handleSettings() {
  if (!requireAuth()) return;
  JsonDocument in;
  if (!body(in)) return;
  if (in["pageSeconds"].is<int>())    cfg.pageSeconds    = constrain(in["pageSeconds"].as<int>(), 0, 3600);
  if (in["refreshMinutes"].is<int>()) cfg.refreshMinutes = constrain(in["refreshMinutes"].as<int>(), 1, 240);
  if (in["brightness"].is<int>())     cfg.brightness     = constrain(in["brightness"].as<int>(), 5, 100);
  if (in["sleepMinutes"].is<int>())   cfg.sleepMinutes   = constrain(in["sleepMinutes"].as<int>(), 0, 1440);
  if (in["dimMinutes"].is<int>())     cfg.dimMinutes     = constrain(in["dimMinutes"].as<int>(), 0, 120);
  if (in["hosting"].is<bool>())    cfg.hosting = in["hosting"];
  settingsSave();
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
}

static void handleReboot() {
  if (!requireAuth()) return;
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
  scheduleReboot();
}

static void handleReset() {
  if (!requireAuth()) return;
  settingsFactoryReset();
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
  scheduleReboot();
}

// Phones probe a few URLs to detect a captive portal; sending everything to
// the setup page makes the page pop up on its own.
static void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

static void routes() {
  if (routesUp) return;
  routesUp = true;
  static const char *headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", WEB_PAGE);
  });
  server.on("/api/state",          HTTP_GET,  handleState);
  server.on("/api/pin/request",    HTTP_POST, handlePinRequest);
  server.on("/api/pin/verify",     HTTP_POST, handlePinVerify);
  server.on("/api/wifi/scan",      HTTP_GET,  handleScan);
  server.on("/api/wifi",           HTTP_POST, handleWifi);
  server.on("/api/signin/start",   HTTP_POST, handleSigninStart);
  server.on("/api/signin/finish",  HTTP_POST, handleSigninFinish);
  server.on("/api/account/remove", HTTP_POST, handleRemove);
  server.on("/api/settings",       HTTP_POST, handleSettings);
  server.on("/api/reboot",         HTTP_POST, handleReboot);
  server.on("/api/reset",          HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
}

static void newHotspotCredentials() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "claude-status-%02x%02x", mac[4], mac[5]);
  static const char A[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 10; i++) apPass[i] = A[esp_random() % (sizeof(A) - 1)];
  apPass[10] = 0;
}

static void serve() {
  routes();
  if (!active) server.begin();
  active = true;
}

void webStartLan() {
  if (lanUp) return;
  serve();
  lanUp = true;
  Serial.printf("[web] setup page at %s (and http://%s.local)\n", webUrl().c_str(), cfg.hostname);
}

// No captive DNS here: the board is also on WiFi, and answering every name
// with itself would break a phone that sends its lookups over this hotspot.
void webStartHotspot() {
  hotspotSeen = millis();
  if (hotspotUp) return;
  newHotspotCredentials();
  WiFi.mode(WIFI_AP_STA);            // keeps the existing WiFi connection
  WiFi.softAP(apSsid, apPass);
  delay(200);
  serve();
  hotspotUp = true;
  Serial.printf("[web] hotspot %s up at %s\n", apSsid, webHotspotUrl().c_str());
}

static void stopHotspot() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  hotspotUp = false;
  if (!lanUp) { server.stop(); active = false; }
  uiHideHotspot();
  Serial.println("[web] hotspot off (idle)");
}

void webStartSetupAP() {
  newHotspotCredentials();
  WiFi.mode(WIFI_AP_STA);            // STA side lets the page scan for networks
  WiFi.softAP(apSsid, apPass);
  delay(200);
  dns.start(53, "*", WiFi.softAPIP());
  routes();
  server.begin();
  apMode = active = true;
  Serial.printf("[web] setup hotspot %s up at %s\n", apSsid, WiFi.softAPIP().toString().c_str());
}

void webLoop() {
  if (!active) return;
  if (apMode) dns.processNextRequest();
  server.handleClient();
  if (pin[0] && millis() > pinUntil) { pin[0] = 0; uiHidePin(); }
  if (rebootAt && millis() > rebootAt) ESP.restart();
  // Stays up while anyone is using it, and always while there's no account.
  if (hotspotUp && accountCount > 0 && WiFi.softAPgetStationNum() == 0 &&
      millis() - hotspotSeen > HOTSPOT_IDLE_MS)
    stopHotspot();
  if (hotspotUp && WiFi.softAPgetStationNum() > 0) hotspotSeen = millis();
}
