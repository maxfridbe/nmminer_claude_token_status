#include "input.h"

#define LONG_PRESS_MS 1500   // hold this long for the menu
#define RELEASE_READS 3      // idle samples before a press counts as over

#if HAS_TOUCHSCREEN
// ---------------------------------------------------------------- CYD touchscreen

#include <SPI.h>
#include <XPT2046_Touchscreen.h>

// Touch controller: its own pins, not the display's HSPI bus.
#define XPT2046_CLK  25
#define XPT2046_MOSI 32
#define XPT2046_CS   33
#define XPT2046_MISO 39
#define XPT2046_IRQ  36
#define TS_MINX      200     // raw extents, landscape
#define TS_MAXX      3700
#define TS_MINY      240
#define TS_MAXY      3800
#define Z_MIN        280     // real presses on this panel read 400+
#define SWIPE_PX     40      // horizontal travel that counts as a swipe

static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

void inputBegin() {
  pinMode(XPT2046_IRQ, INPUT);
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);
}

// The IRQ line only drops under real pressure, so it gates every read.
bool touchRead(int &x, int &y) {
  if (digitalRead(XPT2046_IRQ) == HIGH || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < Z_MIN) return false;
  x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCR_W), 0, SCR_W - 1);
  y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCR_H), 0, SCR_H - 1);
  return true;
}

bool inputPressed() { return digitalRead(XPT2046_IRQ) == LOW; }

static bool down = false, held = false;
static int  sx, sy, lx, ly, idle;
static uint32_t since;

InputEvent inputPoll() {
  int x, y;
  if (touchRead(x, y)) {
    if (!down) { down = true; held = false; sx = x; sy = y; since = millis(); }
    lx = x; ly = y; idle = 0;
    if (!held && millis() - since >= LONG_PRESS_MS && abs(lx - sx) < 30 && abs(ly - sy) < 30) {
      held = true;
      return {IN_HOLD, sx, sy};
    }
    return {IN_NONE, 0, 0};
  }
  if (!down || ++idle < RELEASE_READS) return {IN_NONE, 0, 0};
  down = false;
  if (held) return {IN_NONE, 0, 0};
  int dx = lx - sx;
  if (dx <= -SWIPE_PX) return {IN_SWIPE_LEFT, sx, sy};
  if (dx >= SWIPE_PX)  return {IN_SWIPE_RIGHT, sx, sy};
  return {IN_TAP, sx, sy};
}

#else
// ---------------------------------------------------------------- one touch button

// Either an ESP32 touch pad (reads drop when touched) or a touch IC with a
// digital output; which one, and on which pin, comes from env:nmtv-probe.
static uint16_t padBase = 0;

void inputBegin() {
  if (BUTTON_PIN < 0) return;
#if BUTTON_TOUCHPAD
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) { sum += touchRead(BUTTON_PIN); delay(5); }
  padBase = sum / 16;
#else
  pinMode(BUTTON_PIN, INPUT);
#endif
}

bool inputPressed() {
  if (BUTTON_PIN < 0) return false;
#if BUTTON_TOUCHPAD
  return touchRead(BUTTON_PIN) < padBase * 3 / 4;
#else
  return digitalRead(BUTTON_PIN) == BUTTON_ACTIVE;
#endif
}

bool touchRead(int &, int &) { return false; }

static bool down = false, held = false;
static int  idle;
static uint32_t since;

InputEvent inputPoll() {
  if (inputPressed()) {
    if (!down) { down = true; held = false; since = millis(); }
    idle = 0;
    if (!held && millis() - since >= LONG_PRESS_MS) { held = true; return {IN_HOLD, 0, 0}; }
    return {IN_NONE, 0, 0};
  }
  if (!down || ++idle < RELEASE_READS) return {IN_NONE, 0, 0};
  down = false;
  return {held ? IN_NONE : IN_TAP, 0, 0};
}
#endif

InputEvent inputWait(uint32_t timeoutMs) {
  uint32_t start = millis();
  for (;;) {
    InputEvent e = inputPoll();
    if (e.kind == IN_TAP || e.kind == IN_HOLD) return e;
    if (timeoutMs && millis() - start > timeoutMs) return {IN_NONE, 0, 0};
    delay(10);
  }
}
