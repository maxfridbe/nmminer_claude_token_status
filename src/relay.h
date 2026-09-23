// Remote setup through a public MQTT server, for when a phone can't reach the
// board: guest networks keep devices apart, and the board's own hotspot means
// switching WiFi.
//
// The display shows a QR code for the setup page on GitHub Pages, with a
// random room and a fresh AES key after the '#'. Browsers never send that
// part to any server. The page and the board then trade the setup page's
// usual API calls through the MQTT server, sealed with AES-128-GCM, so the
// server only ever sees ciphertext. Each request carries a rising counter,
// so a copy replayed by anyone watching the topic is ignored.
//
// cfg.relay picks the server: "" tries HiveMQ's public broker, then
// Mosquitto's; "hivemq" or "mosquitto" pins one; "host:port|wss://url" names
// your own (its certificate must chain to a root in RELAY_ROOTS).
#pragma once
#include <Arduino.h>

enum RelayState { RELAY_OFF, RELAY_CONNECTING, RELAY_UP, RELAY_FAILED };

void       relayStart();          // new room and key, unless already running
void       relayStop();
void       relayLoop();
RelayState relayState();
String     relayLink();           // the page link for the QR; "" until up
const char *relayBrokerName();    // host of the server in use, "" when off
bool       relayConfigValid(const char *cfg);
