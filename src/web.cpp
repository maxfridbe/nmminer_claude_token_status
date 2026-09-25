#include "web.h"
#include "web_page.h"
#include "model.h"
#include "settings.h"
#include "relay.h"
#include "board.h"
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

// Handlers answer the same whether the request came over HTTP (LAN or
// hotspot) or through the relay: JSON in, JSON out, an HTTP status back.
enum Via { VIA_LAN, VIA_HOTSPOT, VIA_RELAY };
struct Ctx { Via via; bool authed; };
typedef int (*Handler)(const Ctx &, JsonDocument &in, JsonDocument &out);

static int fail(JsonDocument &out, int code, const char *msg) {
  out.clear();
  out["error"] = msg;
  return code;
}

static int okReply(JsonDocument &out) {
  out["ok"] = true;
  return 200;
}

static void scheduleReboot() { rebootAt = millis() + 1500; }

static bool validAlias(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 23) return false;
  for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("_.-", *s)) return false;
  return true;
}

// ---------------------------------------------------------------- handlers

static int handleState(const Ctx &c, JsonDocument &, JsonDocument &d) {
  d["mode"]        = apMode ? "setup" : "lan";
  d["via"]         = c.via == VIA_RELAY ? "relay" : c.via == VIA_HOTSPOT ? "hotspot" : "lan";
  d["relay"]       = relayBrokerName();
  d["relayCfg"]    = cfg.relay;
  d["hotspot"]     = hotspotUp ? webApSsid() : "";
  d["authed"]      = c.authed;
  d["hostname"]    = cfg.hostname;
  d["ssid"]        = cfg.ssid;
  d["url"]         = webUrl();
  d["mdns"]        = String("http://") + cfg.hostname + ".local";
  d["hosting"]     = cfg.hosting;
  d["pageSeconds"]    = cfg.pageSeconds;
  d["refreshMinutes"] = cfg.refreshMinutes;
  d["brightness"]     = cfg.brightness;
  d["lightMode"]      = cfg.lightMode;
  d["portrait"]       = cfg.portrait;
  d["canRotate"]      = (bool)CAN_ROTATE;
  d["sleepMinutes"]   = cfg.sleepMinutes;
  d["dimMinutes"]     = cfg.dimMinutes;
  d["maxAccounts"] = MAX_ACCOUNTS;
  d["pinPending"]  = pin[0] && millis() < pinUntil;
  if (c.authed) {
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
  return 200;
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
  if (deserializeJson(in, server.arg("plain"))) { sendError(400, "Bad request"); return; }
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

static int handleScan(const Ctx &, JsonDocument &, JsonDocument &d) {
  int n = WiFi.scanNetworks(false, false);
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
  return 200;
}

static int handleWifi(const Ctx &, JsonDocument &in, JsonDocument &out) {
  const char *ssid = in["ssid"] | "";
  const char *host = in["hostname"] | cfg.hostname;
  if (!ssid[0] || strlen(ssid) > 32) return fail(out, 400, "Pick a network");
  if (strlen(in["password"] | "") > 64) return fail(out, 400, "Password too long");
  bool hostOk = strlen(host) >= 1 && strlen(host) <= 32;
  for (const char *p = host; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') hostOk = false;
  if (!hostOk) return fail(out, 400, "Device name: letters, digits and '-' only");

  strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
  strlcpy(cfg.pass, in["password"] | "", sizeof(cfg.pass));
  strlcpy(cfg.hostname, host, sizeof(cfg.hostname));
  if (in["hosting"].is<bool>()) cfg.hosting = in["hosting"];
  settingsSave();
  Serial.printf("[web] WiFi set to '%s', rebooting\n", cfg.ssid);
  scheduleReboot();
  return okReply(out);
}

static int handleSigninStart(const Ctx &, JsonDocument &in, JsonDocument &d) {
  if (apMode) return fail(d, 400, "Join your WiFi first");
  const char *alias = in["alias"] | "";
  if (!validAlias(alias)) return fail(d, 400, "Name: 1-23 letters, digits, '_', '.', '-'");
  if (settingsFindAccount(alias) < 0 && cfg.nAccounts >= MAX_ACCOUNTS)
    return fail(d, 400, "Already at the maximum of 4 accounts");
  d["url"] = apiSigninStart(alias, in["models"] | "");
  return 200;
}

static int handleSigninFinish(const Ctx &, JsonDocument &in, JsonDocument &d) {
  String err, email;
  if (!apiSigninFinish(in["code"] | "", err, email)) return fail(d, 400, err.c_str());
  webWantsCheck = true;
  d["email"] = email;
  return 200;
}

static int handleRemove(const Ctx &, JsonDocument &in, JsonDocument &out) {
  int i = settingsFindAccount(in["alias"] | "");
  if (i < 0) return fail(out, 404, "No such account");
  settingsRemoveAccount(i);
  apiSyncAccounts();
  webWantsCheck = true;
  return okReply(out);
}

static int handleSettings(const Ctx &, JsonDocument &in, JsonDocument &out) {
  if (in["relay"].is<const char *>()) {
    const char *r = in["relay"];
    if (!relayConfigValid(r)) return fail(out, 400, "Relay: empty, hivemq, mosquitto, or host:port|wss://url");
    strlcpy(cfg.relay, r, sizeof(cfg.relay));
  }
  if (in["pageSeconds"].is<int>())    cfg.pageSeconds    = constrain(in["pageSeconds"].as<int>(), 0, 3600);
  if (in["refreshMinutes"].is<int>()) cfg.refreshMinutes = constrain(in["refreshMinutes"].as<int>(), 1, 240);
  if (in["brightness"].is<int>())     cfg.brightness     = constrain(in["brightness"].as<int>(), 5, 100);
  if (in["sleepMinutes"].is<int>())   cfg.sleepMinutes   = constrain(in["sleepMinutes"].as<int>(), 0, 1440);
  if (in["dimMinutes"].is<int>())     cfg.dimMinutes     = constrain(in["dimMinutes"].as<int>(), 0, 120);
  if (in["hosting"].is<bool>())    cfg.hosting = in["hosting"];
  if (in["lightMode"].is<bool>() && in["lightMode"].as<bool>() != cfg.lightMode) {
    cfg.lightMode = in["lightMode"];
    uiApplyTheme();
    uiDrawAll();
  }
  // The screen's size is fixed at boot, so turning the board on end restarts
  // it. The touch calibration goes with it: those corners were another
  // rotation's.
  bool rotated = false;
  if (CAN_ROTATE && in["portrait"].is<bool>() && in["portrait"].as<bool>() != cfg.portrait) {
    cfg.portrait = in["portrait"];
    rotated = true;
  }
  settingsSave();
  if (rotated) {
#if TOUCH_VIA_TFT
    settingsForgetTouchCal();
#endif
    scheduleReboot();
  }
  return okReply(out);
}

static int handleReboot(const Ctx &, JsonDocument &, JsonDocument &out) {
  scheduleReboot();
  return okReply(out);
}

static int handleReset(const Ctx &, JsonDocument &, JsonDocument &out) {
  settingsFactoryReset();
  scheduleReboot();
  return okReply(out);
}

struct Route { HTTPMethod method; const char *path; Handler h; bool auth; };
static const Route ROUTES[] = {
  {HTTP_GET,  "/api/state",          handleState,        false},
  {HTTP_GET,  "/api/wifi/scan",      handleScan,         true},
  {HTTP_POST, "/api/wifi",           handleWifi,         true},
  {HTTP_POST, "/api/signin/start",   handleSigninStart,  true},
  {HTTP_POST, "/api/signin/finish",  handleSigninFinish, true},
  {HTTP_POST, "/api/account/remove", handleRemove,       true},
  {HTTP_POST, "/api/settings",       handleSettings,     true},
  {HTTP_POST, "/api/reboot",         handleReboot,       true},
  {HTTP_POST, "/api/reset",          handleReset,        true},
};

static int runRoute(const Route &r, const Ctx &c, JsonDocument &in, JsonDocument &out) {
  if (r.auth && !c.authed) return fail(out, 401, "Enter the PIN from the display first");
  return r.h(c, in, out);
}

static void serveHttp(const Route &r) {
  Ctx c = {fromHotspot() ? VIA_HOTSPOT : VIA_LAN, authed()};
  JsonDocument in, out;
  if (r.method == HTTP_POST && deserializeJson(in, server.arg("plain"))) { sendError(400, "Bad request"); return; }
  int code = runRoute(r, c, in, out);
  sendJson(out, code);
}

// The relay's way in. Whoever holds the relay's key read it off the display,
// so they are trusted like someone on the board's hotspot.
int webApiCall(const char *method, const char *path, JsonDocument &in, JsonDocument &out) {
  HTTPMethod m = !strcmp(method, "POST") ? HTTP_POST : HTTP_GET;
  for (const Route &r : ROUTES)
    if (r.method == m && !strcmp(r.path, path)) return runRoute(r, {VIA_RELAY, true}, in, out);
  return fail(out, 404, "Not found");
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
  server.on("/api/pin/request",    HTTP_POST, handlePinRequest);
  server.on("/api/pin/verify",     HTTP_POST, handlePinVerify);
  for (const Route &r : ROUTES) server.on(r.path, r.method, [&r] { serveHttp(r); });
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
  if (rebootAt && millis() > rebootAt) ESP.restart();   // also for relay requests
  if (!active) return;
  if (apMode) dns.processNextRequest();
  server.handleClient();
  if (pin[0] && millis() > pinUntil) { pin[0] = 0; uiHidePin(); }
  // Stays up while anyone is using it. On a board with no touch and no button
  // it is the only way in, so it stays until an account exists.
  if (hotspotUp && (HAS_INPUT || accountCount > 0) && WiFi.softAPgetStationNum() == 0 &&
      millis() - hotspotSeen > HOTSPOT_IDLE_MS)
    stopHotspot();
  if (hotspotUp && WiFi.softAPgetStationNum() > 0) hotspotSeen = millis();
}
