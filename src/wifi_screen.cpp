#include "wifi_screen.h"
#include "model.h"
#include "settings.h"
#include <TFT_eSPI.h>
#include <WiFi.h>

extern TFT_eSPI tft;

#define SCR_W 320
#define SCR_H 240

static uint16_t rgb(uint32_t h) { return tft.color565(h >> 16, (h >> 8) & 0xFF, h & 0xFF); }
static uint16_t cBg, cKey, cKeyHi, cText, cDim, cAccent, cBad;

// ---------------------------------------------------------------- touch

// One tap: the point where the finger first came down, reported on release.
bool touchWaitTap(int &x, int &y, uint32_t timeoutMs) {
  uint32_t start = millis();
  int tx, ty;
  while (!touchRead(tx, ty)) {
    if (timeoutMs && millis() - start > timeoutMs) return false;
    delay(10);
  }
  x = tx; y = ty;
  Serial.printf("[kb] tap %d,%d\n", x, y);   // for checking touch calibration
  int released = 0;
  while (released < 3) { released = touchRead(tx, ty) ? 0 : released + 1; delay(10); }
  return true;
}

static void header(const char *title, const char *sub = nullptr) {
  tft.fillScreen(cBg);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(cText);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(title, 10, 14);
  if (sub) {
    tft.setTextFont(1);
    tft.setTextColor(cDim);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(sub, SCR_W - 10, 14);
  }
}

static void button(int x, int y, int w, int h, const char *label, uint16_t fill, uint16_t fg) {
  tft.fillSmoothRoundRect(x, y, w, h, 8, fill, cBg);
  tft.setTextFont(2);
  tft.setTextColor(fg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2);
}

