// WiFi setup on the touchscreen itself: pick a network from a scan, type the
// password on an on-screen keyboard, and join. Only the WiFi settings change;
// accounts and logins are untouched.
#pragma once

// Blocks until the user joins a network (saved; returns true) or cancels.
bool screenWifiSetup();
