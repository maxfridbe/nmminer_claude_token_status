// Bring-up probe for the 4" 480x320 ST7796 board (env:e32r40t-probe).
// Backlight is GPIO 27, active high (from the stock firmware). Draws color bars and a pin summary,
// then shows live touch pressure for an XPT2046 on the display bus.

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;

void setup() {
  Serial.begin(115200);
  delay(300);
  tft.init();                              // also switches the backlight on
  tft.setRotation(1);
  const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_YELLOW, TFT_MAGENTA};
  int w = tft.width() / 6;
  for (int i = 0; i < 6; i++) tft.fillRect(i * w, 0, w, 80, bars[i]);
  tft.fillRect(0, 80, tft.width(), tft.height() - 80, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char line[64];
  snprintf(line, sizeof(line), "ST7796 %dx%d  MOSI %d SCLK %d CS %d DC %d", tft.width(), tft.height(),
           TFT_MOSI, TFT_SCLK, TFT_CS, TFT_DC);
  tft.drawString(line, 8, 96, 2);
  tft.drawString("touch the screen:", 8, 130, 2);
  Serial.printf("[probe] %s\n", line);
}

void loop() {
  static uint16_t peak = 0;
  static uint32_t last = 0;
  uint16_t z = tft.getTouchRawZ();
  if (z > peak) peak = z;
  if (millis() - last > 250) {
    last = millis();
    char line[48];
    snprintf(line, sizeof(line), "pressure %4u  max %4u   ", z, peak);
    tft.drawString(line, 8, 150, 4);
  }
  delay(20);
}
