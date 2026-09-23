// NM-TV touch-button finder (env:nmtv-probe). NMTech doesn't publish which
// pin the button is on, or whether it's an ESP32 touch pad or a touch IC
// with a digital output, so this watches both kinds and logs every change.
//
//   pio run -e nmtv-probe -t upload && pio device monitor
//   then touch the button a few times; the pin that reacts is the button.

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;

// ESP32 touch-pad GPIOs, minus the display's SPI pins (2, 13, 14, 15).
static const int PADS[] = {4, 0, 12, 27, 33, 32};
// Plain inputs that aren't the display, backlight or panel power.
static const int DIGITAL[] = {0, 4, 5, 12, 16, 17, 18, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39};

#define NP (sizeof(PADS) / sizeof(PADS[0]))
#define ND (sizeof(DIGITAL) / sizeof(DIGITAL[0]))

static uint16_t base[NP];
static bool padDown[NP];
static int level[ND];

static void show(const String &line) {
  static int y = 60;
  if (y > 220) { tft.fillRect(0, 56, 240, 184, TFT_BLACK); y = 60; }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line, 8, y, 2);
  y += 18;
  Serial.println("[probe] " + line);
}

void setup() {
  Serial.begin(115200);
  pinMode(21, OUTPUT);               // panel power, per NMTech's guide
  digitalWrite(21, HIGH);
  tft.init();
  tft.setRotation(0);
  pinMode(19, OUTPUT);               // backlight, active low
  digitalWrite(19, LOW);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Button probe", 8, 8, 4);
  tft.drawString("Touch the button a few times", 8, 36, 2);

  for (size_t i = 0; i < NP; i++) {
    uint32_t sum = 0;
    for (int k = 0; k < 16; k++) { sum += touchRead(PADS[i]); delay(5); }
    base[i] = sum / 16;
  }
  for (size_t i = 0; i < ND; i++) { pinMode(DIGITAL[i], INPUT); level[i] = digitalRead(DIGITAL[i]); }
  Serial.print("[probe] touch-pad baselines:");
  for (size_t i = 0; i < NP; i++) Serial.printf(" GPIO%d=%u", PADS[i], base[i]);
  Serial.println();
}

void loop() {
  for (size_t i = 0; i < NP; i++) {
    uint16_t v = touchRead(PADS[i]);
    bool down = v < base[i] * 3 / 4;
    if (down != padDown[i]) {
      padDown[i] = down;
      show(String("pad GPIO") + PADS[i] + (down ? " touched " : " released ") + v + "/" + base[i]);
    }
  }
  for (size_t i = 0; i < ND; i++) {
    int v = digitalRead(DIGITAL[i]);
    if (v != level[i]) {
      show(String("GPIO") + DIGITAL[i] + " " + level[i] + " -> " + v);
      level[i] = v;
    }
  }
  delay(30);
}
