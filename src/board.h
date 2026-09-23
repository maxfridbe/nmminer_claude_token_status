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
  #ifndef BUTTON_ACTIVE
  #define BUTTON_ACTIVE     HIGH     // digital mode only
  #endif

#elif defined(BOARD_E32R40T)
// LCDWiki 4.0" ESP32-32E display, E32R40T (the E32N40T is the same board
// without touch): ESP32-WROOM-32E, 480x320 ST7796, XPT2046 resistive touch on
// the display bus. Pins were first read out of the factory test firmware by
// disassembly, then matched against LCDWiki's table: SPI 14/12/13, LCD CS 15,
// DC 2, reset tied to EN, backlight 27 (high = on), touch CS 33.
  #define BOARD_ID          "e32r40t"
  #define BOARD_NAME        "E32R40T 4.0in"
  #define SCR_W             480
  #define SCR_H             320
  #define TFT_ROTATION      1
  #define HAS_TOUCHSCREEN   1
  #define TOUCH_VIA_TFT     1
  #define TOUCH_CAL_DEFAULT {295, 3614, 269, 3492, 7}   // measured on one unit
  #define BL_ACTIVE_LOW     0
  #define PANEL_POWER_PIN   -1

#elif defined(BOARD_CROW28)
// CrowPanel "ESP32 Miner LCD-2.8 inch" (SKU DHM04728D): classic ESP32,
// 240x320 ILI9341 on the same SPI pins as the CYD, backlight on GPIO 27.
// Resistive touch (XPT2046) shares the display's SPI bus with chip select 33.
// Unlike the Cheap Yellow Display, this board wires MISO to GPIO 4, which is
// why touch reads came back empty until that was corrected.
  #define BOARD_ID          "crow28"
  #define BOARD_NAME        "CrowPanel 2.8in"
  #define SCR_W             320
  #define SCR_H             240
  #define TFT_ROTATION      1
  #define HAS_TOUCHSCREEN   1
  #define TOUCH_VIA_TFT     1        // shared SPI bus: tft.getTouch()
  // Starting point until calibrated on the board itself (measured on one unit).
  #define TOUCH_CAL_DEFAULT {181, 3522, 326, 3497, 1}
  #define BL_ACTIVE_LOW     0
  #define PANEL_POWER_PIN   -1

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

#ifndef TOUCH_VIA_TFT
#define TOUCH_VIA_TFT 0
#endif
#ifndef BUTTON_PIN
#define BUTTON_PIN -1        // touchscreen boards have no separate button
#endif
#ifndef TOUCH_CAL_DEFAULT
#define TOUCH_CAL_DEFAULT {0, 0, 0, 0, 0}
#endif

#define SQUARE_SCREEN (SCR_W == SCR_H)

// Boards with neither a touchscreen nor a button are set up entirely from a
// phone, so the hotspot has to offer itself rather than wait to be summoned.
#define HAS_INPUT (HAS_TOUCHSCREEN || BUTTON_PIN >= 0)
