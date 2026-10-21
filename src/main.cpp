#include <Arduino.h>

#include "ble_control.h"
#include "config.h"
#include "lamp.h"
#include "wifi_control.h"

Lamp lamp;

void pollSerial() {
  static char line[128];
  static size_t len = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len) {
        line[len] = 0;
        if (!strncmp(line, "wifi", 4)) wifi::handleCommand(line);
        else lamp.handleCommands(line);
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = ch;
    }
  }
}

// Click: on/off. Hold: next effect, repeating while held.
void pollButton() {
  static bool wasDown = false;
  static bool held = false;
  static uint32_t downAt = 0;

  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (down && !wasDown) {
    downAt = now;
    held = false;
  }
  if (down && now - downAt >= LONG_PRESS_MS) {
    lamp.nextEffect();
    downAt = now;
    held = true;
  }
  if (!down && wasDown && !held && now - downAt > 30) lamp.togglePower();
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);  // don't stall when powered from a charger with no PC attached
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lamp.begin();
  wifi::begin(lamp);
  if (ENABLE_BLE) ble::begin(BLE_NAME);
  Serial.println("TheLamp ready. Type 'help' for commands, 'wifi' for network status.");
}

void loop() {
  char cmd[128];
  while (ENABLE_BLE && ble::poll(cmd, sizeof(cmd))) {
    if (Serial) Serial.printf("%lu BLE> %s\n", (unsigned long)millis(), cmd);
    lamp.handleCommands(cmd);
  }
  pollSerial();
  pollButton();
  wifi::update();

  lamp.update();

  // Report state changes to the phone, at most ~10 times a second.
  static bool pending = false;
  static uint32_t lastPublish = 0;
  if (lamp.consumeChanged()) pending = true;
  if (pending && millis() - lastPublish >= 100) {
    String state = lamp.stateJson();
    if (ENABLE_BLE) ble::publish(state);
    if (Serial) Serial.println(state);
    pending = false;
    lastPublish = millis();
  }
}
