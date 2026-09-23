// Persistent device settings, in NVS: WiFi, hostname, hosting, accounts and
// their tokens. Written by the setup website, and seeded from secrets.h when
// the firmware was built by deploy_and_build.sh with a config file.
#pragma once
#include <Arduino.h>
#include "model.h"

struct AccountCfg {
  char     alias[24];
  char     email[64];
  char     plan[12];
  char     models[48];      // comma list of model names, "" = every limit
  char     scope[96];       // scope requested on refresh
  char     seed[20];        // identifies the login a script deploy seeded
  String   access;
  String   refresh;
  uint64_t expiresMs;
};

struct Settings {
  char       ssid[33];
  char       pass[65];
  char       hostname[33];
  char       tz[64];
  char       refreshScope[96];
  bool       hosting;       // serve the setup website on the LAN
  bool       lightMode;     // dark text on a light background (theme.h)
  char       relay[128];    // remote-setup MQTT server: "" = automatic (see relay.h)
  uint16_t   pageSeconds;
  uint16_t   touchCal[5];   // TFT_eSPI touch calibration, saved after running it
  bool       touchCalOk;
  uint16_t   refreshMinutes;   // how often to check usage
  uint8_t    brightness;       // backlight %, while the screen is on
  uint16_t   sleepMinutes;     // idle time before the screen dims out; 0 = never
  uint16_t   dimMinutes;       // length of that fade before it turns off
  int        nAccounts;
  AccountCfg acct[MAX_ACCOUNTS];
};

extern Settings cfg;

void settingsLoad();                 // NVS, seeded from secrets.h when it changed
void settingsSave();                 // everything except tokens
void settingsSaveTokens(int i);
void settingsSaveTouchCal();
bool settingsHaveWifi();
int  settingsFindAccount(const char *alias);
void settingsRemoveAccount(int i);
void settingsFactoryReset();
void settingsRequestSetup();         // next boot starts WiFi setup; nothing is erased
bool settingsTakeSetupFlag();        // true once, then cleared
