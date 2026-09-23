#include "relay.h"
#include "web.h"
#include "model.h"
#include "settings.h"
#include "update.h"
#include "certs.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <mbedtls/gcm.h>
#include <esp_random.h>

#define RELAY_IDLE_MS  (15UL * 60UL * 1000UL)   // closes this long after the last request
#define RETRY_MS       30000UL
#define MSG_MAX        4096                     // one sealed request or reply
#define IV_LEN         12
#define TAG_LEN        16

struct Broker { const char *code, *host; uint16_t port; const char *wss; };
static const Broker PRESETS[] = {
  {"h", "broker.hivemq.com",  8883, "wss://broker.hivemq.com:8884/mqtt"},
  {"m", "test.mosquitto.org", 8886, "wss://test.mosquitto.org:8081/mqtt"},
};

static WiFiClientSecure tls;
static PubSubClient mqtt(tls);
static RelayState state = RELAY_OFF;
static Broker custom;
static char customHost[64], customWss[96];
static const Broker *broker = nullptr;       // the one connected, or being tried
static const Broker *linked = nullptr;       // the one the QR names; kept through pauses
static int tryIndex;
static uint32_t lastTry, lastUse;
static char room[17];                        // 64 bits, hex
static uint8_t key[16];
static char topicQ[32], topicA[32];
static uint64_t lastSeq;                     // page counters are millisecond clocks
static uint8_t *inbox;                       // a request waiting to be handled
static uint8_t *plain, *sealed;              // working buffers, allocated on first use
static size_t inboxLen;

// ---------------------------------------------------------------- config

// "host:port|wss://url" -> a broker of your own.
static bool parseCustom(const char *s, Broker &b) {
  const char *colon = strchr(s, ':'), *bar = strchr(s, '|');
  if (!colon || !bar || colon > bar) return false;
  size_t hostLen = colon - s;
  if (!hostLen || hostLen >= sizeof(customHost)) return false;
  int port = atoi(colon + 1);
  if (port <= 0 || port > 65535) return false;
  if (strncmp(bar + 1, "wss://", 6) || strlen(bar + 1) >= sizeof(customWss)) return false;
  memcpy(customHost, s, hostLen);
  customHost[hostLen] = 0;
  strlcpy(customWss, bar + 1, sizeof(customWss));
  b = {"", customHost, (uint16_t)port, customWss};
  return true;
}

bool relayConfigValid(const char *s) {
  Broker b;
  return !s[0] || !strcmp(s, "hivemq") || !strcmp(s, "mosquitto") || parseCustom(s, b);
}

// The brokers to try, in order.
static int candidates(const Broker **out) {
  if (!strcmp(cfg.relay, "hivemq"))    { out[0] = &PRESETS[0]; return 1; }
  if (!strcmp(cfg.relay, "mosquitto")) { out[0] = &PRESETS[1]; return 1; }
  if (cfg.relay[0] && parseCustom(cfg.relay, custom)) { out[0] = &custom; return 1; }
  out[0] = &PRESETS[0];
  out[1] = &PRESETS[1];
  return 2;
}

// ---------------------------------------------------------------- crypto

// Sealed: IV | ciphertext | tag. The direction and room are bound in as
// associated data, so a reply can't pass for a request or move rooms.
static void aad(char dir, char *out) { out[0] = dir; memcpy(out + 1, room, 16); }

static size_t seal(char dir, const uint8_t *plain, size_t n, uint8_t *out) {
  char ad[17];
  aad(dir, ad);
  esp_fill_random(out, IV_LEN);
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
  int rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, out, IV_LEN, (const uint8_t *)ad,
                                     sizeof(ad), plain, out + IV_LEN, TAG_LEN, out + IV_LEN + n);
  mbedtls_gcm_free(&g);
  return rc ? 0 : IV_LEN + n + TAG_LEN;
}

static bool unseal(char dir, const uint8_t *in, size_t n, uint8_t *out, size_t &outLen) {
  if (n < IV_LEN + TAG_LEN) return false;
  char ad[17];
  aad(dir, ad);
  outLen = n - IV_LEN - TAG_LEN;
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
  int rc = mbedtls_gcm_auth_decrypt(&g, outLen, in, IV_LEN, (const uint8_t *)ad, sizeof(ad),
                                    in + n - TAG_LEN, TAG_LEN, in + IV_LEN, out);
  mbedtls_gcm_free(&g);
  return rc == 0;
}

static String base64url(const uint8_t *p, size_t n) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String s;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    s += A[v >> 18 & 63];
    s += A[v >> 12 & 63];
    if (i + 1 < n) s += A[v >> 6 & 63];
    if (i + 2 < n) s += A[v & 63];
  }
  return s;
}

// ---------------------------------------------------------------- MQTT

// Runs inside mqtt.loop(), whose buffer the reply will reuse: just keep a copy.
static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  if (strcmp(topic, topicQ) || inboxLen || len > MSG_MAX) return;
  memcpy(inbox, payload, len);
  inboxLen = len;
}

static bool reconnect();

