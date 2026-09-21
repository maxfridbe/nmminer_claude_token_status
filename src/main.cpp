// Claude usage dashboard for the ESP32-2432S028 "Cheap Yellow Display".
//
// Checks every account every refresh_minutes (sooner while the network is
// unreachable) and redraws every 30 seconds so reset countdowns and the WiFi
// signal stay current. With sleep_minutes set, the screen dims out after that
// long without a touch or a change, and a touch or a change brings it back. With three or more accounts,
// swipe sideways (or tap near an edge) to scroll.
//
// With no WiFi saved, or with the screen held at power-on, it starts a setup
// hotspot instead; the setup website takes it from there.

#include "model.h"
#include "settings.h"
#include "web.h"
#include "wifi_screen.h"
#include <SPI.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// Touch controller: its own pins, not the display's HSPI bus.
#define XPT2046_CLK  25
#define XPT2046_MOSI 32
#define XPT2046_CS   33
#define XPT2046_MISO 39
#define XPT2046_IRQ  36
#define TS_MINX      200     // raw extents, landscape
#define TS_MAXX      3700
#define TS_MINY      240
#define TS_MAXY      3800
#define LONG_PRESS_MS 1500   // hold this long for the menu
#define Z_MIN        280     // real presses on this panel read 400+
#define SWIPE_PX     40      // horizontal travel that counts as a swipe
#define EDGE_PX      60      // taps this close to either side scroll too

#define BL_CHANNEL 1
#define BL_FREQ    5000
#define BL_BITS    12
#define BL_MAX     ((1 << BL_BITS) - 1)
#define TICK_MS      30000UL                // redraw: countdowns and signal
#define HOLD_MS      2000UL                 // hold at power-on for setup mode
#define RETRY_MIN_MS (2UL * 60UL * 1000UL)  // first retry after a network failure

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

Account   accounts[MAX_ACCOUNTS];
int       accountCount  = 0;
NetStatus netStatus     = NET_IDLE;
time_t    lastGoodCheck = 0;
bool      networkDown   = false;
Link      wifiLink      = {};

static uint32_t nextCheckAt, lastTick, lastPageFlip;
static uint32_t retryDelay = RETRY_MIN_MS;
static bool     setupMode  = false;

// ---------------------------------------------------------------- screen

enum Screen { SCREEN_ON, SCREEN_FADING, SCREEN_OFF };
static Screen   screen = SCREEN_ON;
static uint32_t lastActivity, fadeStart, blLevel;
static String   lastSig;

static uint32_t checkIntervalMs() {
#ifdef CHECK_INTERVAL_MS
  return CHECK_INTERVAL_MS;                    // demo builds shorten it
#else
  return max<uint32_t>(cfg.refreshMinutes, 1) * 60000UL;
#endif
}

static uint32_t onLevel() { return BL_MAX * constrain((int)cfg.brightness, 5, 100) / 100; }

static void setBacklight(uint32_t v) { blLevel = v; ledcWrite(BL_CHANNEL, v); }

static void rampBacklight(uint32_t to, uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    ledcWrite(BL_CHANNEL, (uint32_t)((float)to * (millis() - start) / ms));
    delay(8);
  }
  ledcWrite(BL_CHANNEL, to);
  blLevel = to;
}

static void panelSleep(bool sleep) {
  if (sleep) { tft.writecommand(0x28); tft.writecommand(0x10); }       // DISPOFF, SLPIN
  else       { tft.writecommand(0x11); delay(120); tft.writecommand(0x29); }  // SLPOUT, DISPON
}

static void wake(const char *why) {
  lastActivity = millis();
  if (screen == SCREEN_ON) return;
  if (screen == SCREEN_OFF) { panelSleep(false); uiDrawAll(); }
  screen = SCREEN_ON;
  setBacklight(onLevel());
  Serial.printf("[screen] on (%s)\n", why);
}

// Dimming and sleep, plus picking up brightness changes from the website.
static void updateScreen(uint32_t now) {
  if (screen == SCREEN_ON) {
    if (blLevel != onLevel()) setBacklight(onLevel());
    if (cfg.sleepMinutes && now - lastActivity >= cfg.sleepMinutes * 60000UL) {
      screen = SCREEN_FADING;
      fadeStart = now;
      Serial.println("[screen] dimming");
    }
  } else if (screen == SCREEN_FADING) {
    uint32_t len = max<uint32_t>(cfg.dimMinutes * 60000UL, 1000);
    float t = (float)(now - fadeStart) / len;
    if (t >= 1.0f) {
      setBacklight(0);
      panelSleep(true);
      screen = SCREEN_OFF;
      Serial.println("[screen] off");
    } else {
      setBacklight((uint32_t)(onLevel() * powf(1.0f - t, 2.2f)));   // perceptually even
    }
  }
}

