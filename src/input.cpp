#include "input.h"
#include "settings.h"

#define LONG_PRESS_MS 1500   // hold this long for the menu
#define RELEASE_READS 3      // idle samples before a press counts as over
#define DOUBLE_GAP_MS 350    // a second tap within this window is a double tap

static void (*holdIndicator)(float) = nullptr;
void inputSetHoldIndicator(void (*fn)(float)) { holdIndicator = fn; }

static void showHold(float f) { if (holdIndicator) holdIndicator(f); }

#if HAS_TOUCHSCREEN
#if TOUCH_VIA_TFT
// ------------------------------------------- touch on the display's own SPI bus
// TFT_eSPI owns the bus and reads the touch controller itself (TOUCH_CS).

#include <TFT_eSPI.h>
extern TFT_eSPI tft;

#define SWIPE_PX 40

void inputBegin() {}

bool touchRead(int &x, int &y) {
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return false;
  x = constrain((int)tx, 0, SCR_W - 1);
  y = constrain((int)ty, 0, SCR_H - 1);
  return true;
}

bool inputPressed() {
  int x, y;
  return touchRead(x, y);
}

#else
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
// Rotating the screen does not rotate the panel, so standing the board on end
// swaps the axes here: the raw reading still runs along the panel's own sides.
bool touchRead(int &x, int &y) {
  if (digitalRead(XPT2046_IRQ) == HIGH || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < Z_MIN) return false;
  if (CAN_ROTATE && cfg.portrait) {
    // Rotation 1 reads the panel's long side as x. Turning the screen to
    // rotation 0 puts the panel's short side across the top, running the
    // other way, so x comes from an inverted p.y and y straight from p.x.
    x = constrain(map(p.y, TS_MAXY, TS_MINY, 0, SCR_W), 0, SCR_W - 1);
    y = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCR_H), 0, SCR_H - 1);
  } else {
    x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, SCR_W), 0, SCR_W - 1);
    y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCR_H), 0, SCR_H - 1);
  }
#ifdef TOUCH_TRACE
  Serial.printf("[touch] raw %4d,%4d z=%3d -> %3d,%3d  (flipped %3d,%3d)\n", p.x, p.y, p.z, x, y,
                constrain(map(p.y, TS_MINY, TS_MAXY, 0, SCR_W), 0, SCR_W - 1),
                constrain(map(p.x, TS_MAXX, TS_MINX, 0, SCR_H), 0, SCR_H - 1));
#endif
  return true;
}

bool inputPressed() { return digitalRead(XPT2046_IRQ) == LOW; }

#endif  // TOUCH_VIA_TFT

// ---- shared by both touchscreen backends: taps, swipes and holds
static bool down = false, held = false;
static int  sx, sy, lx, ly, idle;
static uint32_t since;

InputEvent inputPoll() {
  int x, y;
  if (touchRead(x, y)) {
    if (!down) { down = true; held = false; sx = x; sy = y; since = millis(); }
    lx = x; ly = y; idle = 0;
    bool still = abs(lx - sx) < 30 && abs(ly - sy) < 30;
    if (!held && millis() - since >= LONG_PRESS_MS && still) {
      held = true;
      showHold(0);
      return {IN_HOLD, sx, sy};
    }
    if (!held) showHold(still ? (millis() - since) / (float)LONG_PRESS_MS : 0);
    return {IN_NONE, 0, 0};
  }
  if (!down || ++idle < RELEASE_READS) return {IN_NONE, 0, 0};
  down = false;
  showHold(0);
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
static uint16_t padBase = 0;     // the resting reading
static bool     everPressed = false;

#if BUTTON_TOUCHPAD
// Touch reads are noisy: the middle of three throws out the stray one.
static uint16_t padRead() {
  uint16_t a = touchRead(BUTTON_PIN), b = touchRead(BUTTON_PIN), c = touchRead(BUTTON_PIN);
  return max(min(a, b), min(max(a, b), c));
}
// A finger drops this pad about 20% (101 resting, 78 pressed). Trigger at
// half that, and let go later than it grabs, so a hold can't stutter.
static uint16_t padThreshold()   { return padBase - padBase / 8; }
static uint16_t padRelease()     { return padBase - padBase / 16; }
#endif

void inputBegin() {
  if (BUTTON_PIN < 0) return;
#if BUTTON_TOUCHPAD
  padRead();                      // the first read after boot means nothing
  delay(20);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) { sum += padRead(); delay(5); }
  padBase = sum / 16;
  Serial.printf("[input] touch pad GPIO%d resting at %u, press reads under %u\n",
                BUTTON_PIN, padBase, padThreshold());
#else
  pinMode(BUTTON_PIN, BUTTON_ACTIVE == LOW ? INPUT_PULLUP : INPUT);
#endif
}

