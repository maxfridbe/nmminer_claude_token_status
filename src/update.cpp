#include "update.h"
#include "model.h"
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include "certs.h"

extern TFT_eSPI tft;

#ifndef FW_VERSION
#define FW_VERSION ""
#endif
#ifndef UPDATE_REPO
#define UPDATE_REPO ""
#endif

static const char *DEFAULT_REPO = "maxfridbe/nmminer_claude_token_status";
static const char *APP_ASSET    = "claude-status-app.bin";

const char *fwVersion() { return FW_VERSION[0] ? FW_VERSION : "dev"; }
static const char *repo() { return UPDATE_REPO[0] ? UPDATE_REPO : DEFAULT_REPO; }

#define SCR_W 320
#define SCR_H 240
static uint16_t rgb(uint32_t h) { return tft.color565(h >> 16, (h >> 8) & 0xFF, h & 0xFF); }
static uint16_t cBg, cKey, cText, cDim, cAccent, cBad, cTrack;

static void screen(const char *title, const char *line1, const char *line2 = nullptr, uint16_t c2 = 0) {
  tft.fillScreen(cBg);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(cText);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(title, 12, 16);
  tft.setTextFont(2);
  tft.setTextColor(cText);
  tft.drawString(line1, 12, 60);
  if (line2) { tft.setTextColor(c2 ? c2 : cDim); tft.drawString(line2, 12, 84); }
}

static void button(int x, int y, int w, int h, const char *label, bool primary) {
  tft.fillSmoothRoundRect(x, y, w, h, 9, primary ? cAccent : cKey, cBg);
  tft.setTextFont(2);
  tft.setTextColor(primary ? cBg : cText);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2);
}

// Up to three buttons in a row; returns the index tapped.
static int chooseN(const char *const *labels, int n) {
  const int y = 170, h = 44, gap = 8, w = (SCR_W - 24 - gap * (n - 1)) / n;
  for (int i = 0; i < n; i++) {
    int x = 12 + i * (w + gap);
    tft.fillSmoothRoundRect(x, y, w, h, 9, i == 0 ? cAccent : cKey, cBg);
    tft.setTextFont(2);
    tft.setTextColor(i == 0 ? cBg : cText);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(labels[i], x + w / 2, y + h / 2);
  }
  for (;;) {
    int x, ty;
    touchWaitTap(x, ty);
    if (ty < y - 8 || ty > y + h + 8) continue;
    for (int i = 0; i < n; i++)
      if (x >= 12 + i * (w + gap) - gap / 2 && x < 12 + (i + 1) * (w + gap) - gap / 2) return i;
  }
}

// Two buttons (or one); returns true for the primary.
static bool choose(const char *primary, const char *secondary) {
  const int y = 170, h = 44;
  button(12, y, 140, h, primary, true);
  if (secondary) button(168, y, 140, h, secondary, false);
  for (;;) {
    int x, ty;
    touchWaitTap(x, ty);
    if (ty < y - 8 || ty > y + h + 8) continue;
    if (x < 160) return true;
    if (secondary) return false;
  }
}

// github.com/<repo>/releases/latest redirects to .../releases/tag/<tag>.
static bool latestTag(String &tag, String &err) {
  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOTS);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char *keep[] = {"Location"};
  http.collectHeaders(keep, 1);
  String url = String("https://github.com/") + repo() + "/releases/latest";
  if (!http.begin(tls, url)) { err = "Bad URL"; return false; }
  int code = http.GET();
  String loc = http.header("Location");
  http.end();
  int at = loc.indexOf("/tag/");
  if (code < 300 || code >= 400 || at < 0) {
    err = code > 0 ? String("GitHub answered ") + code : HTTPClient::errorToString(code);
    return false;
  }
  tag = loc.substring(at + 5);
  return tag.length() > 0;
}

// Releases that carry an over-the-air image, newest first.
struct Release { char tag[24]; char date[11]; };

