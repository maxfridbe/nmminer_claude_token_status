// Claude usage dashboard for ESP32 display boards (see board.h).
//
// Checks every account every refresh_minutes (sooner while the network is
// unreachable) and redraws every 30 seconds so reset countdowns and the WiFi
// signal stay current. With sleep_minutes set, the screen dims out after that
// long without a touch or a change, and a touch or a change brings it back.
// When accounts don't all fit, pages flip on a timer; swipe (touchscreen) or
// tap (button) to move by hand. Hold for the menu.
//
// With no WiFi saved, or with the screen held at power-on, it starts a setup
// hotspot instead; the setup website takes it from there.

#include "model.h"
#include "settings.h"
#include "web.h"
#include "wifi_screen.h"
#include "update.h"
#include "input.h"
#include "board.h"
#include "relay.h"
#include <WiFi.h>
#include <TFT_eSPI.h>

#define EDGE_PX      60      // touchscreen taps this close to a side scroll too

#define BL_CHANNEL 1
#define BL_FREQ    5000
#define BL_BITS    12
#define BL_MAX     ((1 << BL_BITS) - 1)
#define TICK_MS      30000UL                // redraw: countdowns and signal
#define HOLD_MS      2000UL                 // hold at power-on for setup mode
#define RETRY_MIN_MS (2UL * 60UL * 1000UL)  // first retry after a network failure

TFT_eSPI tft = TFT_eSPI();

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

// Duty in "brightness" terms; some boards light the backlight on a low level.
static void blWrite(uint32_t v) { ledcWrite(BL_CHANNEL, BL_ACTIVE_LOW ? BL_MAX - v : v); }

static void setBacklight(uint32_t v) { blLevel = v; blWrite(v); }

static void rampBacklight(uint32_t to, uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    blWrite((uint32_t)((float)to * (millis() - start) / ms));
    delay(8);
  }
  setBacklight(to);
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


// ---------------------------------------------------------------- input

// Relay: works anywhere. Address: only if the phone can see the board, which
// guest networks prevent. Hotspot: the phone joins the board itself.
static void phoneSetup() {
  bool guest = String(wifiLink.ssid).indexOf("uest") >= 0;   // Guest, guest
  String lanNote = guest ? String("!'") + wifiLink.ssid + "' looks like guest WiFi: likely blocked"
                         : String("!Phone must be on ") + wifiLink.ssid + ". Guest WiFi blocks it";
  const char *labels[] = {"Remote link (any network)", "This network (IP address)", "Board hotspot", "Cancel"};
  const char *subs[]   = {"One-time encrypted link through a relay", lanNote.c_str(),
                          "Your phone joins the board's own WiFi", ""};
  switch (uiChoose("Phone setup", labels, subs, 4)) {
    case 0:
      relayStart();
      uiShowRemote();
      break;
    case 1:
      if (!wifiLink.up) { uiCloseOverlay(); break; }
      webStartLan();
      strlcpy(wifiLink.url, webUrl().c_str(), sizeof(wifiLink.url));
      uiShowLan(wifiLink.url);
      break;
    case 2:
      webStartHotspot();
      uiShowHotspot(webApSsid(), webApPass(), webHotspotUrl().c_str());
      break;
    default:
      uiCloseOverlay();
  }
}

// Brightness from the menu: applied at once, saved once the taps stop.
static uint32_t brightSaveAt;
static void setBrightness(int pct) {
  cfg.brightness = constrain(pct, 5, 100);
  setBacklight(onLevel());
  brightSaveAt = millis() + 3000;
  uiMenuBrightnessChanged();
}