static bool inBox(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

// ---------------------------------------------------------------- keyboard

enum Layer { ABC, NUM, SYM };
static const char *ROWS[3][3] = {
  {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
  {"1234567890", "-/:;()$&@\"", ".,?!'_+"},
  {"[]{}#%^*=~", "\\|<>`'\"?!,", ".,_-+/:"},
};
#define KB_Y   62
#define KEY_H  42
#define KEY_W  32

struct Key { int x, y, w, h; char ch; int action; };   // action: 0 char, else special
enum { K_SHIFT = 1, K_BACK, K_LAYER, K_LAYER2, K_SPACE, K_CANCEL, K_OK, K_SHOW };

static int layoutKeys(Layer layer, Key *keys) {
  int n = 0;
  for (int r = 0; r < 3; r++) {
    const char *row = ROWS[layer][r];
    int len = strlen(row);
    int y = KB_Y + r * KEY_H;
    int x0 = (r == 2) ? 48 : (SCR_W - len * KEY_W) / 2;
    for (int i = 0; i < len; i++) keys[n++] = {x0 + i * KEY_W, y, KEY_W, KEY_H, row[i], 0};
  }
  int y2 = KB_Y + 2 * KEY_H, y3 = KB_Y + 3 * KEY_H;
  keys[n++] = {0, y2, 48, KEY_H, 0, layer == ABC ? K_SHIFT : K_LAYER2};
  keys[n++] = {SCR_W - 48, y2, 48, KEY_H, 0, K_BACK};
  keys[n++] = {0, y3, 56, KEY_H, 0, K_LAYER};
  keys[n++] = {56, y3, 136, KEY_H, ' ', K_SPACE};
  keys[n++] = {192, y3, 56, KEY_H, 0, K_CANCEL};
  keys[n++] = {248, y3, 72, KEY_H, 0, K_OK};
  return n;
}

static const char *keyLabel(const Key &k, Layer layer, bool shift, char *buf) {
  switch (k.action) {
    case K_SHIFT:  return shift ? "ABC" : "abc";
    case K_LAYER2: return layer == NUM ? "#+=" : "123";
    case K_BACK:   return "<-";
    case K_LAYER:  return layer == ABC ? "123" : "abc";
    case K_SPACE:  return "space";
    case K_CANCEL: return "Cancel";
    case K_OK:     return "Join";
  }
  buf[0] = shift && layer == ABC ? toupper(k.ch) : k.ch;
  buf[1] = 0;
  return buf;
}

static void drawKey(const Key &k, Layer layer, bool shift, bool pressed) {
  char buf[2];
  uint16_t fill = pressed ? cAccent : (k.action == K_OK ? cAccent : k.action ? cKeyHi : cKey);
  uint16_t fg = (pressed || k.action == K_OK) ? cBg : cText;
  tft.fillRect(k.x, k.y, k.w, k.h, cBg);
  tft.fillSmoothRoundRect(k.x + 2, k.y + 2, k.w - 4, k.h - 4, 6, fill, cBg);
  tft.setTextFont(k.action ? 2 : 4);
  tft.setTextColor(fg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(keyLabel(k, layer, shift, buf), k.x + k.w / 2, k.y + k.h / 2 + (k.action ? 0 : 1));
}

// The typed text, masked unless shown; long text scrolls to keep its end visible.
static void drawField(const String &text, bool show) {
  const int x = 8, y = 26, w = SCR_W - 16 - 56, h = 30;
  tft.fillSmoothRoundRect(x, y, w, h, 8, cKey, cBg);
  String vis = show ? text : String();
  if (!show) for (size_t i = 0; i < text.length(); i++) vis += '*';
  tft.setTextFont(2);
  while (vis.length() && tft.textWidth(vis) > w - 20) vis.remove(0, 1);
  tft.setTextColor(cText);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(vis + "_", x + 10, y + h / 2);
  button(SCR_W - 60, y, 52, h, show ? "hide" : "show", cKeyHi, cText);
}

// Returns false on Cancel. `text` holds what was typed, kept across retries.
static bool keyboard(const char *title, const char *sub, String &text, bool secret) {
  Layer layer = ABC;
  bool shift = false, show = !secret;
  Key keys[48];
  int n = 0;
  bool redraw = true;

  for (;;) {
    if (redraw) {
      header(title, sub);
      n = layoutKeys(layer, keys);
      for (int i = 0; i < n; i++) drawKey(keys[i], layer, shift, false);
      redraw = false;
    }
    drawField(text, show);

    int x, y;
    touchWaitTap(x, y);
    if (inBox(x, y, SCR_W - 60, 26, 52, 30)) { show = !show; continue; }

    for (int i = 0; i < n; i++) {
      const Key &k = keys[i];
      if (!inBox(x, y, k.x, k.y, k.w, k.h)) continue;
      drawKey(k, layer, shift, true);            // brief press feedback
      delay(70);
      drawKey(k, layer, shift, false);
      switch (k.action) {
        case 0:
          if (text.length() < 63) text += (shift && layer == ABC) ? (char)toupper(k.ch) : k.ch;
          break;
        case K_SPACE:  if (text.length() < 63) text += ' '; break;
        case K_BACK:   if (text.length()) text.remove(text.length() - 1); break;
        case K_SHIFT:  shift = !shift; redraw = true; break;
        case K_LAYER:  layer = layer == ABC ? NUM : ABC; redraw = true; break;
        case K_LAYER2: layer = layer == NUM ? SYM : NUM; redraw = true; break;
        case K_CANCEL: return false;
        case K_OK:     return true;
      }
      break;
    }
  }
}

// ---------------------------------------------------------------- network list

struct Net { char ssid[33]; int rssi; bool open; };
#define LIST_Y0   30
#define ROW_H     34
#define ROWS_PG   5

static int scan(Net *nets, int max) {
  header("Choose WiFi", "scanning...");
  int found = WiFi.scanNetworks(false, false);
  int n = 0;
  for (int i = 0; i < found && n < max; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int j = 0; j < n; j++) if (s == nets[j].ssid) { dup = true; break; }
    if (dup) continue;
    strlcpy(nets[n].ssid, s.c_str(), sizeof(nets[n].ssid));
    nets[n].rssi = WiFi.RSSI(i);
    nets[n].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    n++;
  }
  WiFi.scanDelete();
  for (int i = 1; i < n; i++)                     // strongest first
    for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; j--) { Net t = nets[j]; nets[j] = nets[j - 1]; nets[j - 1] = t; }
  return n;
}

static void drawBars(int x, int y, int rssi) {
  int bars = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    if (i < bars) tft.fillRect(x + i * 5, y + 13 - h, 3, h, cText);
    else          tft.drawRect(x + i * 5, y + 13 - h, 3, h, cDim);
  }
}

