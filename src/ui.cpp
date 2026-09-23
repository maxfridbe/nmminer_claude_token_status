// Rendering: light on dark, on pure black, with a narrow WiFi strip on the
// right edge. One account fills the screen; two sit side by side; three or
// more show two at a time and scroll sideways. White text; meters colored
// green through amber to red as limits fill. Everything is composed in one
// column-sized off-screen sprite and pushed in pieces, so redraws never
// flicker and the wide layout needs no bigger buffer.

#include "model.h"
#include <TFT_eSPI.h>
#include "claude_mark.h"
#include "settings.h"
#include "web.h"
#include "update.h"
#include "relay.h"
#include <WiFi.h>
#include <qrcode.h>

extern TFT_eSPI tft;

// Screen size comes from board.h. The CYD shows two 153px columns and a WiFi
// strip; square screens show one account per page.
#if SQUARE_SCREEN
#define LINK_W 0
#define COL_W  SCR_W
#else
#define LINK_W 14                        // right-edge WiFi strip
#define COL_W  ((SCR_W - LINK_W) / 2)    // 153
#endif
#define PAD    12

#define RING_A0  30     // TFT_eSPI arcs: 0 deg at 6 o'clock, clockwise
#define RING_A1  330
#define MAX_ROWS 2

// 480x320 screens get the same layout, scaled up, and render each pane in
// two halves so the off-screen buffer stays small next to TLS.
#define BIG_SCREEN (SCR_H >= 320)
#if BIG_SCREEN
#define NAME_Y   24
#define PLAN_Y   48
#define RING_CY  132
#define RING_R   62
#define RING_IR  48
#define RESET_Y  212
#define WEEK_Y   230
#define ROW_Y0   244
#define ROW_H    36
#define BAR_H    11
#define RING_FONT  (&FreeSansBold24pt7b)
#define RING_LABEL 26
#else
#define NAME_Y   18
#define PLAN_Y   38
#define RING_CY  98
#define RING_R   44
#define RING_IR  34
#define RESET_Y  154
#define WEEK_Y   168              // thin week-progress bar under the reset line
#define ROW_Y0   178
#define ROW_H    31
#define BAR_H    9                // odd, so the round caps center on a pixel
#define RING_FONT  (&FreeSansBold18pt7b)
#define RING_LABEL 17
#endif

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
#if SQUARE_SCREEN
  haveColSpr = colSpr.createSprite(SCR_W, SCR_H / 2) != nullptr;   // drawn in two halves
#elif BIG_SCREEN
  haveColSpr = colSpr.createSprite(COL_W, SCR_H / 2) != nullptr;   // drawn in two halves
  linkSpr.setColorDepth(16);
  haveLinkSpr = linkSpr.createSprite(LINK_W, SCR_H) != nullptr;
#else
  haveColSpr = colSpr.createSprite(COL_W, SCR_H) != nullptr;
  linkSpr.setColorDepth(16);
  haveLinkSpr = linkSpr.createSprite(LINK_W, SCR_H) != nullptr;
#endif
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
static void drawBar(TFT_eSPI &g, int x, int y, int w, int h, float pct, bool stale) {
  const int r = h / 2;
  g.fillSmoothRoundRect(x, y, w, h, r, C_TRACK, C_BG);
  int fw = (int)(w * pct / 100.0f + 0.5f);
  if (pct <= 0 || fw <= 0) return;
  if (fw < h) fw = h;

  uint16_t c0 = stale ? C_FAINT : meterColor(0);
  uint16_t c1 = stale ? C_FAINT : meterColor(pct);
  g.fillSmoothCircle(x + r, y + r, r, c0, C_TRACK);
  g.fillSmoothCircle(x + fw - 1 - r, y + r, r, c1, C_TRACK);
  if (fw - 2 * r > 0) g.fillRectHGradient(x + r, y, fw - 2 * r, h, c0, c1);
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
  drawBar(g, x0, y + 19, x1 - x0 - pw - 6, BAR_H, b.util, stale);
  g.setTextColor(stale ? C_DIM : C_TEXT);
  g.setTextDatum(MR_DATUM);
  g.drawString(pct, x1, y + 23);
}

static const Bucket *sessionOf(const Account &a) {
  return (a.everOk && a.nBuckets && !strcmp(a.buckets[0].label, "Session")) ? &a.buckets[0] : nullptr;
}

