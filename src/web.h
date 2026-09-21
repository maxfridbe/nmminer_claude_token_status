// The setup website, reachable two ways:
//  - the board's own WPA2 hotspot: on first boot (with a captive portal), on
//    demand after a long press, and whenever no account exists. Its password
//    changes each time and appears only on screen, so hotspot clients are
//    trusted. This is the route that works on guest networks, which isolate
//    clients from each other.
//  - the LAN, with hosting enabled. Changes there need a PIN that the display
//    shows on request.
#pragma once
#include <Arduino.h>

void   webStartSetupAP();
void   webStartLan();
void   webStartHotspot();        // alongside the WiFi connection, until idle
bool   webHotspotUp();
void   webLoop();
bool   webActive();
bool   webSetupMode();
const char *webApSsid();
const char *webApPass();
String webUrl();          // LAN address when hosting there, else the hotspot's
String webHotspotUrl();

extern bool webWantsCheck;   // an account was added or removed