// The resting level drifts with temperature, and WiFi starting after
// inputBegin() moves it too, so it follows the idle reading rather than
// staying where boot left it. Only readings at rest feed that drift: a
// near-miss press must not drag the threshold down past itself, which is a
// button that stops working the harder it is pressed.
bool inputPressed() {
  if (BUTTON_PIN < 0) return false;
#if BUTTON_TOUCHPAD
  uint16_t v = padRead();
  if (!v) return false;                          // peripheral not answering
  static bool pressed = false;
  if (padBase > 16) pressed = v < (pressed ? padRelease() : padThreshold());
  // Nobody holds a button for ten seconds. If it looks that way the resting
  // level moved under us, so take the reading as the new rest.
  static uint32_t pressedSince = 0;
  if (!pressed) pressedSince = 0;
  else if (!pressedSince) pressedSince = millis();
  else if (millis() - pressedSince > 10000) {
    Serial.printf("[pad] stuck low at %u, re-resting (was %u)\n", v, padBase);
    padBase = v;
    pressed = false;
    pressedSince = 0;
  }
  static uint32_t driftAt = 0;
  if (v >= padRelease() && millis() - driftAt > 250) {
    driftAt = millis();
    padBase = padBase ? (uint16_t)((padBase * 7 + v) / 8) : v;
  }
  // Until the first press lands, say what the pad reads: a button that never
  // registers looks exactly like one nobody touched.
  static uint32_t traceAt = 0;
  if (!everPressed && millis() - traceAt > 1000) {
    traceAt = millis();
    Serial.printf("[pad] %u (resting %u, press under %u)\n", v, padBase, padThreshold());
  }
  if (pressed && !everPressed) {
    everPressed = true;
    Serial.printf("[pad] first press: %u (resting %u)\n", v, padBase);
  }
  return pressed;
#else
  return digitalRead(BUTTON_PIN) == BUTTON_ACTIVE;
#endif
}

bool touchRead(int &, int &) { return false; }

static bool down = false, held = false;
static int  idle;
static uint32_t since;

// One button, three gestures. A finished tap is held back for DOUBLE_GAP_MS
// to see whether a second one follows, so single taps arrive a moment late
// but double taps are unambiguous.
static bool     tapPending = false;
static uint32_t tapAt = 0;

InputEvent inputPoll() {
  if (inputPressed()) {
    if (!down) {
      down = true;
      held = false;
      since = millis();
      if (tapPending && millis() - tapAt <= DOUBLE_GAP_MS) {
        tapPending = false;
        held = true;                       // this press is spoken for
        return {IN_DOUBLE, 0, 0};
      }
    }
    idle = 0;
    if (!held && millis() - since >= LONG_PRESS_MS) {
      held = true;
      showHold(0);
      return {IN_HOLD, 0, 0};
    }
    if (!held) showHold((millis() - since) / (float)LONG_PRESS_MS);
    return {IN_NONE, 0, 0};
  }

  if (down && ++idle >= RELEASE_READS) {
    down = false;
    showHold(0);
    if (!held) { tapPending = true; tapAt = millis(); }   // hold it back briefly
  }
  if (tapPending && millis() - tapAt > DOUBLE_GAP_MS) {
    tapPending = false;
    return {IN_TAP, 0, 0};
  }
  return {IN_NONE, 0, 0};
}
#endif

InputEvent inputWait(uint32_t timeoutMs) {
  uint32_t start = millis();
  for (;;) {
    InputEvent e = inputPoll();
    if (e.kind == IN_TAP || e.kind == IN_DOUBLE || e.kind == IN_HOLD) return e;
    if (timeoutMs && millis() - start > timeoutMs) return {IN_NONE, 0, 0};
    delay(10);
  }
}