// Mark and name, plan beneath.
static void drawHeader(TFT_eSPI &g, int ox, int oy, const Account &a, int maxW) {
  drawMark(g, ox + PAD, oy + NAME_Y - 9, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
  // Long names drop to the smaller font before anything gets cut off.
  const int nameW = maxW - PAD * 2 - 24;
  g.setFreeFont(&FreeSansBold9pt7b);
  if (g.textWidth(a.label) > nameW) g.setTextFont(2);
  g.setTextColor(C_TEXT);
  g.setTextDatum(ML_DATUM);
  g.drawString(fit(g, a.label, nameW), ox + PAD + 24, oy + NAME_Y);
  g.setTextFont(2);
  g.setTextColor(C_DIM);
  g.drawString(a.plan, ox + PAD + 24, oy + PLAN_Y);
}

static void drawRing(TFT_eSPI &g, int cx, int cy, int r, int ir, const Bucket *s, bool stale,
                     const GFXfont *font, int labelDy) {
  g.drawSmoothArc(cx, cy, r, ir, RING_A0, RING_A1, C_TRACK, C_BG, true);
  if (s) {
    int end = RING_A0 + (int)((RING_A1 - RING_A0) * s->util / 100.0f + 0.5f);
    if (end > RING_A0 + 1)
      g.drawSmoothArc(cx, cy, r, ir, RING_A0, end, stale ? C_FAINT : meterColor(s->util), C_BG, true);
  }
  char pct[8];
  if (s) snprintf(pct, sizeof(pct), "%d%%", (int)(s->util + 0.5f));
  else   snprintf(pct, sizeof(pct), "--");
  g.setFreeFont(font);
  g.setTextColor(s && !stale ? C_TEXT : C_FAINT);
  g.setTextDatum(MC_DATUM);
  g.drawString(pct, cx, cy - 6);
  g.setTextFont(2);
  g.setTextColor(C_DIM);
  g.drawString("session", cx, cy + labelDy);
}

// When the session resets, or what went wrong.
static void drawResetLine(TFT_eSPI &g, int cx, int y, int maxW, const Account &a, const Bucket *s) {
  if (!a.ok && a.error[0]) { drawErrorLine(g, cx, y, maxW, a.error); return; }
  if (!s) return;
  char rs[20];
  fmtReset(s->resetsAt, false, rs, sizeof(rs));
  if (!rs[0]) return;
  const char *pre = strcmp(rs, "now") ? "resets in " : "resets ";
  g.setTextFont(2);
  int w1 = g.textWidth(pre), w2 = g.textWidth(rs);
  int x = cx - (w1 + w2) / 2;
  g.setTextDatum(ML_DATUM);
  g.setTextColor(C_DIM);
  g.drawString(pre, x, y);
  g.setTextColor(C_SOFT);
  g.drawString(rs, x + w1, y);
}

// How far into the weekly window we are, so a meter's % can be read against
// the time that's gone. Neutral grey: it measures time, not usage.
static void drawWeekBar(TFT_eSPI &g, int x0, int x1, int y, const Account &a) {
  if (!a.everOk || !a.weekResetsAt) return;
  const long WEEK = 7L * 86400L;
  long left = (long)(a.weekResetsAt - time(nullptr));
  left = constrain(left, 0L, WEEK);

  char txt[20];
  if (left >= 86400) snprintf(txt, sizeof(txt), "%ldd %ldh left", left / 86400, (left % 86400) / 3600);
  else               snprintf(txt, sizeof(txt), "%ldh %02ldm left", left / 3600, (left % 3600) / 60);
  g.setTextFont(1);
  g.setTextColor(C_DIM);
  g.setTextDatum(MR_DATUM);
  g.drawString(txt, x1, y + 1);

  int w = x1 - x0 - g.textWidth(txt) - 6;
  if (w < 20) return;
  g.fillRoundRect(x0, y, w, 3, 1, C_TRACK);
  int fw = (int)(w * (float)(WEEK - left) / WEEK + 0.5f);
  if (fw > 0) g.fillRoundRect(x0, y, max(fw, 3), 3, 1, C_DIM);
}

static void drawColumn(TFT_eSPI &g, int ox, int oy, const Account &a, bool ruleRight) {
  const int cx = ox + COL_W / 2;
  const bool stale = !a.ok && a.everOk;
  const Bucket *s = sessionOf(a);

  g.fillRect(ox, oy, COL_W, SCR_H, C_BG);
  if (ruleRight) g.drawFastVLine(ox + COL_W - 1, oy + 14, SCR_H - 28, C_RULE);

  drawHeader(g, ox, oy, a, COL_W);
  drawRing(g, cx, oy + RING_CY, RING_R, RING_IR, s, stale, RING_FONT, RING_LABEL);
  drawResetLine(g, cx, oy + RESET_Y, COL_W - PAD * 2, a, s);
  drawWeekBar(g, ox + PAD, ox + COL_W - PAD, oy + WEEK_Y, a);

  int first = s ? 1 : 0;
  int no = a.everOk ? a.nBuckets - first : 0;
  int shown = no <= MAX_ROWS ? no : MAX_ROWS - 1;
  for (int r = 0; r < shown; r++)
    drawRow(g, ox, oy + ROW_Y0 + r * ROW_H, a.buckets[first + r], stale);
  if (no > shown) {
    char more[20];
    snprintf(more, sizeof(more), "+%d more", no - shown);
    g.setTextFont(2);
    g.setTextColor(C_DIM);
    g.setTextDatum(TL_DATUM);
    g.drawString(more, ox + PAD, oy + ROW_Y0 + shown * ROW_H);
  }
}

// ---------------------------------------------------------------- single account

// One account uses the whole screen, drawn as two column-sized panes: a big
// session ring on the left, roomier model meters on the right.
#if BIG_SCREEN
#define WIDE_RING_CY 160
#define WIDE_RING_R  84
#define WIDE_RING_IR 64
#define WIDE_RESET_Y 268
#define WIDE_WEEK_Y  290
#define WIDE_ROW_Y0  36
#define WIDE_ROW_H   66
#define WIDE_ROWS    4
#define WIDE_BAR_H   13
#else
#define WIDE_RING_CY 124
#define WIDE_RING_R  60
#define WIDE_RING_IR 46
#define WIDE_RESET_Y 204
#define WIDE_WEEK_Y  220
#define WIDE_ROW_Y0  26
#define WIDE_ROW_H   50
#define WIDE_ROWS    4
#define WIDE_BAR_H   11
#endif

static void drawWideLeft(TFT_eSPI &g, int ox, int oy, const Account &a) {
  const bool stale = !a.ok && a.everOk;
  const Bucket *s = sessionOf(a);
  g.fillRect(ox, oy, COL_W, SCR_H, C_BG);
  drawHeader(g, ox, oy, a, COL_W);
  drawRing(g, ox + COL_W / 2, oy + WIDE_RING_CY, WIDE_RING_R, WIDE_RING_IR, s, stale, &FreeSansBold24pt7b, 26);
  drawResetLine(g, ox + COL_W / 2, oy + WIDE_RESET_Y, COL_W - PAD, a, s);
  drawWeekBar(g, ox + PAD, ox + COL_W - PAD, oy + WIDE_WEEK_Y, a);
}

static void drawWideRow(TFT_eSPI &g, int ox, int y, const Bucket &b, bool stale) {
  const int x0 = ox + 6, x1 = ox + COL_W - PAD;

  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)(b.util + 0.5f));
  g.setFreeFont(&FreeSansBold12pt7b);
  g.setTextColor(stale ? C_DIM : C_TEXT);
  g.setTextDatum(MR_DATUM);
  g.drawString(pct, x1, y + 8);
  int pw = g.textWidth(pct);

  g.setTextFont(2);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(stale ? C_DIM : C_SOFT);
  g.drawString(fit(g, b.label, x1 - x0 - pw - 8), x0, y + 8);

  drawBar(g, x0, y + 22, x1 - x0, WIDE_BAR_H, b.util, stale);

  char rs[16], note[32];
  fmtReset(b.resetsAt, false, rs, sizeof(rs));
  snprintf(note, sizeof(note), "%s%s%s%s", b.shared ? "shared" : "",
           b.shared && rs[0] ? "  -  " : "", rs[0] ? "resets " : "", rs);
  g.setTextFont(1);
  g.setTextColor(C_DIM);
  g.setTextDatum(TL_DATUM);
  g.drawString(fit(g, note, x1 - x0), x0, y + 38);
}