static void handleInbox() {
  size_t n;
  bool ok = unseal('q', inbox, inboxLen, plain, n);
  inboxLen = 0;
  if (!ok) { Serial.println("[relay] dropped a message that didn't decrypt"); return; }

  JsonDocument req;
  if (deserializeJson(req, (const char *)plain, n)) return;
  uint64_t seq = req["n"].as<uint64_t>();   // an integer: a double would come back as 1.7e12
  if (seq <= lastSeq) { Serial.println("[relay] dropped a replayed request"); return; }
  lastSeq = seq;
  lastUse = millis();

  const char *method = req["m"] | "GET", *path = req["p"] | "";
  JsonDocument in, out;
  if (req["b"].is<JsonObject>()) in.set(req["b"]);
  int status = webApiCall(method, path, in, out);
  Serial.printf("[relay] %s %s -> %d\n", method, path, status);
  if (!reconnect()) { Serial.println("[relay] couldn't reconnect to reply"); return; }

  JsonDocument reply;
  reply["n"] = seq;
  reply["s"] = status;
  reply["b"] = out;
  n = serializeJson(reply, (char *)plain, MSG_MAX - IV_LEN - TAG_LEN);
  if (!n || n >= MSG_MAX - IV_LEN - TAG_LEN - 1) return;
  size_t len = seal('a', plain, n, sealed);
  if (len && !mqtt.publish(topicA, sealed, len)) Serial.println("[relay] reply too big to send");
}

static bool connectTo(const Broker *b) {
  broker = b;
  tls.stop();
  tls.setCACert(RELAY_ROOTS);
  tls.setTimeout(10);
  mqtt.setServer(b->host, b->port);
  char id[24];
  snprintf(id, sizeof(id), "cs-%s", room);
  Serial.printf("[relay] connecting to %s:%u\n", b->host, b->port);
  if (!mqtt.connect(id) || !mqtt.subscribe(topicQ)) {
    Serial.printf("[relay] %s failed (state %d)\n", b->host, mqtt.state());
    mqtt.disconnect();
    tls.stop();
    return false;
  }
  Serial.printf("[relay] up on %s, room %s, free heap %u\n", b->host, room, (unsigned)ESP.getFreeHeap());
  return true;
}

// After relayPause(), in the middle of a request: back on the same broker,
// so the reply reaches the page.
static bool reconnect() {
  if (mqtt.connected()) return true;
  if (!broker || !connectTo(broker)) return false;
  state = RELAY_UP;
  return true;
}

// ---------------------------------------------------------------- public

void relayPause() {
  if (state != RELAY_UP) return;
  mqtt.disconnect();
  tls.stop();
  state = RELAY_CONNECTING;                   // relayLoop() reconnects
  tryIndex = 0;
  lastTry = 0;
  Serial.printf("[relay] paused for another TLS request, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

void relayStart() {
  lastUse = millis();
  if (state == RELAY_CONNECTING || state == RELAY_UP) return;
  if (!inbox) {
    inbox  = (uint8_t *)malloc(MSG_MAX);
    plain  = (uint8_t *)malloc(MSG_MAX);
    sealed = (uint8_t *)malloc(MSG_MAX);
    mqtt.setBufferSize(MSG_MAX + 64);
    mqtt.setCallback(onMessage);
    mqtt.setKeepAlive(30);
    mqtt.setSocketTimeout(10);
  }
  uint8_t r[8];
  esp_fill_random(r, sizeof(r));
  for (int i = 0; i < 8; i++) sprintf(room + i * 2, "%02x", r[i]);
  esp_fill_random(key, sizeof(key));
  snprintf(topicQ, sizeof(topicQ), "cs1/%s/q", room);
  snprintf(topicA, sizeof(topicA), "cs1/%s/a", room);
  lastSeq = 0;
  inboxLen = 0;
  linked = nullptr;
  tryIndex = 0;
  lastTry = 0;
  state = RELAY_CONNECTING;
}

void relayStop() {
  if (state == RELAY_OFF) return;
  mqtt.disconnect();
  tls.stop();
  state = RELAY_OFF;
  broker = linked = nullptr;
  memset(key, 0, sizeof(key));
  Serial.println("[relay] off");
}

void relayLoop() {
  if (state == RELAY_OFF) return;
  // Stays while in use, and while there's no account to show.
  if (accountCount > 0 && millis() - lastUse > RELAY_IDLE_MS) { relayStop(); return; }
  if (WiFi.status() != WL_CONNECTED) return;

  if (state == RELAY_UP) {
    if (mqtt.loop()) {
      if (inboxLen) handleInbox();
      return;
    }
    Serial.println("[relay] connection lost");
    state = RELAY_CONNECTING;        // same room and key, so the QR stays good
    tryIndex = 0;
    lastTry = 0;
  }

  // Connecting: one broker per pass, then wait before going round again.
  if (lastTry && millis() - lastTry < (state == RELAY_FAILED ? RETRY_MS : 0)) return;
  const Broker *list[2];
  int n = candidates(list);
  lastTry = millis();
  if (connectTo(list[tryIndex % n])) {
    state = RELAY_UP;
    tryIndex = 0;
    linked = broker;
#ifdef RELAY_DEBUG_LINK
    Serial.printf("[relay] link %s\n", relayLink().c_str());   // test builds only: it holds the key
#endif
    return;
  }
  if (++tryIndex >= n) { state = RELAY_FAILED; tryIndex = 0; }
}

RelayState relayState() { return state; }

const char *relayBrokerName() { return state != RELAY_OFF && linked ? linked->host : ""; }

// https://owner.github.io/repo/#r=room&k=key&b=h
String relayLink() {
  const Broker *broker = linked;
  if (state == RELAY_OFF || !broker) return "";
  String repo = updateRepo();
  int slash = repo.indexOf('/');
  String owner = repo.substring(0, slash), name = repo.substring(slash + 1);
  owner.toLowerCase();
  String link = "https://" + owner + ".github.io/" + name + "/#r=" + room + "&k=" + base64url(key, 16) + "&b=";
  if (broker->code[0]) return link + broker->code;
  String w = broker->wss;                       // own server: its address, escaped
  for (int i = 0; i < (int)w.length(); i++) {
    char c = w[i];
    if (isalnum((unsigned char)c) || strchr("-._~/", c)) link += c;
    else { char e[4]; snprintf(e, sizeof(e), "%%%02X", (uint8_t)c); link += e; }
  }
  return link;
}
