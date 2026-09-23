// Dark (the default) or light: every screen picks each color as a pair, so
// light mode is one setting. Accents get deeper in light mode to stay legible
// on a pale background.
#pragma once
#include "settings.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

inline uint16_t themed(uint32_t dark, uint32_t light) {
  uint32_t h = cfg.lightMode ? light : dark;
  return tft.color565((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
}
