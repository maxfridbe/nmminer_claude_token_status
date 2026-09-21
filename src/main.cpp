// Claude usage dashboard for the ESP32-2432S028 "Cheap Yellow Display".
//
// Always on at 90% brightness. Checks both accounts every 15 minutes (sooner
// while the network is unreachable) and redraws every 30 seconds so reset
// countdowns and the WiFi signal stay current.

#include "model.h"
#include <TFT_eSPI.h>

#define BL_CHANNEL 1
#define BL_FREQ    5000
#define BL_BITS    12
#define BL_MAX     ((1 << BL_BITS) - 1)
#define BL_LEVEL   (BL_MAX * 90 / 100)

#ifndef CHECK_INTERVAL_MS
#define CHECK_INTERVAL_MS (15UL * 60UL * 1000UL)
#endif
#define TICK_MS      30000UL                // redraw: countdowns and signal
#define RETRY_MIN_MS (2UL * 60UL * 1000UL)  // first retry after a network failure

TFT_eSPI tft = TFT_eSPI();

Account   accounts[MAX_ACCOUNTS];
int       accountCount  = 0;
NetStatus netStatus     = NET_IDLE;
time_t    lastGoodCheck = 0;
bool      networkDown   = false;
Link      wifiLink      = {};

static uint32_t nextCheckAt, lastTick;
static uint32_t retryDelay = RETRY_MIN_MS;

static void rampBacklight(uint32_t to, uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    ledcWrite(BL_CHANNEL, (uint32_t)((float)to * (millis() - start) / ms));
    delay(8);
  }
  ledcWrite(BL_CHANNEL, to);
}

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
  delay(50);
}
