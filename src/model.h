// Shared state between the network layer (api.cpp), rendering (ui.cpp) and
// the power/scheduling loop (main.cpp).
#pragma once
#include <Arduino.h>
#include <time.h>

#define MAX_ACCOUNTS 4   // 3+ scroll sideways, two at a time
#define MAX_BUCKETS  6

// One usage meter to display. Bucket 0 is the session window when present;
// the rest are the models configured for that account.
struct Bucket {
  char   label[24];   // "Session", or the server's model display name
  float  util;        // percent of the limit used, 0-100
  time_t resetsAt;    // UTC epoch seconds, 0 when the API gives none
  bool   shared;      // model has no limit of its own: shows the all-models weekly
};

struct Account {
  char   label[24];
  char   plan[12];
  Bucket buckets[MAX_BUCKETS];
  int    nBuckets;
  bool   ok;          // the most recent check succeeded
  bool   everOk;      // buckets hold real data from some earlier check
  char   error[48];
  time_t fetchedAt;
};

enum NetStatus { NET_IDLE, NET_CHECKING, NET_OK, NET_PARTIAL, NET_FAILED };

// The WiFi link, shown on the right edge. WiFi stays connected between checks.
struct Link {
  char ssid[33];
  char url[40];       // setup page address when it's being served, else ""

  int  rssi;          // dBm, last reading while connected
  bool up;
};

extern Account   accounts[MAX_ACCOUNTS];
extern int       accountCount;
extern NetStatus netStatus;
extern time_t    lastGoodCheck;
extern bool      networkDown;   // last check never reached the API (WiFi/NTP)
extern Link      wifiLink;

// api.cpp
void apiInit();
void apiCheckAll();
void apiPollLink();   // refresh wifiLink.up / wifiLink.rssi without touching the API
void   apiSyncAccounts();                                        // settings -> display model
bool   apiWifiUp();                                              // join the configured WiFi
String apiSigninStart(const char *alias, const char *models);   // returns the claude.ai link
bool   apiSigninFinish(String pasted, String &err, String &email);
bool   apiSigninPending(String &alias, String &url);

// ui.cpp
void   uiInit();
void   uiSplash(const char* status);
void   uiDrawAll();
void   uiDrawStatus();
void   uiSetupScreen(const char *apSsid, const char *apPass, const char *url);
// Overlays sit on top of the dashboard and survive its redraws.
enum OverlayKind { OV_NONE, OV_PIN, OV_HOTSPOT, OV_MENU };
void   uiShowPin(const char *pin);
void   uiHidePin();
void   uiShowHotspot(const char *ssid, const char *pass, const char *url);
void   uiHideHotspot();
void   uiShowMenu();
void   uiCloseOverlay();
int    uiOverlay();
int    uiMenuHit(int x, int y);       // menu row at a touch point, or -1
bool   uiSetupButtonHit(int x, int y); // "Use this screen" on the setup screen

// main.cpp: one calibrated touch sample, false when not pressed
bool   touchRead(int &x, int &y);
bool   uiScroll(int delta);   // with 3+ accounts: +1 right, -1 left; true if it moved
bool   uiNextPage();          // with 3+ accounts: advance one page, wrapping around
