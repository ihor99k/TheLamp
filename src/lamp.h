#pragma once

#include <Arduino.h>

#include "config.h"

// Settings that survive a power cycle (stored in NVS).
struct Settings {
  uint8_t version;
  uint8_t stripLen;    // NUM_LEDS at save time: reset `count` when the strip changes
  uint8_t brightness;  // 1..255, perceptual
  uint8_t r, g, b;
  uint8_t effect;
  uint8_t speed;       // 1..255, 128 = normal
  uint8_t count;       // how many LEDs are lit, 1..NUM_LEDS
  uint8_t align;       // where they are: 0 start, 1 center, 2 end, 3 spread evenly
  uint8_t limit;       // 1 = cap strip current at MAX_MILLIAMPS
};

class Lamp {
 public:
  void begin();
  void update();  // call from loop() as often as possible

  // One or more text commands separated by ';' or newlines, e.g. "bri 80; fx 3".
  void handleCommands(const char* text);

  void togglePower();
  void nextEffect();

  String stateJson() const;
  bool consumeChanged();  // true once after any state change
  static void printHelp();

 private:
  void handleOne(char* cmd);
  void setPower(bool value);
  void setEffect(int index);
  void setTimer(uint32_t minutes);
  void applySpeed();
  void applyPowerLimit();
  void stepLevel(uint32_t dt);
  void renderFrame(uint32_t dt);
  void markChanged(bool persist);
  void loadSettings();
  void saveIfDue(uint32_t now);

  Settings s_{};
  bool on_ = true;  // always start lit, so a wall switch works as expected
  float level_ = 0;  // current master brightness, fades towards target
  uint32_t fadeMs_ = 0;

  bool timerActive_ = false;
  uint32_t sleepAt_ = 0;

  uint64_t fxClockQ8_ = 0;  // effect clock in 1/256 ms
  uint32_t fxPrevT_ = 0;
  uint32_t speedQ8_ = 256;
  uint32_t lastFrame_ = 0;

  bool changed_ = false;
  bool dirty_ = false;
  uint32_t lastChange_ = 0;
};