static void menuAction(MenuAction act) {
  Serial.printf("[menu] %d\n", (int)act);
  switch (act) {
    case MA_PHONE:        // the setup page, three ways to reach it
      phoneSetup();
      break;
    case MA_WIFI_SCREEN:  // WiFi on the touchscreen; accounts and logins untouched
      if (screenWifiSetup()) {
        uiSplash((String("Joined ") + cfg.ssid + ". Restarting...").c_str());
        delay(900);
        ESP.restart();
      }
      uiCloseOverlay();
      break;
    case MA_WIFI_PHONE:   // WiFi from a phone, on the setup hotspot at next boot
      settingsRequestSetup();
      uiSplash("Restarting into WiFi setup...");
      delay(700);
      ESP.restart();
      break;
    case MA_UPDATE:       // firmware from GitHub releases
      relayStop();                            // its TLS memory goes to the download
      screenFirmwareUpdate();
      uiCloseOverlay();
      break;
#if TOUCH_VIA_TFT
    case MA_CALIBRATE:
      uiTouchCalibrate(cfg.touchCal, 0);
      settingsSaveTouchCal();
      uiCloseOverlay();
      break;
#endif
    case MA_WIPE:         // factory reset: WiFi, accounts and logins
      if (uiConfirm("Wipe all settings", "Erases WiFi, accounts and every login on this board.",
                    "Wipe", "Cancel")) {
        settingsFactoryReset();
        uiSplash("Wiped. Restarting...");
        delay(900);
        ESP.restart();
      }
      uiCloseOverlay();
      break;
    case MA_BRIGHT_DOWN:
      setBrightness(cfg.brightness > 10 ? cfg.brightness - 10 : 5);
      return;
    case MA_BRIGHT_UP:
      setBrightness(cfg.brightness < 10 ? 10 : cfg.brightness + 10);
      return;
    case MA_THEME:
      cfg.lightMode = !cfg.lightMode;
      uiApplyTheme();
      brightSaveAt = millis() + 3000;
      uiDrawAll();                            // the menu, repainted in the new colors
      return;
    case MA_BRIGHT: {     // one button: the next step up, wrapping to the dimmest
      static const uint8_t STEPS[] = {10, 25, 50, 75, 100};
      int next = STEPS[0];
      for (uint8_t s : STEPS) if (s > cfg.brightness) { next = s; break; }
      setBrightness(next);
      return;
    }
    case MA_RESTART:
      if (brightSaveAt) settingsSave();
      uiSplash("Restarting...");
      delay(400);
      ESP.restart();
      break;
    case MA_CLOSE:
    default:
      uiCloseOverlay();
      break;
  }
}

static void scrollBy(int step) {
  if (step && uiScroll(step)) {
    Serial.printf("[input] page %+d\n", step);
    uiDrawAll();
    lastTick = lastPageFlip = millis();   // let the reader stay on this page
  }
}

// Hold opens the menu (on a one-button board, holding in the menu picks the
// highlighted item). A tap works an open menu or closes an overlay; on the
// dashboard, a swipe or edge tap scrolls (touchscreen) or a tap moves to the
// next page (button).
static void handleInput() {
  InputEvent e = inputPoll();
  if (e.kind == IN_NONE) return;

  if (e.kind == IN_DOUBLE) {     // one-button boards: straight into and out of the menu
    wake("double tap");
    if (uiOverlay() == OV_MENU) uiCloseOverlay();
    else                        uiShowMenu();
    return;
  }
  if (e.kind == IN_HOLD) {
    wake("hold");
    if (!HAS_TOUCHSCREEN && uiOverlay() == OV_MENU) { menuAction(uiMenuSelected()); return; }
    Serial.println("[input] menu");
    uiShowMenu();
    return;
  }
  if (screen != SCREEN_ON) { wake("touch"); return; }
  lastActivity = millis();

  switch (uiOverlay()) {
    case OV_MENU:
      if (HAS_TOUCHSCREEN) { MenuAction a = uiMenuHit(e.x, e.y); if (a != MA_NONE) menuAction(a); }
      else                 uiMenuNext();
      return;
    case OV_HOTSPOT:
    case OV_REMOTE:
    case OV_LAN:     uiCloseOverlay(); return;
    case OV_PIN:     return;
  }

  if (e.kind == IN_SWIPE_LEFT)  { scrollBy(+1); return; }
  if (e.kind == IN_SWIPE_RIGHT) { scrollBy(-1); return; }
  if (!HAS_TOUCHSCREEN)         { if (uiNextPage()) { uiDrawAll(); lastTick = lastPageFlip = millis(); } return; }
  if (e.x < EDGE_PX)            scrollBy(-1);
  else if (e.x > SCR_W - EDGE_PX) scrollBy(+1);
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
  if (!inputPressed()) return false;
  uiSplash("Keep holding for setup...");
  uint32_t start = millis();
  while (millis() - start < HOLD_MS)
    if (!inputPressed()) return false;
  return true;
}