// What a person would notice: rounded percentages, reset times to the minute,
// and which accounts are failing.
static String statsSignature() {
  String sig;
  for (int i = 0; i < accountCount; i++) {
    const Account &a = accounts[i];
    sig += a.ok ? 'Y' : 'N';
    for (int b = 0; b < a.nBuckets; b++) {
      sig += a.buckets[b].label; sig += ':';
      sig += (int)(a.buckets[b].util + 0.5f); sig += '@';
      sig += (long)(a.buckets[b].resetsAt / 60); sig += ';';
    }
    sig += '#';
  }
  return sig;
}


// ---------------------------------------------------------------- touch

static bool touching = false, longFired = false;
static int  startX, startY, lastX, lastY, releaseCount;
static uint32_t pressStart;

// The IRQ line only drops under real pressure, so it gates every read.
bool touchRead(int &x, int &y) {
  if (digitalRead(XPT2046_IRQ) == HIGH || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < Z_MIN) return false;
  x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, 320), 0, 319);
  y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, 240), 0, 239);
  return true;
}

static void menuAction(int item) {
  Serial.printf("[menu] %d\n", item);
  switch (item) {
    case 0:   // phone setup page, over the board's own hotspot
      webStartHotspot();
      uiShowHotspot(webApSsid(), webApPass(), webHotspotUrl().c_str());
      break;
    case 1:   // WiFi on the touchscreen; accounts and logins untouched
      if (screenWifiSetup()) {
        uiSplash((String("Joined ") + cfg.ssid + ". Restarting...").c_str());
        delay(900);
        ESP.restart();
      }
      uiCloseOverlay();
      break;
    case 2:   // WiFi from a phone, on the setup hotspot at next boot
      settingsRequestSetup();
      uiSplash("Restarting into WiFi setup...");
      delay(700);
      ESP.restart();
      break;
    case 3:
      uiSplash("Restarting...");
      delay(400);
      ESP.restart();
      break;
    case 4:
      uiCloseOverlay();
      break;
  }
}

// Hold for the menu. On release: a tap works the menu or closes an overlay,
// a swipe scrolls toward where the finger came from, and a tap near either
// edge scrolls that way.
static void pollTouch() {
  int x, y;
  if (touchRead(x, y)) {
    if (!touching) { touching = true; longFired = false; startX = x; startY = y; pressStart = millis(); }
    lastX = x; lastY = y;
    releaseCount = 0;
    if (!longFired && millis() - pressStart >= LONG_PRESS_MS &&
        abs(lastX - startX) < 30 && abs(lastY - startY) < 30) {
      longFired = true;
      wake("touch");
      Serial.println("[touch] menu");
      uiShowMenu();
    }
    return;
  }
  if (!touching || ++releaseCount < 3) return;
  touching = false;
  if (longFired) return;                     // the hold already opened the menu
  if (screen != SCREEN_ON) { wake("touch"); return; }
  lastActivity = millis();

  switch (uiOverlay()) {
    case OV_MENU:    { int i = uiMenuHit(lastX, lastY); if (i >= 0) menuAction(i); return; }
    case OV_HOTSPOT: uiCloseOverlay(); return;
    case OV_PIN:     return;
  }

  int dx = lastX - startX, step = 0;
  if (dx <= -SWIPE_PX)            step = +1;
  else if (dx >= SWIPE_PX)        step = -1;
  else if (lastX < EDGE_PX)       step = -1;
  else if (lastX > 320 - EDGE_PX) step = +1;
  if (step && uiScroll(step)) {
    Serial.printf("[touch] scroll %+d\n", step);
    uiDrawAll();
    lastTick = lastPageFlip = millis();   // let the reader stay on this page
  }
}

// ---------------------------------------------------------------- checks

