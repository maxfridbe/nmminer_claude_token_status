// Claude usage dashboard for the ESP32-2432S028 "Cheap Yellow Display".
//
// Always on at 90% brightness. Checks every account every 15 minutes (sooner
// while the network is unreachable) and redraws every 30 seconds so reset
// countdowns and the WiFi signal stay current. With three or more accounts,
// swipe sideways (or tap near an edge) to scroll.

#include "model.h"
#include <SPI.h>
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
#define Z_MIN        280     // real presses on this panel read 400+
#define SWIPE_PX     40      // horizontal travel that counts as a swipe
#define EDGE_PX      60      // taps this close to either side scroll too

#define BL_CHANNEL 1
#define BL_FREQ    5000
#define BL_BITS    12
#define BL_MAX     ((1 << BL_BITS) - 1)
#define BL_LEVEL   (BL_MAX * 90 / 100)

#ifndef CHECK_INTERVAL_MS
#define CHECK_INTERVAL_MS (15UL * 60UL * 1000UL)
#endif
#define TICK_MS      30000UL                // redraw: countdowns and signal

// Auto page flip with 3+ accounts; secrets.h sets it from config.toml.
#if __has_include("secrets.h") && !defined(DEMO_DATA)
#include "secrets.h"
#endif
#ifndef PAGE_SECONDS
#define PAGE_SECONDS 12
#endif
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

static void rampBacklight(uint32_t to, uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    ledcWrite(BL_CHANNEL, (uint32_t)((float)to * (millis() - start) / ms));
    delay(8);
  }
  ledcWrite(BL_CHANNEL, to);
}

// ---------------------------------------------------------------- touch

static bool touching = false;
static int  startX, lastX, releaseCount;

// The IRQ line only drops under real pressure, so it gates every read.
static bool touchX(int &x) {
  if (digitalRead(XPT2046_IRQ) == HIGH || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < Z_MIN) return false;
  x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, 320), 0, 319);
  return true;
}

// Acts on release: a swipe scrolls toward where the finger came from, a tap
// near either edge scrolls that way.
static void pollTouch() {
  int x;
  if (touchX(x)) {
    if (!touching) { touching = true; startX = x; }
    lastX = x;
    releaseCount = 0;
    return;
  }
  if (!touching || ++releaseCount < 3) return;
  touching = false;

  int dx = lastX - startX, step = 0;
  if (dx <= -SWIPE_PX)           step = +1;
  else if (dx >= SWIPE_PX)       step = -1;
  else if (lastX < EDGE_PX)      step = -1;
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
  uint32_t wait = CHECK_INTERVAL_MS;
  if (networkDown) {
    wait = retryDelay;
    retryDelay = min(retryDelay * 2, (uint32_t)CHECK_INTERVAL_MS);
  } else {
    retryDelay = RETRY_MIN_MS;
  }
  nextCheckAt = millis() + wait;
  lastTick = millis();

  Serial.printf("[check] next in %lu s\n", (unsigned long)(wait / 1000));
  uiDrawAll();
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

  uiInit();
#ifdef DEMO_DATA
  uiSplash("Demo mode - no network");
#else
  uiSplash("Fetching usage...");
#endif
  rampBacklight(BL_LEVEL, 400);

  pinMode(XPT2046_IRQ, INPUT);
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  apiInit();
  runCheck();
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - nextCheckAt) >= 0) { runCheck(); return; }
  if (now - lastTick >= TICK_MS) {
    lastTick = now;
    apiPollLink();
    uiDrawAll();
  }
  if (PAGE_SECONDS > 0 && !touching && now - lastPageFlip >= PAGE_SECONDS * 1000UL) {
    lastPageFlip = now;
    if (uiNextPage()) { Serial.println("[page] auto flip"); uiDrawAll(); }
  }
  pollTouch();
  delay(20);
}
