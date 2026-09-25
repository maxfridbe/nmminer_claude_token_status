// Shared state between the network layer (api.cpp), rendering (ui.cpp) and
// the power/scheduling loop (main.cpp).
#pragma once
#include <Arduino.h>
#include <time.h>
#include "board.h"
#include "input.h"

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
  time_t weekResetsAt;   // end of the all-models weekly window, 0 if unknown
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
// Called while a check blocks, every 10ms or so: return true to give up on it.
// Without this a board looking for a network that isn't there reads nothing
// anyone presses for the half minute it spends trying.
void   apiSetInterrupt(bool (*fn)());
String apiSigninStart(const char *alias, const char *models);   // returns the claude.ai link
bool   apiSigninFinish(String pasted, String &err, String &email);
bool   apiSigninPending(String &alias, String &url);

// ui.cpp
void   uiInit();
void   uiApplyTheme();   // after cfg.lightMode changes
void   uiSplash(const char* status);
void   uiDrawAll();
void   uiDrawStatus();
void   uiSetupScreen(const char *apSsid, const char *apPass, const char *url);
// Overlays sit on top of the dashboard and survive its redraws.
enum OverlayKind { OV_NONE, OV_PIN, OV_HOTSPOT, OV_MENU, OV_REMOTE, OV_LAN };
void   uiShowPin(const char *pin);
void   uiHidePin();
void   uiShowHotspot(const char *ssid, const char *pass, const char *url);
void   uiHideHotspot();
void   uiShowMenu();
void   uiHoldProgress(float f);       // 0..1 while a hold builds, 0 when it ends
void   uiShowRemote();                 // the relay's QR, or its progress
void   uiShowLan(const char *url);     // the page's address on this network
// A full-screen list to pick from; returns the index picked. A sub line that
// starts with '!' is shown as a warning.
int    uiChoose(const char *title, const char *const *labels, const char *const *subs, int n);
void   uiCloseOverlay();
int    uiOverlay();
// Menu entries by what they do, since boards show different sets.
enum MenuAction { MA_NONE = -1, MA_PHONE, MA_WIFI_SCREEN, MA_WIFI_PHONE, MA_UPDATE, MA_CALIBRATE,
                  MA_WIPE, MA_RESTART, MA_CLOSE,
                  MA_BRIGHT_DOWN, MA_BRIGHT_UP,   // touchscreens: the - and + buttons
                  MA_BRIGHT,                      // one-button boards: step through levels
                  MA_THEME,                       // light / dark
                  MA_ROTATE };                    // landscape / portrait
MenuAction uiMenuHit(int x, int y);   // touchscreens: entry at a point, or MA_NONE
void       uiMenuNext();              // one-button boards: move the highlight
MenuAction uiMenuSelected();          // one-button boards: the highlighted entry
void       uiMenuBrightnessChanged(); // redraw the brightness control after a change
// Full-screen yes/no. Touchscreens tap a button; one-button boards tap to move
// the highlight and hold to pick.
bool       uiConfirm(const char *title, const char *line, const char *yes, const char *no);
#if TOUCH_VIA_TFT
// Corner-touch calibration; false if nobody touches within 15s when waiting.
bool       uiTouchCalibrate(uint16_t out[5], uint32_t waitMs);
#endif
bool   uiSetupButtonHit(int x, int y); // "Use this screen" on the setup screen

// wifi_screen.cpp (touchscreens): blocks for one tap; false on timeout
bool   touchWaitTap(int &x, int &y, uint32_t timeoutMs = 0);
bool   uiScroll(int delta);   // with 3+ accounts: +1 right, -1 left; true if it moved
bool   uiNextPage();          // with 3+ accounts: advance one page, wrapping around