static int listReleases(Release *out, int max, String &err) {
  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOTS);
  HTTPClient http;
  http.setTimeout(15000);
  http.useHTTP10(true);                    // no chunking, so the body streams straight into the parser
  http.setUserAgent("claude-status/1.0 (esp32)");
  String url = String("https://api.github.com/repos/") + repo() + "/releases?per_page=15";
  if (!http.begin(tls, url)) { err = "Bad URL"; return -1; }
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    err = code > 0 ? String("GitHub answered ") + code : HTTPClient::errorToString(code);
    http.end();
    return -1;
  }
  // The list is ~30KB of release notes; keep only what's needed.
  JsonDocument filter;
  filter[0]["tag_name"] = true;
  filter[0]["published_at"] = true;
  filter[0]["assets"][0]["name"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (e) { err = String("Bad reply: ") + e.c_str(); return -1; }

  int n = 0;
  for (JsonObjectConst r : doc.as<JsonArrayConst>()) {
    bool ota = false;
    for (JsonObjectConst a : r["assets"].as<JsonArrayConst>())
      if (!strcmp(a["name"] | "", APP_ASSET)) ota = true;
    if (!ota || n >= max) continue;
    strlcpy(out[n].tag, r["tag_name"] | "", sizeof(out[n].tag));
    strlcpy(out[n].date, r["published_at"] | "", sizeof(out[n].date));
    n++;
  }
  return n;
}

static void drawProgress(int done, int total) {
  static int lastPct = -1;
  int pct = total > 0 ? (int)((int64_t)done * 100 / total) : 0;
  if (pct == lastPct) return;
  lastPct = pct;
  const int x = 12, y = 120, w = SCR_W - 24, h = 14;
  tft.fillSmoothRoundRect(x, y, w, h, 7, cTrack, cBg);
  if (pct > 0) tft.fillSmoothRoundRect(x, y, max(h, w * pct / 100), h, 7, cAccent, cBg);
  char t[24];
  snprintf(t, sizeof(t), "%d%%  %d / %d KB", pct, done / 1024, total / 1024);
  tft.fillRect(x, y + 20, w, 18, cBg);
  tft.setTextFont(2);
  tft.setTextColor(cDim);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(t, x, y + 20);
}

#ifdef OTA_SELFTEST
// Diagnostic build: at boot, install the latest release without asking and
// log progress over serial. Exercises lookup, TLS, redirect and flashing.
void updateSelfTest() {
  String latest, err;
  if (!latestTag(latest, err)) { Serial.printf("[selftest] check failed: %s\n", err.c_str()); return; }
  Serial.printf("[selftest] installed %s, installing %s\n", fwVersion(), latest.c_str());
  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOTS);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int d, int t) {
    static int last = -1;
    int p = t ? (int)((int64_t)d * 100 / t) : 0;
    if (p / 10 != last) { last = p / 10; Serial.printf("[selftest] %d%%\n", p); }
  });
  String url = String("https://github.com/") + repo() + "/releases/download/" + latest + "/" + APP_ASSET;
  if (httpUpdate.update(tls, url) == HTTP_UPDATE_OK) {
    Serial.println("[selftest] OK, restarting");
    delay(300);
    ESP.restart();
  }
  Serial.printf("[selftest] failed: %s\n", httpUpdate.getLastErrorString().c_str());
}
#endif

// Download a release's app image into the idle slot and restart into it.
static void installTag(const String &tag) {
  screen("Updating", (String("Downloading ") + tag).c_str(), "Don't unplug the board.");
  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOTS);
  tls.setTimeout(20);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress(drawProgress);
  String url = String("https://github.com/") + repo() + "/releases/download/" + tag + "/" + APP_ASSET;
  Serial.printf("[update] installing %s\n", tag.c_str());
  t_httpUpdate_return r = httpUpdate.update(tls, url);

  if (r == HTTP_UPDATE_OK) {
    Serial.printf("[update] installed %s, restarting\n", tag.c_str());
    screen("Updated", (String("Installed ") + tag + ".").c_str(), "Restarting...");
    delay(1200);
    ESP.restart();
  }
  String why = httpUpdate.getLastErrorString();
  Serial.printf("[update] failed: %s\n", why.c_str());
  screen("Update failed", why.c_str(), "The current firmware is still installed.", cBad);
  choose("OK", nullptr);
}

