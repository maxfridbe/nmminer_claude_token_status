// Board profiles. One firmware per board; platformio.ini picks the profile
// with -D BOARD_CYD or -D BOARD_NMTV. Display pins live in platformio.ini
// (TFT_eSPI reads them from there); everything else board-specific is here.
#pragma once

#if defined(BOARD_NMTV)
// NMMiner NM-TV 1.54": ESP32, 240x240 ST7789, one touch button on top.
// Pins per NMTech's custom-firmware guide; the button pin isn't published,
// so it comes from the nmtv-probe build once there's hardware to probe.
  #define BOARD_ID          "nmtv"
  #define BOARD_NAME        "NM-TV 1.54in"
  #define SCR_W             240
  #define SCR_H             240
  #define TFT_ROTATION      0
  #define HAS_TOUCHSCREEN   0
  #define BL_ACTIVE_LOW     1        // TFT_BACKLIGHT_ON LOW in NMTech's setup
  #define PANEL_POWER_PIN   21       // display power switch; driven high (to confirm)
  #ifndef BUTTON_PIN
  #define BUTTON_PIN        -1       // unknown until probed
  #endif
  #ifndef BUTTON_TOUCHPAD
  #define BUTTON_TOUCHPAD   1        // 1: ESP32 touch pad (touchRead); 0: digital, e.g. a TTP223
  #endif
  #define BUTTON_ACTIVE     HIGH     // digital mode only

#else
// ESP32-2432S028 "Cheap Yellow Display": 320x240 ILI9341, resistive touch.
  #ifndef BOARD_CYD
  #define BOARD_CYD
  #endif
  #define BOARD_ID          "cyd"
  #define BOARD_NAME        "CYD 2.8in"
  #define SCR_W             320
  #define SCR_H             240
  #define TFT_ROTATION      1
  #define HAS_TOUCHSCREEN   1
  #define BL_ACTIVE_LOW     0
  #define PANEL_POWER_PIN   -1
#endif

#define SQUARE_SCREEN (SCR_W == SCR_H)