static void drawWideRight(TFT_eSPI &g, int ox, int oy, const Account &a) {
  const bool stale = !a.ok && a.everOk;
  g.fillRect(ox, oy, COL_W, SCR_H, C_BG);
  int first = sessionOf(a) ? 1 : 0;
  int no = a.everOk ? a.nBuckets - first : 0;
  for (int r = 0; r < no && r < WIDE_ROWS; r++)
    drawWideRow(g, ox, oy + WIDE_ROW_Y0 + r * WIDE_ROW_H, a.buckets[first + r], stale);
}

// ---------------------------------------------------------------- square page

// One account on a 240x240 screen. `oy` shifts everything vertically so the
// page can be rendered through a half-height sprite in two passes.
#define SQ_RING_CY  96
#define SQ_RING_R   54
#define SQ_RING_IR  41
#define SQ_RESET_Y  162
#define SQ_WEEK_Y   175
#define SQ_ROW_Y0   186
#define SQ_ROW_H    25
#define SQ_BAR_H    7

static void drawSquareRow(TFT_eSPI &g, int y, const Bucket &b, bool stale) {
  const int x0 = PAD + 4, x1 = SCR_W - PAD - 4;
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)(b.util + 0.5f));
  g.setFreeFont(&FreeSansBold9pt7b);
  g.setTextColor(stale ? C_DIM : C_TEXT);
  g.setTextDatum(MR_DATUM);
  g.drawString(pct, x1, y + 7);
  int pw = g.textWidth(pct);

  char rs[16];
  fmtReset(b.resetsAt, true, rs, sizeof(rs));
  int rsW = 0;
  g.setTextFont(1);
  g.setTextColor(C_DIM);
  if (rs[0]) { g.drawString(rs, x1 - pw - 8, y + 7); rsW = g.textWidth(rs) + 8; }

  g.setTextFont(2);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(stale ? C_DIM : C_SOFT);
  String name = fit(g, b.label, x1 - x0 - pw - rsW - 14);
  g.drawString(name, x0, y + 7);
  if (b.shared) {
    int nx = x0 + g.textWidth(name) + 5;
    g.setTextFont(1);
    if (nx + g.textWidth("shared") < x1 - pw - rsW - 8) { g.setTextColor(C_FAINT); g.drawString("shared", nx, y + 8); }
  }
  drawBar(g, x0, y + 16, x1 - x0, SQ_BAR_H, b.util, stale);
}

