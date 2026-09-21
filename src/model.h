// Shared state between the network layer (api.cpp), rendering (ui.cpp) and
// the power/scheduling loop (main.cpp).
#pragma once
#include <Arduino.h>
#include <time.h>

#define MAX_ACCOUNTS 2
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

// ui.cpp
void   uiInit();
void   uiSplash(const char* status);
void   uiDrawAll();
void   uiDrawStatus();
