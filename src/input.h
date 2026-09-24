// Input as events: the CYD's touchscreen (taps with a position, swipes,
// holds) or the NM-TV's single touch button (taps and holds).
#pragma once
#include <Arduino.h>
#include "board.h"

enum InputKind { IN_NONE, IN_TAP, IN_DOUBLE, IN_HOLD, IN_SWIPE_LEFT, IN_SWIPE_RIGHT };
struct InputEvent {
  InputKind kind;
  int x, y;       // where the press started; touchscreens only
};

void       inputBegin();
// Called while a press is building toward a hold, with 0..1 of the way there,
// then 0 when it ends. Lets the screen show how long is left.
void       inputSetHoldIndicator(void (*fn)(float progress));
bool       inputPressed();                  // finger down right now
InputEvent inputPoll();                     // non-blocking; a hold fires once, while still held
InputEvent inputWait(uint32_t timeoutMs = 0);  // blocks for a tap or hold; IN_NONE on timeout
bool       touchRead(int &x, int &y);       // touchscreen boards: one calibrated sample