static void runCheck() {
  apiCheckAll();

  // Network failures retry sooner, backing off 2, 4, 8 min up to the normal
  // interval.
  uint32_t wait = checkIntervalMs();
  if (networkDown) {
    wait = min(retryDelay, checkIntervalMs());
    retryDelay = min(retryDelay * 2, checkIntervalMs());
  } else {
    retryDelay = RETRY_MIN_MS;
  }
  nextCheckAt = millis() + wait;
  lastTick = millis();

  Serial.printf("[check] next in %lu s\n", (unsigned long)(wait / 1000));
  uiDrawAll();

  // New numbers wake a sleeping screen; a network blip doesn't count.
  if (!networkDown) {
    String sig = statsSignature();
    if (sig != lastSig && lastSig.length()) wake("usage changed");
    lastSig = sig;
  }
}

// Held from power-on for HOLD_MS: the way back to setup when the old WiFi is
// gone and the menu can't be reached over it.
static bool heldAtBoot() {
  if (digitalRead(XPT2046_IRQ) == HIGH) return false;
  uiSplash("Keep holding for setup...");
  uint32_t start = millis();
  while (millis() - start < HOLD_MS)
    if (digitalRead(XPT2046_IRQ) == HIGH) return false;
  return true;
}

// Serve the setup page once WiFi is up, if hosting is on or there is nothing
// to show yet.
// With no account yet, the board's hotspot stays up so a phone can add one
// even on a guest network. With hosting on, the page is also on the LAN.
static void maybeStartWeb() {
  if (WiFi.status() != WL_CONNECTED) return;
  bool changed = false;
  if (accountCount == 0 && !webHotspotUp()) { webStartHotspot(); changed = true; }
  if (cfg.hosting && !wifiLink.url[0]) {
    webStartLan();
    strlcpy(wifiLink.url, webUrl().c_str(), sizeof(wifiLink.url));
    changed = true;
  }
  if (changed) uiDrawAll();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nclaude-status starting");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // tft.init() drives the backlight as a plain GPIO; hand it to LEDC to set 90%.
  ledcSetup(BL_CHANNEL, BL_FREQ, BL_BITS);
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, 0);

  pinMode(XPT2046_IRQ, INPUT);
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  uiInit();
#ifdef DEMO_DATA
  uiSplash("Demo mode - no network");
  cfg.pageSeconds = 12;
  cfg.brightness = 90;
  rampBacklight(onLevel(), 400);
  apiInit();
  runCheck();
#else
  uiSplash("Starting...");
  settingsLoad();
  rampBacklight(onLevel(), 400);

  bool asked = settingsTakeSetupFlag();       // "Change WiFi" from the menu
  bool held = !asked && heldAtBoot();
  if (!settingsHaveWifi() || held || asked) {
    Serial.println(asked ? "[boot] WiFi setup requested from the menu" :
                   held  ? "[boot] screen held: setup mode" : "[boot] no WiFi saved: setup mode");
    webStartSetupAP();
    uiSetupScreen(webApSsid(), webApPass(), webUrl().c_str());
    setupMode = true;
    return;
  }

  apiInit();
  uiSplash((String("Joining ") + cfg.ssid + "...").c_str());
  apiWifiUp();
  maybeStartWeb();
  runCheck();
#endif
}

// Setup hotspot screen: "Use this screen" types the WiFi in right here.
static void setupModeTouch() {
  int x, y;
  if (!touchRead(x, y)) return;
  int rx = x, ry = y, released = 0;
  while (released < 3) { released = touchRead(x, y) ? 0 : released + 1; delay(10); }
  if (!uiSetupButtonHit(rx, ry)) return;
  if (screenWifiSetup()) {
    uiSplash((String("Joined ") + cfg.ssid + ". Restarting...").c_str());
    delay(900);
    ESP.restart();
  }
  uiSetupScreen(webApSsid(), webApPass(), webUrl().c_str());
}

void loop() {
  webLoop();
  if (setupMode) { setupModeTouch(); delay(2); return; }

  uint32_t now = millis();
  maybeStartWeb();
  if (webWantsCheck) {          // an account was added or removed on the page
    webWantsCheck = false;
    wake("setup page");
    apiSyncAccounts();
    runCheck();
    return;
  }
  if ((int32_t)(now - nextCheckAt) >= 0) { runCheck(); return; }
  updateScreen(now);
  pollTouch();
  if (screen == SCREEN_OFF) { delay(20); return; }
  if (now - lastTick >= TICK_MS) {
    lastTick = now;
    apiPollLink();
    uiDrawAll();
  }
  if (cfg.pageSeconds > 0 && !touching && now - lastPageFlip >= cfg.pageSeconds * 1000UL) {
    lastPageFlip = now;
    if (uiNextPage()) { Serial.println("[page] auto flip"); uiDrawAll(); }
  }
  delay(10);
}