static void drawSquare(TFT_eSPI &g, int oy, const Account &a, int page, int pages) {
  const int cx = SCR_W / 2;
  const bool stale = !a.ok && a.everOk;
  const Bucket *s = sessionOf(a);
  g.fillRect(0, oy, SCR_W, SCR_H, C_BG);

  // Header: mark and name, plan and page on the right.
  drawMark(g, PAD, oy + 9, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
  g.setTextFont(2);
  int planW = g.textWidth(a.plan);
  g.setTextColor(C_DIM);
  g.setTextDatum(MR_DATUM);
  g.drawString(a.plan, SCR_W - PAD, oy + 18);
  if (pages > 1) {
    char pg[8];
    snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages);
    g.setTextFont(1);
    g.setTextColor(C_FAINT);
    g.drawString(pg, SCR_W - PAD, oy + 33);
  }
  const int nameW = SCR_W - PAD * 2 - 24 - planW - 8;
  g.setFreeFont(&FreeSansBold9pt7b);
  if (g.textWidth(a.label) > nameW) g.setTextFont(2);
  g.setTextColor(C_TEXT);
  g.setTextDatum(ML_DATUM);
  g.drawString(fit(g, a.label, nameW), PAD + 24, oy + 18);

  drawRing(g, cx, oy + SQ_RING_CY, SQ_RING_R, SQ_RING_IR, s, stale, &FreeSansBold24pt7b, 24);
  drawResetLine(g, cx, oy + SQ_RESET_Y, SCR_W - PAD * 2, a, s);
  drawWeekBar(g, PAD + 4, SCR_W - PAD - 4, oy + SQ_WEEK_Y, a);

  int first = s ? 1 : 0;
  int no = a.everOk ? min(a.nBuckets - first, 2) : 0;
  for (int r = 0; r < no; r++) drawSquareRow(g, oy + SQ_ROW_Y0 + r * SQ_ROW_H, a.buckets[first + r], stale);
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

  // Render the name flat in a scratch sprite, then copy it rotated. While the
  // setup page is served, its address follows the network name.
  char label[80];
  if (wifiLink.url[0]) snprintf(label, sizeof(label), "%s  %s", wifiLink.ssid, wifiLink.url + 7);
  else                 strlcpy(label, wifiLink.ssid[0] ? wifiLink.ssid : "no WiFi", sizeof(label));
  const char *name = label;
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

// ---------------------------------------------------------------- QR, setup, PIN

// Dark modules on a white quiet zone, as phone scanners expect.
static void drawQr(TFT_eSPI &g, int x, int y, int maxPx, const char *text) {
  static uint8_t buf[((8 * 4 + 17) * (8 * 4 + 17) + 7) / 8];   // version 8 modules
  // The library doesn't check capacity (too long a text overruns its stack
  // buffers), so pick the version here: bytes that fit at ECC_LOW, v1-v8.
  static const uint8_t CAP[8] = {17, 32, 53, 78, 106, 134, 154, 192};
  size_t len = strlen(text);
  int v = 1;
  while (v <= 8 && CAP[v - 1] < len) v++;
  if (v > 8) return;
  QRCode qr;
  qrcode_initText(&qr, buf, v, ECC_LOW, text);
  int scale = max(1, maxPx / (qr.size + 4));
  int side = (qr.size + 4) * scale;
  g.fillRect(x, y, side, side, TFT_WHITE);
  for (int r = 0; r < qr.size; r++)
    for (int c = 0; c < qr.size; c++)
      if (qrcode_getModule(&qr, c, r))
        g.fillRect(x + (c + 2) * scale, y + (r + 2) * scale, scale, scale, TFT_BLACK);
}

static void stepText(int x, int y, const char *num, const char *text, uint16_t col) {
  tft.setTextFont(BIG_SCREEN ? 4 : 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_MARK);
  tft.drawString(num, x, y);
  tft.setTextColor(col);
  tft.drawString(text, x + (BIG_SCREEN ? 22 : 14), y);
}

// Where drawJoin puts its text column, per screen. J_IN indents the detail
// lines under each numbered step.
#if SQUARE_SCREEN
#define J_QR_X 4
#define J_QR_Y 40
#define J_QR   128
#define J_X    138
#elif BIG_SCREEN
#define J_QR_X 12
#define J_QR_Y 46
#define J_QR   252
#define J_X    280
#else
#define J_QR_X 10
#define J_QR_Y 40
#define J_QR   150
#define J_X    172
#endif
#define J_IN   (BIG_SCREEN ? 22 : 14)
#define JY(v)  ((v) * SCR_H / 240)

// "Join the board's hotspot, then open the page": QR on the left, steps on
// the right. Used for first-boot setup, while no account exists, and from
// the menu.
static void drawJoin(const char *title, const char *ssid, const char *pass, const char *url,
                     const char *step3, const char *footer) {
  tft.fillScreen(C_BG);
  char wifiQr[96];
  snprintf(wifiQr, sizeof(wifiQr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
  drawQr(tft, J_QR_X, J_QR_Y, J_QR, wifiQr);
  const int x = J_X;
#if SQUARE_SCREEN
  // 240x240: title across the top, QR bottom-left, steps to its right.
  drawMark(tft, PAD, 9, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
  if (!strncmp(url, "http://", 7)) url += 7;         // the column is narrow
  if (!strncmp(ssid, "claude-status-", 14)) ssid += 14 - 3;   // "...b4c8"
#else
  drawMark(tft, x, JY(12), CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
#endif
  tft.setFreeFont(BIG_SCREEN ? &FreeSansBold12pt7b : &FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(title, SQUARE_SCREEN ? PAD + 24 : x + 24, SQUARE_SCREEN ? 18 : JY(21));

  stepText(x, JY(46), "1", "Scan to join", C_SOFT);
  tft.setTextFont(BIG_SCREEN ? 2 : 1);
  tft.setTextColor(C_DIM);
  tft.drawString(ssid, x + J_IN, JY(66));
  tft.drawString(String("pw ") + pass, x + J_IN, JY(78));
  stepText(x, JY(98), "2", "Open", C_SOFT);
  tft.setTextFont(2);                 // stepText left the big font set
  tft.setTextColor(C_TEXT);
  tft.drawString(fit(tft, url, SCR_W - x - J_IN - 4), x + J_IN, JY(118));
  stepText(x, JY(142), "3", step3, C_SOFT);

  tft.setTextFont(BIG_SCREEN ? 2 : 1);
  tft.setTextColor(C_FAINT);
  tft.setTextDatum(BL_DATUM);
  tft.drawString(fit(tft, footer, SCR_W - 16), SQUARE_SCREEN ? 6 : J_QR_X, SCR_H - 6);
}

static char lanUrl[40];          // for the "This network" screen

static void infoLine(int x, int y, const String &s, uint16_t col) {
  tft.setTextFont(BIG_SCREEN ? 2 : 1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col);
  tft.drawString(fit(tft, s, SCR_W - x - 4), x, y);
}

static void linkTitle(const char *title) {
  tft.fillScreen(C_BG);
#if SQUARE_SCREEN
  drawMark(tft, PAD, 9, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
#else
  drawMark(tft, J_X, JY(12), CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
#endif
  tft.setFreeFont(BIG_SCREEN ? &FreeSansBold12pt7b : &FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(title, SQUARE_SCREEN ? PAD + 24 : J_X + 24, SQUARE_SCREEN ? 18 : JY(21));
}

static void linkFooter(const String &s) {
  tft.setTextFont(BIG_SCREEN ? 2 : 1);
  tft.setTextColor(C_FAINT);
  tft.setTextDatum(BL_DATUM);
  tft.drawString(fit(tft, s, SCR_W - 16), SQUARE_SCREEN ? 6 : J_QR_X, SCR_H - 6);
}

// Text centred in the QR's square, while there's no QR to show.
static void qrPlaceholder(const char *l1, const char *l2, uint16_t col) {
  const int cx = J_QR_X + J_QR / 2, cy = J_QR_Y + J_QR / 2;
  tft.drawRoundRect(J_QR_X, J_QR_Y, J_QR, J_QR, 12, C_FAINT);
  tft.setTextFont(BIG_SCREEN ? 2 : 1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col);
  tft.drawString(fit(tft, l1, J_QR - 12), cx, cy - 8);
  tft.setTextColor(C_DIM);
  tft.drawString(fit(tft, l2, J_QR - 12), cx, cy + 8);
}

static bool looksLikeGuest(const char *ssid) {
  String s = ssid;
  s.toLowerCase();
  return s.indexOf("guest") >= 0 || s.indexOf("visitor") >= 0 || s.indexOf("public") >= 0;
}

// The relay's QR: works from any network, since both ends only reach out
// to the relay server.
static void drawRemote(const char *title, const String &footer) {
  linkTitle(title);
  RelayState st = relayState();
  if (st == RELAY_UP) drawQr(tft, J_QR_X, J_QR_Y, J_QR, relayLink().c_str());
  else if (st == RELAY_FAILED) qrPlaceholder("Relay unreachable", "retrying; or use the hotspot", C_HOT);
  else qrPlaceholder("Connecting to the relay", relayBrokerName()[0] ? relayBrokerName() : "...", C_SOFT);

  const int x = J_X;
  stepText(x, JY(46), "1", SQUARE_SCREEN ? "Scan it" : "Scan with phone", C_SOFT);
  infoLine(x + J_IN, JY(66), "Any network works:", C_DIM);
  infoLine(x + J_IN, JY(78), "guest WiFi, mobile data", C_DIM);
  stepText(x, JY(98), "2", SQUARE_SCREEN ? "Set up" : "Set up on the page", C_SOFT);
  infoLine(x + J_IN, JY(118), "Encrypted end to end", C_DIM);
  infoLine(x + J_IN, JY(130), String("via ") + (relayBrokerName()[0] ? relayBrokerName() : "relay"), C_DIM);
  linkFooter(footer);
}

// The page at the board's address on this network. Guest networks keep
// devices apart, so say so, louder when the name gives it away.
static void drawLan() {
  linkTitle("This network");
  drawQr(tft, J_QR_X, J_QR_Y, J_QR, lanUrl);
  const int x = J_X;
  stepText(x, JY(46), "1", SQUARE_SCREEN ? "Scan it" : "Scan with phone", C_SOFT);
  infoLine(x + J_IN, JY(66), String("phone on ") + wifiLink.ssid, C_DIM);
  infoLine(x + J_IN, JY(78), lanUrl + 7, C_TEXT);
  stepText(x, JY(98), "2", SQUARE_SCREEN ? "PIN" : "Enter the PIN", C_SOFT);
  infoLine(x + J_IN, JY(118), "shown here on request", C_DIM);
  bool guest = looksLikeGuest(wifiLink.ssid);
  infoLine(x, JY(146), guest ? "Looks like guest WiFi:" : "Guest WiFi blocks this.", C_WARN);
  infoLine(x, JY(158), guest ? "phones can't reach the" : "If the page won't load,", C_WARN);
  infoLine(x, JY(170), guest ? "board. Use Remote link." : "use Remote link instead.", C_WARN);
  linkFooter("Tap to close.");
}

int uiChoose(const char *title, const char *const *labels, const char *const *subs, int n) {
  const int y0 = BIG_SCREEN ? 48 : 34, rowH = BIG_SCREEN ? 60 : 44, gap = BIG_SCREEN ? 8 : 4;
  const int x = 12, w = SCR_W - 24;
  int sel = 0;
  auto paint = [&]() {
    tft.fillScreen(C_BG);
    tft.setFreeFont(BIG_SCREEN ? &FreeSansBold12pt7b : &FreeSansBold9pt7b);
    tft.setTextColor(C_TEXT);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(title, x + 2, y0 / 2);
    for (int i = 0; i < n; i++) {
      int y = y0 + i * (rowH + gap);
      tft.fillSmoothRoundRect(x, y, w, rowH, 10, rgb(0x1C1C23), C_BG);
      if (!HAS_TOUCHSCREEN && i == sel) tft.drawRoundRect(x, y, w, rowH, 10, C_MARK);
      bool hasSub = subs && subs[i] && subs[i][0];
      if (BIG_SCREEN) tft.setFreeFont(&FreeSansBold9pt7b); else tft.setTextFont(2);
      tft.setTextColor(C_TEXT);
      tft.setTextDatum(ML_DATUM);
      tft.drawString(fit(tft, labels[i], w - 28), x + 14, y + (hasSub ? rowH / 3 : rowH / 2));
      if (!hasSub) continue;
      const char *sub = subs[i];
      bool warn = sub[0] == '!';                    // a leading '!' marks a warning
      tft.setTextFont(BIG_SCREEN ? 2 : 1);
      tft.setTextColor(warn ? C_WARN : C_DIM);
      tft.drawString(fit(tft, sub + warn, w - 28), x + 14, y + rowH * 3 / 4);
    }
    if (!HAS_TOUCHSCREEN) {
      tft.setTextFont(1);
      tft.setTextColor(C_FAINT);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("tap: next    hold: select", SCR_W / 2, SCR_H - 8);
    }
  };
  paint();
  for (;;) {
    InputEvent e = inputWait();
    if (!HAS_TOUCHSCREEN) {
      if (e.kind == IN_HOLD) return sel;
      sel = (sel + 1) % n;
      paint();
      continue;
    }
    if (e.kind != IN_TAP) continue;
    for (int i = 0; i < n; i++) {
      int y = y0 + i * (rowH + gap);
      if (e.y >= y - gap / 2 && e.y < y + rowH + gap / 2) return i;
    }
  }
}

#define SETUP_BTN_X J_X
#define SETUP_BTN_Y JY(170)
#define SETUP_BTN_W (SCR_W - J_X - 10)
#define SETUP_BTN_H (BIG_SCREEN ? 50 : 36)

void uiSetupScreen(const char *apSsid, const char *apPass, const char *url) {
  drawJoin("WiFi setup", apSsid, apPass, url, "Pick WiFi",
           SQUARE_SCREEN ? "Power-cycle to cancel" : "Accounts and logins are kept. Power-cycle to cancel.");
  if (!HAS_TOUCHSCREEN) return;
  // Or skip the phone: type the WiFi password on this screen.
  tft.fillSmoothRoundRect(SETUP_BTN_X, SETUP_BTN_Y, SETUP_BTN_W, SETUP_BTN_H, 9, C_MARK, C_BG);
  tft.setTextFont(BIG_SCREEN ? 4 : 2);
  tft.setTextColor(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Use this screen", SETUP_BTN_X + SETUP_BTN_W / 2, SETUP_BTN_Y + SETUP_BTN_H / 2);
}

bool uiSetupButtonHit(int x, int y) {
  return x >= SETUP_BTN_X - 6 && x < SETUP_BTN_X + SETUP_BTN_W + 6 &&
         y >= SETUP_BTN_Y - 6 && y < SETUP_BTN_Y + SETUP_BTN_H + 6;
}

// ---------------------------------------------------------------- overlays

static OverlayKind overlay = OV_NONE;
static int menuSel = 0;          // highlighted menu entry, one-button boards
static char pinShown[7] = "";
static char hsSsid[33], hsPass[16], hsUrl[40];

int  uiOverlay() { return overlay; }

void uiShowPin(const char *pin) {
  strlcpy(pinShown, pin, sizeof(pinShown));
  overlay = OV_PIN;
  uiDrawAll();
}
void uiHidePin()      { if (overlay == OV_PIN) uiCloseOverlay(); }
void uiHideHotspot()  { if (overlay == OV_HOTSPOT) uiCloseOverlay(); }
void uiShowMenu()     { overlay = OV_MENU; menuSel = 0; uiDrawAll(); }
void uiCloseOverlay() { overlay = OV_NONE; uiDrawAll(); }

void uiShowRemote()             { overlay = OV_REMOTE; uiDrawAll(); }
void uiShowLan(const char *url) { strlcpy(lanUrl, url, sizeof(lanUrl)); overlay = OV_LAN; uiDrawAll(); }

void uiShowHotspot(const char *ssid, const char *pass, const char *url) {
  strlcpy(hsSsid, ssid, sizeof(hsSsid));
  strlcpy(hsPass, pass, sizeof(hsPass));
  strlcpy(hsUrl, url, sizeof(hsUrl));
  overlay = OV_HOTSPOT;
  uiDrawAll();
}

static void drawPinOverlay() {
  const int area = SQUARE_SCREEN ? SCR_W : 2 * COL_W;
  const int w = min(240, SCR_W - 16), h = 120, x = (area - w) / 2, y = (SCR_H - h) / 2;
  tft.fillSmoothRoundRect(x, y, w, h, 12, rgb(0x16161B), C_BG);
  tft.drawRoundRect(x, y, w, h, 12, C_MARK);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("PIN for the setup page", x + w / 2, y + 22);
  char spaced[12];
  snprintf(spaced, sizeof(spaced), "%.3s %.3s", pinShown, pinShown + 3);
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_TEXT);
  tft.drawString(spaced, x + w / 2, y + 64);
  tft.setTextFont(1);
  tft.setTextColor(C_DIM);
  tft.drawString("valid for 2 minutes", x + w / 2, y + h - 14);
}

// Menu entries per board. On a touchscreen the last row holds two half-width
// buttons, Restart and Close; one-button boards list them as rows and move a
// highlight with each tap (hold picks it).
struct MenuEntry { MenuAction act; const char *label; const char *sub; };
static const MenuEntry MENU[] = {
  {MA_PHONE,       "Phone setup",         ""},
#if HAS_TOUCHSCREEN
  {MA_WIFI_SCREEN, "WiFi on this screen", ""},
#endif
  {MA_WIFI_PHONE,  "WiFi with a phone",   ""},
  {MA_UPDATE,      "Update firmware",     ""},
#if TOUCH_VIA_TFT
  {MA_CALIBRATE,   "Calibrate touch",     ""},
#endif
  {MA_WIPE,        "Wipe all settings",   ""},
  {MA_RESTART,     "Restart",             ""},
  {MA_CLOSE,       "Close",               ""},
};
#define MENU_ENTRIES ((int)(sizeof(MENU) / sizeof(MENU[0])))
#if HAS_TOUCHSCREEN
// Two buttons a row; Restart and Close share the last one.
#define MENU_ITEMS (MENU_ENTRIES - 2)
#define MENU_ROWS ((MENU_ITEMS + 1) / 2 + 1)
#define MENU_X    12
#if BIG_SCREEN
#define MENU_Y0   44
#define MENU_H    46
#define MENU_GAP  8
#else
#define MENU_Y0   34
#define MENU_H    34
#define MENU_GAP  6
#endif
#else
#define MENU_ROWS MENU_ENTRIES
#define MENU_X    12
#define MENU_Y0   30
#define MENU_H    26
#define MENU_GAP  3
#endif
#define MENU_W   (SCR_W - 2 * MENU_X)

MenuAction uiMenuHit(int x, int y) {
  if (overlay != OV_MENU || x < MENU_X || x > MENU_X + MENU_W) return MA_NONE;
  for (int i = 0; i < MENU_ROWS; i++) {
    int top = MENU_Y0 + i * (MENU_H + MENU_GAP);
    if (y < top - MENU_GAP / 2 || y >= top + MENU_H + MENU_GAP / 2) continue;
#if HAS_TOUCHSCREEN
    bool right = x >= MENU_X + MENU_W / 2;
    if (i == MENU_ROWS - 1) return right ? MA_CLOSE : MA_RESTART;
    int k = i * 2 + right;
    return k < MENU_ITEMS ? MENU[k].act : MA_NONE;
#else
    return MENU[i].act;
#endif
  }
  return MA_NONE;
}

void uiMenuNext() { menuSel = (menuSel + 1) % MENU_ENTRIES; uiDrawAll(); }
MenuAction uiMenuSelected() { return MENU[menuSel].act; }

static void drawMenu() {
  tft.fillScreen(C_BG);
  drawMark(tft, MENU_X, 10, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Menu", MENU_X + 26, 19);
  char where[64];
#if SQUARE_SCREEN
  snprintf(where, sizeof(where), "%s %s", fwVersion(), BOARD_ID);
#else
  snprintf(where, sizeof(where), "%s %s  %s  %s", fwVersion(), BOARD_ID, wifiLink.ssid,
           wifiLink.up ? WiFi.localIP().toString().c_str() : "offline");
#endif
  tft.setTextFont(1);
  tft.setTextColor(C_DIM);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(where, SCR_W - MENU_X, 19);

#if HAS_TOUCHSCREEN
  const int half = (MENU_W - 6) / 2;
  for (int k = 0; k < MENU_ENTRIES; k++) {
    int row = k < MENU_ITEMS ? k / 2 : MENU_ROWS - 1;
    int col = k < MENU_ITEMS ? k % 2 : k - MENU_ITEMS;
    int bx = MENU_X + col * (half + 6), y = MENU_Y0 + row * (MENU_H + MENU_GAP);
    bool quiet = k >= MENU_ITEMS;                  // Restart, Close
    tft.fillSmoothRoundRect(bx, y, half, MENU_H, 10, rgb(quiet ? 0x101014 : 0x1C1C23), C_BG);
    if (k == 0) tft.drawRoundRect(bx, y, half, MENU_H, 10, C_MARK);
    if (BIG_SCREEN) tft.setFreeFont(&FreeSans9pt7b); else tft.setTextFont(2);
    tft.setTextColor(quiet ? C_SOFT : C_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(MENU[k].label, bx + half / 2, y + MENU_H / 2);
  }
  return;
#endif
  for (int i = 0; i < MENU_ROWS; i++) {
    int y = MENU_Y0 + i * (MENU_H + MENU_GAP);
    bool lit = HAS_TOUCHSCREEN ? i == 0 : i == menuSel;
    tft.fillSmoothRoundRect(MENU_X, y, MENU_W, MENU_H, 10, rgb(0x16161B), C_BG);
    if (lit) tft.drawRoundRect(MENU_X, y, MENU_W, MENU_H, 10, C_MARK);
    bool sub = !SQUARE_SCREEN && MENU[i].sub[0];
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(MENU[i].label, MENU_X + 14, y + (sub ? 11 : MENU_H / 2));
    if (sub) {
      tft.setTextFont(1);
      tft.setTextColor(C_DIM);
      tft.drawString(MENU[i].sub, MENU_X + 14, y + 25);
    }
  }
  if (!HAS_TOUCHSCREEN) {
    tft.setTextFont(1);
    tft.setTextColor(C_FAINT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("tap: next    hold: select", SCR_W / 2, SCR_H - 8);
  }
}

// No accounts yet: the hotspot is up, so show how to join it.
static void drawEmptyScreen() {
  if (relayState() == RELAY_UP) {
    String footer;
    if (webHotspotUp()) footer = String("Or join ") + webApSsid() + "  pw " + webApPass() + "  -> " + (webHotspotUrl().c_str() + 7);
    drawRemote("Add account", footer);
    return;
  }
  if (webHotspotUp()) {
    char footer[64] = "";
    if (cfg.hosting && wifiLink.up)
      snprintf(footer, sizeof(footer), "On a home network also: http://%s", WiFi.localIP().toString().c_str());
    drawJoin("Add account", webApSsid(), webApPass(), webHotspotUrl().c_str(),
             "Add account", footer);
    return;
  }
  tft.fillScreen(C_BG);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("No accounts yet. Joining WiFi...", SCR_W / 2, SCR_H / 2);
}

// With no buttons or touch, the way in is the hotspot, so say so on screen
// while it's up.
static void drawSetupFooter() {
  const int h = 14, y = SCR_H - h;
  char line[96];
  snprintf(line, sizeof(line), "setup: join %s  pw %s  ->  %s", webApSsid(), webApPass(),
           webHotspotUrl().c_str() + 7);
  tft.fillRect(0, y, SCR_W, h, rgb(0x121218));
  tft.setTextFont(1);
  tft.setTextColor(C_SOFT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(fit(tft, line, SCR_W - 12), 6, y + h / 2);
}

#if TOUCH_VIA_TFT
// Touch the arrow in each corner. Returns false if nobody touches the screen
// within the first 15 seconds, so a board with broken touch still boots.
bool uiTouchCalibrate(uint16_t out[5], uint32_t waitMs) {
  tft.fillScreen(C_BG);
  drawMark(tft, (SCR_W - CLAUDE_MARK_18) / 2, 40, CLAUDE_MARK_18, CLAUDE_MARK_18_ALPHA);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(C_TEXT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Calibrate the touchscreen", SCR_W / 2, 90);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM);
  tft.drawString("Touch the arrow in each corner.", SCR_W / 2, 120);

  if (waitMs) {
    tft.drawString("Touch the screen to start.", SCR_W / 2, 145);
    uint32_t start = millis();
    while (!inputPressed()) {
      if (millis() - start > waitMs) return false;    // nobody there; carry on
      char t[32];
      snprintf(t, sizeof(t), "skipping in %2d s ", (int)((waitMs - (millis() - start)) / 1000));
      tft.setTextColor(C_FAINT, C_BG);
      tft.drawString(t, SCR_W / 2, 175);
      delay(100);
    }
    while (inputPressed()) delay(10);
  }

  tft.fillScreen(C_BG);
  tft.calibrateTouch(out, C_TEXT, C_BG, 20);
  tft.setTouch(out);
  tft.fillScreen(C_BG);
  tft.setTextFont(2);
  tft.setTextColor(C_SOFT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Calibrated", SCR_W / 2, SCR_H / 2);
  delay(700);
  return true;
}
#endif

// Full-screen yes/no, used for anything destructive.
bool uiConfirm(const char *title, const char *line, const char *yes, const char *no) {
  const int by = SCR_H - 60, bh = 44, half = (SCR_W - 36) / 2;
  const char *labels[2] = {yes, no};
  int sel = 0;
  auto paint = [&]() {
    tft.fillScreen(C_BG);
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(C_TEXT);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(title, 14, 24);
    tft.setTextFont(2);
    tft.setTextColor(C_SOFT);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(fit(tft, line, SCR_W - 28), 14, 60);
    for (int i = 0; i < 2; i++) {
      int x = 12 + i * (half + 12);
      bool lit = HAS_TOUCHSCREEN ? i == 0 : i == sel;
      tft.fillSmoothRoundRect(x, by, half, bh, 9, lit ? C_HOT : rgb(0x22222A), C_BG);
      tft.setTextFont(2);
      tft.setTextColor(lit ? C_BG : C_TEXT);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(labels[i], x + half / 2, by + bh / 2);
    }
    if (!HAS_TOUCHSCREEN) {
      tft.setTextFont(1);
      tft.setTextColor(C_FAINT);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("tap: next    hold: select", SCR_W / 2, SCR_H - 8);
    }
  };
  paint();
  for (;;) {
    InputEvent e = inputWait();
    if (!HAS_TOUCHSCREEN) {
      if (e.kind == IN_HOLD) return sel == 0;
      sel ^= 1;
      paint();
      continue;
    }
    if (e.kind != IN_TAP || e.y < by - 10 || e.y > by + bh + 10) continue;
    return e.x < SCR_W / 2;
  }
}

// ---------------------------------------------------------------- public

// The only status is a small dot, top right of the accounts, while a check runs.
void uiDrawStatus() {
  if (overlay == OV_MENU || overlay == OV_HOTSPOT || overlay == OV_REMOTE || overlay == OV_LAN) return;
  uint16_t c = netStatus == NET_CHECKING ? C_INFO : C_BG;
  tft.fillSmoothCircle(SQUARE_SCREEN ? SCR_W - 5 : 2 * COL_W - 8, SQUARE_SCREEN ? 4 : 7, 3, c, C_BG);
}

// Accounts per page: one on a square screen, two side by side otherwise.
// firstVisible is the first account on the current page.
static const int PER_PAGE = SQUARE_SCREEN ? 1 : 2;
static int firstVisible = 0;

bool uiScroll(int delta) {
  if (accountCount <= PER_PAGE) return false;
  int next = constrain(firstVisible + delta, 0, accountCount - PER_PAGE);
  if (next == firstVisible) return false;
  firstVisible = next;
  return true;
}

// Next page, wrapping from the last back to the first.
bool uiNextPage() {
  if (accountCount <= PER_PAGE) return false;
  firstVisible = firstVisible + PER_PAGE < accountCount ? firstVisible + 1 : 0;
  return true;
}

// Paint one column-sized pane via the sprite, or straight to the panel.
static void pushPane(int x, void (*draw)(TFT_eSPI &, int, int, const Account &), const Account &a) {
  if (!haveColSpr) { draw(tft, x, 0, a); return; }
#if BIG_SCREEN
  for (int half = 0; half < 2; half++) {                // the buffer is half height
    draw(colSpr, 0, -half * (SCR_H / 2), a);
    colSpr.pushSprite(x, half * (SCR_H / 2));
  }
#else
  draw(colSpr, 0, 0, a);
  colSpr.pushSprite(x, 0);
#endif
}
static void columnWithRule(TFT_eSPI &g, int ox, int oy, const Account &a) { drawColumn(g, ox, oy, a, true); }
static void columnPlain(TFT_eSPI &g, int ox, int oy, const Account &a)    { drawColumn(g, ox, oy, a, false); }

// Dots for every account (the visible pair lit) and arrows at the edges
// that have more beyond them.
static void drawScrollHints() {
  const int y = SCR_H - 5, gap = 10;
  int x = COL_W - (accountCount - 1) * gap / 2;
  for (int i = 0; i < accountCount; i++, x += gap) {
    bool lit = i == firstVisible || i == firstVisible + 1;
    tft.fillSmoothCircle(x, y, lit ? 2 : 1, lit ? C_SOFT : C_FAINT, C_BG);
  }
  const int my = SCR_H / 2;
  if (firstVisible > 0)
    tft.fillTriangle(1, my, 5, my - 5, 5, my + 5, C_DIM);
  if (firstVisible + 2 < accountCount)
    tft.fillTriangle(2 * COL_W - 2, my, 2 * COL_W - 6, my - 5, 2 * COL_W - 6, my + 5, C_DIM);
}

void uiDrawAll() {
  if (overlay == OV_MENU) { drawMenu(); return; }
  if (overlay == OV_REMOTE) {
    drawRemote("Remote setup", SQUARE_SCREEN ? "Tap to close. One-time link."
                               : "One-time link, new each time. Ends after 15 idle min. Tap to close.");
    return;
  }
  if (overlay == OV_LAN) { drawLan(); return; }
  if (overlay == OV_HOTSPOT) {
    drawJoin("Phone setup", hsSsid, hsPass, hsUrl, SQUARE_SCREEN ? "Settings" : "Accounts & settings",
             SQUARE_SCREEN ? "Tap to close. Off after 15 idle min"
                           : "Tap to close. The hotspot turns off after 15 idle minutes.");
    return;
  }
#if SQUARE_SCREEN
  if (accountCount == 0) {
    drawEmptyScreen();
  } else {
    firstVisible = constrain(firstVisible, 0, accountCount - 1);
    const Account &a = accounts[firstVisible];
    if (haveColSpr) {
      for (int half = 0; half < 2; half++) {
        drawSquare(colSpr, -half * (SCR_H / 2), a, firstVisible, accountCount);
        colSpr.pushSprite(0, half * (SCR_H / 2));
      }
    } else {
      drawSquare(tft, 0, a, firstVisible, accountCount);
    }
  }
  if (overlay == OV_PIN) drawPinOverlay();
  uiDrawStatus();
  return;
#endif
  if (accountCount == 0) {
    drawEmptyScreen();
  } else if (accountCount == 1) {
    pushPane(0, drawWideLeft, accounts[0]);
    pushPane(COL_W, drawWideRight, accounts[0]);
  } else {
    firstVisible = constrain(firstVisible, 0, max(0, accountCount - 2));
    pushPane(0, columnWithRule, accounts[firstVisible]);
    pushPane(COL_W, columnPlain, accounts[firstVisible + 1]);
    if (accountCount > 2) drawScrollHints();
  }
  if (haveLinkSpr) { drawLinkStrip(linkSpr, 0); linkSpr.pushSprite(2 * COL_W, 0); }
  else             drawLinkStrip(tft, 2 * COL_W);
  if (!HAS_INPUT && webHotspotUp() && accountCount) drawSetupFooter();
  if (overlay == OV_PIN) drawPinOverlay();
  uiDrawStatus();
}

void uiSplash(const char *status) {
  tft.fillScreen(C_BG);
  drawMark(tft, (SCR_W - CLAUDE_MARK_60) / 2, 56, CLAUDE_MARK_60, CLAUDE_MARK_60_ALPHA);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(status, SCR_W / 2, 150);
  // Which firmware is starting, so an update is easy to confirm.
  char ver[40];
  snprintf(ver, sizeof(ver), "%s  %s", fwVersion(), BOARD_ID);
  tft.setTextFont(1);
  tft.setTextColor(C_FAINT);
  tft.drawString(ver, SCR_W / 2, SCR_H - 12);
}
