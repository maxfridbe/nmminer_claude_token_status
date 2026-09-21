// Rendering: light on dark. Two columns on pure black, one per account, and
// a narrow strip on the right edge for the WiFi link. White text; meters
// colored green through amber to red as limits fill. Everything is composed
// in off-screen sprites and pushed in one go, so redraws never flicker.

#include "model.h"
#include <TFT_eSPI.h>
#include "claude_mark.h"

extern TFT_eSPI tft;

#define SCR_W  320
#define SCR_H  240
#define LINK_W 14                        // right-edge WiFi strip
#define COL_W  ((SCR_W - LINK_W) / 2)    // 153
#define PAD    12

#define NAME_Y   18
#define PLAN_Y   38
#define RING_CY  98
#define RING_R   44
#define RING_IR  34
#define RING_A0  30     // TFT_eSPI arcs: 0 deg at 6 o'clock, clockwise
#define RING_A1  330
#define RESET_Y  156
#define ROW_Y0   174
#define ROW_H    32
#define MAX_ROWS 2
#define BAR_H    9                // odd, so the round caps center on a pixel

static uint16_t rgb(uint32_t hex) {
  return tft.color565((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static uint16_t C_BG, C_TEXT, C_SOFT, C_DIM, C_FAINT, C_TRACK, C_RULE;
static uint16_t C_GOOD, C_WARN, C_HOT, C_INFO, C_MARK;

static TFT_eSprite colSpr  = TFT_eSprite(&tft);
static TFT_eSprite linkSpr = TFT_eSprite(&tft);
static bool haveColSpr = false, haveLinkSpr = false;

void uiInit() {
  C_BG    = rgb(0x000000);
  C_TEXT  = rgb(0xFFFFFF);
  C_SOFT  = rgb(0xD6D6D6);
  C_DIM   = rgb(0x8A8A8A);
  C_FAINT = rgb(0x4A4A4A);
  C_TRACK = rgb(0x262626);
  C_RULE  = rgb(0x1C1C1C);
  C_GOOD  = rgb(0x2EE59D);
  C_WARN  = rgb(0xFFB020);
  C_HOT   = rgb(0xFF4B4B);
  C_INFO  = rgb(0x4DA3FF);
  C_MARK  = rgb(0xD97757);      // hsl(14.8, 63.1%, 59.6%), the mark's own fill

  // Allocated once, before WiFi and TLS claim heap.
  colSpr.setColorDepth(16);
  haveColSpr = colSpr.createSprite(COL_W, SCR_H) != nullptr;
  linkSpr.setColorDepth(16);
  haveLinkSpr = linkSpr.createSprite(LINK_W, SCR_H) != nullptr;
  Serial.printf("[ui] sprites col=%d link=%d, free heap %u\n", haveColSpr, haveLinkSpr,
                (unsigned)ESP.getFreeHeap());
}

// ---------------------------------------------------------------- helpers

// Green up to half, blending to amber by 75%, and to red by 100%.
static uint16_t meterColor(float pct) {
  if (pct <= 50) return C_GOOD;
  if (pct <= 75) return tft.alphaBlend((uint8_t)((pct - 50) / 25 * 255), C_WARN, C_GOOD);
  return tft.alphaBlend((uint8_t)(min(pct - 75, 25.0f) / 25 * 255), C_HOT, C_WARN);
}

// Anti-aliased mark: each pixel blends the mark color over bg by coverage.
static void drawMark(TFT_eSPI &g, int x, int y, int size, const uint8_t *alpha) {
  for (int r = 0; r < size; r++)
    for (int c = 0; c < size; c++) {
      uint8_t a = alpha[r * size + c];
      if (a) g.drawPixel(x + c, y + r, a == 255 ? C_MARK : tft.alphaBlend(a, C_MARK, C_BG));
    }
}

// Relative for anything inside a day, weekday + clock beyond that.
static void fmtReset(time_t t, bool compact, char *out, size_t n) {
  out[0] = 0;
  if (!t) return;
  long d = (long)(t - time(nullptr));
  if (d <= 0)   { snprintf(out, n, "now"); return; }
  if (d < 3600) { snprintf(out, n, "%ldm", d / 60 + 1); return; }
  if (d < 86400) {
    snprintf(out, n, compact ? "%ldh%02ldm" : "%ldh %02ldm", d / 3600, (d % 3600) / 60);
    return;
  }
  struct tm lt;
  localtime_r(&t, &lt);
  static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int h = lt.tm_hour % 12;
  if (!h) h = 12;
  const char *ap = lt.tm_hour < 12 ? (compact ? "a" : " AM") : (compact ? "p" : " PM");
  if (lt.tm_min) snprintf(out, n, "%s %d:%02d%s", DAYS[lt.tm_wday], h, lt.tm_min, ap);
  else           snprintf(out, n, "%s %d%s", DAYS[lt.tm_wday], h, ap);
}

// Trim to fit, marking the cut with '~' (the built-in fonts have no ellipsis).
static String fit(TFT_eSPI &g, String s, int maxW) {
  if (g.textWidth(s) <= maxW) return s;
  while (s.length() > 1 && g.textWidth(s + "~") > maxW) s.remove(s.length() - 1);
  return s + "~";
}

static void drawErrorLine(TFT_eSPI &g, int cx, int y, int maxW, const char *msg) {
  g.setTextColor(C_HOT);
  g.setTextDatum(MC_DATUM);
  g.setTextFont(2);
  if (g.textWidth(msg) <= maxW) { g.drawString(msg, cx, y); return; }

  g.setTextFont(1);
  String s = msg;
  int cut = s.length();
  while (cut > 0 && g.textWidth(s.substring(0, cut)) > maxW) {
    int sp = s.lastIndexOf(' ', cut - 1);
    cut = sp > 0 ? sp : cut - 1;
  }
  g.drawString(s.substring(0, cut), cx, y - 5);
  g.drawString(fit(g, s.substring(cut + (s[cut] == ' ')), maxW), cx, y + 5);
}

// ---------------------------------------------------------------- column

// The fill is a gradient along the meter's own color scale: a bar at 30% is
// all green, one at 95% runs green through amber into red.
static void drawBar(TFT_eSPI &g, int x, int y, int w, float pct, bool stale) {
  const int r = BAR_H / 2;
  g.fillSmoothRoundRect(x, y, w, BAR_H, r, C_TRACK, C_BG);
  int fw = (int)(w * pct / 100.0f + 0.5f);
  if (pct <= 0 || fw <= 0) return;
  if (fw < BAR_H) fw = BAR_H;

  uint16_t c0 = stale ? C_FAINT : meterColor(0);
  uint16_t c1 = stale ? C_FAINT : meterColor(pct);
  g.fillSmoothCircle(x + r, y + r, r, c0, C_TRACK);
  g.fillSmoothCircle(x + fw - 1 - r, y + r, r, c1, C_TRACK);
  if (fw - 2 * r > 0) g.fillRectHGradient(x + r, y, fw - 2 * r, BAR_H, c0, c1);
}

// Model name and reset time on top, meter with percentage beneath.
static void drawRow(TFT_eSPI &g, int ox, int y, const Bucket &b, bool stale) {
  const int x0 = ox + PAD, x1 = ox + COL_W - PAD;

  char rs[16];
  fmtReset(b.resetsAt, true, rs, sizeof(rs));
  g.setTextFont(1);
  g.setTextColor(C_DIM);
  g.setTextDatum(TR_DATUM);
  int rsW = 0;
  if (rs[0]) { g.drawString(rs, x1, y + 4); rsW = g.textWidth(rs) + 8; }

  g.setTextFont(2);
  g.setTextDatum(TL_DATUM);
  g.setTextColor(stale ? C_DIM : C_SOFT);
  String name = fit(g, b.label, x1 - x0 - rsW);
  g.drawString(name, x0, y);

  // A model with no limit of its own is drawing on the all-models weekly.
  if (b.shared) {
    int nx = x0 + g.textWidth(name) + 5;
    g.setTextFont(1);
    if (nx + g.textWidth("shared") <= x1 - rsW) {
      g.setTextColor(C_FAINT);
      g.drawString("shared", nx, y + 4);
    }
  }

  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)(b.util + 0.5f));
  g.setFreeFont(&FreeSansBold9pt7b);
  int pw = g.textWidth("100%");
  drawBar(g, x0, y + 19, x1 - x0 - pw - 6, b.util, stale);
  g.setTextColor(stale ? C_DIM : C_TEXT);
  g.setTextDatum(MR_DATUM);
  g.drawString(pct, x1, y + 23);
}

static void drawColumn(TFT_eSPI &g, int ox, const Account &a, bool ruleRight) {
  const int cx = ox + COL_W / 2;
  const bool stale = !a.ok && a.everOk;

  g.fillRect(ox, 0, COL_W, SCR_H, C_BG);
  if (ruleRight) g.drawFastVLine(ox + COL_W - 1, 14, SCR_H - 28, C_RULE);

  // Name with the mark, plan beneath.
  drawMark(g, ox + PAD, NAME_Y - 9, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
  g.setFreeFont(&FreeSansBold9pt7b);
  g.setTextColor(C_TEXT);
  g.setTextDatum(ML_DATUM);
  g.drawString(fit(g, a.label, COL_W - PAD * 2 - 24), ox + PAD + 24, NAME_Y);
  g.setTextFont(2);
  g.setTextColor(C_DIM);
  g.drawString(a.plan, ox + PAD + 24, PLAN_Y);

  // Session ring
  const Bucket *s = (a.everOk && a.nBuckets && !strcmp(a.buckets[0].label, "Session"))
                        ? &a.buckets[0] : nullptr;
  g.drawSmoothArc(cx, RING_CY, RING_R, RING_IR, RING_A0, RING_A1, C_TRACK, C_BG, true);
  if (s) {
    int end = RING_A0 + (int)((RING_A1 - RING_A0) * s->util / 100.0f + 0.5f);
    if (end > RING_A0 + 1)
      g.drawSmoothArc(cx, RING_CY, RING_R, RING_IR, RING_A0, end,
                      stale ? C_FAINT : meterColor(s->util), C_BG, true);
  }
  char pct[8];
  if (s) snprintf(pct, sizeof(pct), "%d%%", (int)(s->util + 0.5f));
  else   snprintf(pct, sizeof(pct), "--");
  g.setFreeFont(&FreeSansBold18pt7b);
  g.setTextColor(s && !stale ? C_TEXT : C_FAINT);
  g.setTextDatum(MC_DATUM);
  g.drawString(pct, cx, RING_CY - 6);
  g.setTextFont(2);
  g.setTextColor(C_DIM);
  g.drawString("session", cx, RING_CY + 17);

  // Under the ring: when the session resets, or what went wrong.
  if (!a.ok && a.error[0]) {
    drawErrorLine(g, cx, RESET_Y, COL_W - PAD * 2, a.error);
  } else if (s) {
    char rs[20];
    fmtReset(s->resetsAt, false, rs, sizeof(rs));
    if (rs[0]) {
      const char *pre = strcmp(rs, "now") ? "resets in " : "resets ";
      g.setTextFont(2);
      int w1 = g.textWidth(pre), w2 = g.textWidth(rs);
      int x = cx - (w1 + w2) / 2;
      g.setTextDatum(ML_DATUM);
      g.setTextColor(C_DIM);
      g.drawString(pre, x, RESET_Y);
      g.setTextColor(C_SOFT);
      g.drawString(rs, x + w1, RESET_Y);
    }
  }

  // Model meters
  int first = s ? 1 : 0;
  int no = a.everOk ? a.nBuckets - first : 0;
  int shown = no <= MAX_ROWS ? no : MAX_ROWS - 1;
  for (int r = 0; r < shown; r++)
    drawRow(g, ox, ROW_Y0 + r * ROW_H, a.buckets[first + r], stale);
  if (no > shown) {
    char more[20];
    snprintf(more, sizeof(more), "+%d more", no - shown);
    g.setTextFont(2);
    g.setTextColor(C_DIM);
    g.setTextDatum(TL_DATUM);
    g.drawString(more, ox + PAD, ROW_Y0 + shown * ROW_H);
  }
}

// ---------------------------------------------------------------- link strip

// WiFi name written top to bottom (text rotated 90 degrees clockwise), and
// a narrow signal meter along the edge that fills from the bottom.
static void drawLinkStrip(TFT_eSPI &g, int ox) {
  g.fillRect(ox, 0, LINK_W, SCR_H, C_BG);

  const int top = 14, bottom = SCR_H - 14;
  const int barX = ox + LINK_W - 4, barW = 3;

  // -100 dBm or worse reads as empty, -40 dBm or better as full.
  int q = wifiLink.up ? constrain((wifiLink.rssi + 100) * 100 / 60, 0, 100) : 0;
  uint16_t qc = meterColor(100 - q);
  g.fillSmoothRoundRect(barX, top, barW, bottom - top, 1, C_TRACK, C_BG);
  int fh = (bottom - top) * q / 100;
  if (fh > 0) g.fillSmoothRoundRect(barX, bottom - fh, barW, fh, 1, qc, C_BG);

  // Render the name flat in a scratch sprite, then copy it rotated.
  const char *name = wifiLink.ssid[0] ? wifiLink.ssid : "no WiFi";
  TFT_eSprite txt = TFT_eSprite(&tft);
  txt.setColorDepth(16);
  txt.setTextFont(1);
  int tw = min((int)txt.textWidth(name), bottom - top), th = 8;
  if (!txt.createSprite(tw, th)) return;
  txt.fillSprite(C_BG);
  txt.setTextColor(wifiLink.up ? C_DIM : C_HOT, C_BG);
  txt.setTextDatum(TL_DATUM);
  txt.drawString(name, 0, 0);
  const int tx0 = ox + 1;
  for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++) {
      uint16_t c = txt.readPixel(x, y);
      if (c != C_BG) g.drawPixel(tx0 + (th - 1 - y), top + x, c);
    }
  txt.deleteSprite();
}

// ---------------------------------------------------------------- public

// The only status is a small dot, top right of the accounts, while a check runs.
void uiDrawStatus() {
  uint16_t c = netStatus == NET_CHECKING ? C_INFO : C_BG;
  tft.fillSmoothCircle(2 * COL_W - 8, 7, 3, c, C_BG);
}

void uiDrawAll() {
  for (int i = 0; i < accountCount; i++) {
    int x = accountCount == 1 ? (2 * COL_W - COL_W) / 2 : i * COL_W;
    bool rule = accountCount > 1 && i == 0;
    if (haveColSpr) {
      drawColumn(colSpr, 0, accounts[i], rule);
      colSpr.pushSprite(x, 0);
    } else {
      drawColumn(tft, x, accounts[i], rule);
    }
  }
  if (haveLinkSpr) { drawLinkStrip(linkSpr, 0); linkSpr.pushSprite(2 * COL_W, 0); }
  else             drawLinkStrip(tft, 2 * COL_W);
  uiDrawStatus();
}

void uiSplash(const char *status) {
  tft.fillScreen(C_BG);
  drawMark(tft, (SCR_W - CLAUDE_MARK_60) / 2, 56, CLAUDE_MARK_60, CLAUDE_MARK_60_ALPHA);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(status, SCR_W / 2, 150);
}