// Every release with an over-the-air image, to move to any of them,
// older ones included.
static void versionsScreen(const String &installed, const String &latest) {
  screen("Versions", "Loading the release list...");
  Release rel[8];
  String err;
  int n = listReleases(rel, 8, err);
  if (n <= 0) {
    screen("Versions", n < 0 ? (String("Couldn't list releases: ") + err).c_str() : "No installable releases.",
           nullptr, cBad);
    choose("OK", nullptr);
    return;
  }

  const int y0 = 38, rh = 30, shown = min(n, 4);
  tft.fillScreen(cBg);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(cText);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Versions", 12, 16);
  for (int i = 0; i < shown; i++) {
    int y = y0 + i * (rh + 4);
    bool here = installed == rel[i].tag;
    tft.fillSmoothRoundRect(12, y, SCR_W - 24, rh, 8, here ? cTrack : cKey, cBg);
    tft.setTextFont(2);
    tft.setTextColor(cText);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(rel[i].tag, 24, y + rh / 2);
    tft.setTextColor(cDim);
    tft.setTextDatum(MR_DATUM);
    String note = String(rel[i].date) + (here ? "  installed" : latest == rel[i].tag ? "  latest" : "");
    tft.drawString(note, SCR_W - 24, y + rh / 2);
  }
  const int by = y0 + 4 * (rh + 4) + 4;
  tft.fillSmoothRoundRect(12, by, SCR_W - 24, 34, 9, cKey, cBg);
  tft.setTextFont(2);
  tft.setTextColor(cText);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Cancel", SCR_W / 2, by + 17);

  for (;;) {
    int x, y;
    touchWaitTap(x, y);
    if (y >= by - 4) return;
    int i = (y - y0) / (rh + 4);
    if (y < y0 || i >= shown) continue;
    String tag = rel[i].tag;
    bool older = tag < installed && installed != "dev";
    screen("Install", (String("Install ") + tag + "?").c_str(),
           older ? "That's older than what's installed. Settings and logins are kept."
                 : "Settings and logins are kept.", older ? cAccent : cDim);
    if (choose("Install", "Cancel")) installTag(tag);
    return;
  }
}

void screenFirmwareUpdate() {
  cBg = rgb(0x000000); cKey = rgb(0x22222A); cText = rgb(0xF4F4F7); cDim = rgb(0x8A8A96);
  cAccent = rgb(0xD97757); cBad = rgb(0xFF4B4B); cTrack = rgb(0x262626);
  String installed = fwVersion();
  Serial.printf("[update] installed %s, repo %s\n", installed.c_str(), repo());

  if (WiFi.status() != WL_CONNECTED) {
    screen("Firmware update", "Not connected to WiFi.", "Join a network from the menu first.", cBad);
    choose("OK", nullptr);
    return;
  }
  screen("Firmware update", (String("Installed  ") + installed).c_str(), "Checking GitHub...");

  String latest, err;
  if (!latestTag(latest, err)) {
    Serial.printf("[update] check failed: %s\n", err.c_str());
    screen("Firmware update", (String("Installed  ") + installed).c_str(),
           (String("Couldn't check: ") + err).c_str(), cBad);
    choose("OK", nullptr);
    return;
  }
  // vYY.MM.NN sorts as text, and "dev" builds always count as older.
  bool newer = installed == "dev" || latest > installed;
  Serial.printf("[update] latest %s (%s)\n", latest.c_str(), newer ? "newer" : "not newer");
  screen("Firmware update", (String("Installed  ") + installed).c_str(),
         (String("Latest       ") + latest + (newer ? "  - new" : "  - you're up to date")).c_str(),
         newer ? cAccent : cDim);
  tft.setTextFont(1);
  tft.setTextColor(cDim);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Settings and logins are kept. If the download fails,", 12, 112);
  tft.drawString("the current firmware keeps running.", 12, 124);
  const char *actions[] = {newer ? "Update" : "Reinstall", "Versions", "Cancel"};
  switch (chooseN(actions, 3)) {
    case 0: installTag(latest); break;
    case 1: versionsScreen(installed, latest); break;
    default: break;
  }
}
