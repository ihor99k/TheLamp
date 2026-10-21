#pragma once

#include <FastLED.h>

// Everything an effect needs to draw one frame.
struct FxContext {
  CRGB*    px;     // canvas: the first `n` pixels are the lit part of the strip
  uint16_t n;
  CRGB     color;  // user-selected color
  uint32_t t;      // effect clock, ms (already scaled by speed)
  uint32_t dt;     // effect-clock ms since the previous frame
};

struct Effect {
  const char* name;
  void (*render)(FxContext& c);
};

// Order matters: the index is what the app sends with "fx N" (keep web/index.html in sync).
extern const Effect  EFFECTS[];
extern const uint8_t EFFECT_COUNT;

// Clears internal state of effects (fire heat, timers). Call when switching effects.
void effectsReset();