// Returns the chosen index, -2 for "Other network", or -1 to cancel.
static int pickNetwork(Net *nets, int n) {
  int page = 0, pages = (n + 1 + ROWS_PG - 1) / ROWS_PG;   // +1 for "Other network"
  for (;;) {
    char sub[24];
    snprintf(sub, sizeof(sub), "%d found  %d/%d", n, page + 1, max(pages, 1));
    header("Choose WiFi", sub);
    for (int r = 0; r < ROWS_PG; r++) {
      int i = page * ROWS_PG + r, y = LIST_Y0 + r * ROW_H;
      if (i > n) break;
      tft.fillSmoothRoundRect(8, y + 2, SCR_W - 16, ROW_H - 4, 8, cKey, cBg);
      tft.setTextFont(2);
      tft.setTextDatum(ML_DATUM);
      if (i == n) {
        tft.setTextColor(cDim);
        tft.drawString("Other network...", 20, y + ROW_H / 2);
        continue;
      }
      tft.setTextColor(cText);
      String s = nets[i].ssid;
      while (s.length() > 1 && tft.textWidth(s) > 220) s.remove(s.length() - 1);
      tft.drawString(s, 20, y + ROW_H / 2);
      tft.setTextFont(1);
      tft.setTextColor(cDim);
      tft.setTextDatum(MR_DATUM);
      if (nets[i].open) tft.drawString("open", SCR_W - 50, y + ROW_H / 2);
      drawBars(SCR_W - 40, y + 10, nets[i].rssi);
    }
    const int by = SCR_H - 38;
    button(8, by, 70, 32, "< Prev", cKeyHi, page > 0 ? cText : cDim);
    button(84, by, 70, 32, "Next >", cKeyHi, page < pages - 1 ? cText : cDim);
    button(160, by, 72, 32, "Rescan", cKeyHi, cText);
    button(238, by, 74, 32, "Cancel", cKeyHi, cText);

    int x, y;
    touchWaitTap(x, y);
    if (y >= by) {
      if (x < 78)       { if (page > 0) page--; }
      else if (x < 154) { if (page < pages - 1) page++; }
      else if (x < 232) return -3;             // rescan
      else              return -1;
      continue;
    }
    int r = (y - LIST_Y0) / ROW_H, i = page * ROWS_PG + r;
    if (y >= LIST_Y0 && r < ROWS_PG && i <= n) return i == n ? -2 : i;
  }
}

// ---------------------------------------------------------------- join

static bool tryJoin(const char *ssid, const char *pass) {
  header("Joining", ssid);
  tft.setTextFont(2);
  tft.setTextColor(cDim);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting...", SCR_W / 2, SCR_H / 2);

  bool ap = WiFi.getMode() & WIFI_MODE_AP;
  WiFi.disconnect(false);
  WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t start = millis();
  wl_status_t st;
  while ((st = WiFi.status()) != WL_CONNECTED && millis() - start < 20000) {
    if (st == WL_CONNECT_FAILED && millis() - start > 4000) break;
    delay(100);
  }
  return st == WL_CONNECTED;
}

static bool message(const char *title, const char *line, const char *ok, const char *cancel) {
  header(title);
  tft.setTextFont(2);
  tft.setTextColor(cText);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line, SCR_W / 2, 100);
  button(40, 150, 110, 40, ok, cAccent, cBg);
  if (cancel) button(170, 150, 110, 40, cancel, cKeyHi, cText);
  for (;;) {
    int x, y;
    touchWaitTap(x, y);
    if (inBox(x, y, 40, 150, 110, 40)) return true;
    if (cancel && inBox(x, y, 170, 150, 110, 40)) return false;
  }
}

bool screenWifiSetup() {
  cBg = rgb(0x000000); cKey = rgb(0x1C1C22); cKeyHi = rgb(0x2A2A33);
  cText = rgb(0xF4F4F7); cDim = rgb(0x8A8A96); cAccent = rgb(0xD97757); cBad = rgb(0xFF4B4B);

  String oldSsid = cfg.ssid, oldPass = cfg.pass;
  static Net nets[20];
  int n = scan(nets, 20);

  for (;;) {
    int pick = pickNetwork(nets, n);
    if (pick == -3) { n = scan(nets, 20); continue; }
    if (pick == -1) break;

    String ssid, pass;
    bool open = false;
    if (pick == -2) {
      if (!keyboard("Network name", "hidden network", ssid, false) || !ssid.length()) continue;
    } else {
      ssid = nets[pick].ssid;
      open = nets[pick].open;
    }

    for (;;) {
      if (!open && !keyboard("Password", ssid.c_str(), pass, true)) break;
      if (tryJoin(ssid.c_str(), pass.c_str())) {
        strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
        strlcpy(cfg.pass, pass.c_str(), sizeof(cfg.pass));
        settingsSave();
        Serial.printf("[wifi] joined '%s' from the touchscreen, saved\n", cfg.ssid);
        return true;
      }
      Serial.printf("[wifi] touchscreen join of '%s' failed\n", ssid.c_str());
      if (open || !message("Couldn't join", "Check the password and try again.", "Try again", "Back")) break;
    }
  }

  // Cancelled: back to the network we had.
  if (oldSsid.length() && WiFi.status() != WL_CONNECTED) {
    WiFi.begin(oldSsid.c_str(), oldPass.c_str());
  }
  return false;
}
