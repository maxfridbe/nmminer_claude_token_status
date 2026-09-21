// The setup website: a WPA2 hotspot with a captive portal on first boot, and
// (with hosting enabled, or while no account is set up) a page on the LAN for
// adding accounts via Claude's copy-paste sign-in. Changes on the LAN need a
// PIN that the display shows on request.
#pragma once
#include <Arduino.h>

void   webStartSetupAP();
void   webStartLan();
void   webLoop();
bool   webActive();
bool   webSetupMode();
const char *webApSsid();
const char *webApPass();
String webUrl();          // "http://<ip>", or "" when not serving

extern bool webWantsCheck;   // an account was added or removed
