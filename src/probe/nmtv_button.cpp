// NM-TV button finder, with the panel lit (env:nmtv-probe).
//
// Panel power (GPIO 21) and backlight (GPIO 19) are both active LOW on this
// board. With the screen visible, this lists every touch pad live: pressing
// the button drops one number sharply, and "low" keeps the smallest seen.

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;

#define PANEL_POWER 21
#define BACKLIGHT   19
static const int PADS[] = {4, 12, 27, 32, 33};
#define NP (sizeof(PADS) / sizeof(PADS[0]))
static uint16_t base[NP], low[NP];

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PANEL_POWER, OUTPUT);
  digitalWrite(PANEL_POWER, LOW);
  tft.init();
  tft.setRotation(0);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, LOW);

  const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW};
  for (int i = 0; i < 4; i++) tft.fillRect(i * (tft.width() / 4), 0, tft.width() / 4, 36, bars[i]);
  tft.fillRect(0, 36, tft.width(), tft.height() - 36, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("press the button", 6, 44, 2);

  for (size_t i = 0; i < NP; i++) {
    uint32_t sum = 0;
    for (int k = 0; k < 16; k++) { sum += touchRead(PADS[i]); delay(5); }
    base[i] = low[i] = sum / 16;
  }
  Serial.println("[probe] panel lit, listing touch pads");
}

void loop() {
  static uint32_t last = 0;
  for (size_t i = 0; i < NP; i++) {
    uint16_t v = touchRead(PADS[i]);
    if (v < low[i]) low[i] = v;
    if (v < base[i] * 3 / 4)
      Serial.printf("[probe] PRESSED GPIO%d: %u (resting %u)\n", PADS[i], v, base[i]);
  }
  if (millis() - last > 400) {
    last = millis();
    int y = 70;
    for (size_t i = 0; i < NP; i++, y += 26) {
      char line[40];
      snprintf(line, sizeof(line), "GPIO%-3d %4u low %4u ", PADS[i], touchRead(PADS[i]), low[i]);
      tft.drawString(line, 6, y, 2);
    }
  }
  delay(40);
}
