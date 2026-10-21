#pragma once

#include <Arduino.h>

// BLE link to the phone. Uses the Nordic UART Service layout, so besides our web page
// any BLE terminal app (nRF Connect, Serial Bluetooth Terminal, ...) can send commands.
namespace ble {

void begin(const char* name);

// Next command received from the phone, if any. Safe to call from loop().
bool poll(char* out, size_t len);

// Makes `state` readable by the phone and notifies subscribed clients.
void publish(const String& state);

}  // namespace ble
