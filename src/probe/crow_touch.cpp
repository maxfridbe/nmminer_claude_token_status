// Touch calibration for the CrowPanel miner LCD (env:crow28-probe).
//
// Touch works but lands in the wrong place, so run TFT_eSPI's calibration:
// touch the arrow in each corner when it appears. The five numbers it
// produces go into the firmware as this board's defaults, and are printed
// here and shown on screen.

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;
static uint16_t cal[5];

void setup() {
  Serial.begin(115200);
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Touch each corner arrow", 20, 100, 4);
  delay(1500);

  tft.fillScreen(TFT_BLACK);
  tft.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 20);

  char line[80];
  snprintf(line, sizeof(line), "%u, %u, %u, %u, %u", cal[0], cal[1], cal[2], cal[3], cal[4]);
  tft.fillScreen(TFT_BLACK);
  tft.drawString("Calibration:", 8, 8, 4);
  tft.drawString(line, 8, 44, 2);
  tft.drawString("now touch anywhere to check", 8, 70, 2);
  Serial.printf("[cal] %s\n", line);
  tft.setTouch(cal);
}

void loop() {
  uint16_t x, y;
  static uint32_t last = 0;
  if (tft.getTouch(&x, &y) && millis() - last > 150) {
    last = millis();
    tft.fillCircle(x, y, 3, TFT_GREEN);
    char at[40];
    snprintf(at, sizeof(at), "tap %3u,%3u   ", x, y);
    tft.drawString(at, 8, 96, 2);
    Serial.printf("[cal] tap %u,%u\n", x, y);
  }
  delay(20);
}