// Serve the setup page once WiFi is up, if hosting is on or there is nothing
// to show yet.
// With no account yet, the board's hotspot stays up so a phone can add one
// even on a guest network. With hosting on, the page is also on the LAN.
static void maybeStartWeb() {
  if (WiFi.status() != WL_CONNECTED) return;
  bool changed = false;
  // No account yet, or no way to ask for it on the board: offer the hotspot.
  // webLoop() drops it after 15 idle minutes once an account exists.
  static bool offered = false;
  if (accountCount == 0 && relayState() == RELAY_OFF) relayStart();   // shown on the empty screen
  if ((accountCount == 0 || (!HAS_INPUT && !offered)) && !webHotspotUp()) {
    webStartHotspot();
    offered = true;
    changed = true;
  }
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
  Serial.printf("\nclaude-status %s (%s) starting\n", fwVersion(), BOARD_ID);

  if (PANEL_POWER_PIN >= 0) {            // boards that switch the display's power
    pinMode(PANEL_POWER_PIN, OUTPUT);
    digitalWrite(PANEL_POWER_PIN, PANEL_POWER_LEVEL);
    delay(20);
  }
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // tft.init() drives the backlight as a plain GPIO; hand it to LEDC for dimming.
  ledcSetup(BL_CHANNEL, BL_FREQ, BL_BITS);
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  blWrite(0);

  inputBegin();
  inputSetHoldIndicator(uiHoldProgress);   // fill the item while the button is held

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
  uiApplyTheme();
  if (cfg.lightMode) uiSplash("Starting...");
  rampBacklight(onLevel(), 400);

#if TOUCH_VIA_TFT
  // This panel's touch needs calibrating once, before anything that needs
  // tapping. A board that has never been set up waits a while for someone to
  // start it; otherwise it moves on quickly. Either way it falls back to the
  // board's defaults, so broken touch can't strand it here.
  tft.setTouch(cfg.touchCal);
  if (!cfg.touchCalOk && uiTouchCalibrate(cfg.touchCal, settingsHaveWifi() ? 15000 : 60000))
    settingsSaveTouchCal();
#endif

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
#ifdef OTA_SELFTEST
  updateSelfTest();
#endif
#endif
}

// Setup hotspot screen: on a touchscreen, "Use this screen" types the WiFi in
// right here.
static void setupModeTouch() {
  if (!HAS_TOUCHSCREEN) return;
  InputEvent e = inputPoll();
  if (e.kind == IN_NONE) return;
  Serial.printf("[input] setup screen %s at %d,%d\n", e.kind == IN_TAP ? "tap" : "event", e.x, e.y);
  if (e.kind != IN_TAP || !uiSetupButtonHit(e.x, e.y)) return;
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
  if (brightSaveAt && (int32_t)(now - brightSaveAt) >= 0) { brightSaveAt = 0; settingsSave(); }
  relayLoop();
  static String shown;                       // redraw the QR screens when the link changes
  String now_ = relayLink();
  if (!now_.length()) now_ = String((int)relayState());   // no link yet: its progress
  if (now_ != shown) {
    shown = now_;
    if (uiOverlay() == OV_REMOTE || (accountCount == 0 && uiOverlay() == OV_NONE)) uiDrawAll();
  }
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
  handleInput();
  if (screen == SCREEN_OFF) { delay(20); return; }
  if (now - lastTick >= TICK_MS) {
    lastTick = now;
    apiPollLink();
    uiDrawAll();
  }
  if (cfg.pageSeconds > 0 && !inputPressed() && uiOverlay() == OV_NONE &&
      now - lastPageFlip >= cfg.pageSeconds * 1000UL) {
    lastPageFlip = now;
    if (uiNextPage()) { Serial.println("[page] auto flip"); uiDrawAll(); }
  }
  delay(10);
}
