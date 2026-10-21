#pragma once

#include <stdint.h>

// ---------- Стрічка ----------
#define LED_PIN         4          // GPIO -> DIN стрічки (через резистор 330 Ом)
#define LED_CHIPSET     WS2812B
#define LED_COLOR_ORDER GRB        // якщо червоний і зелений переплутані — постав RGB

constexpr uint16_t DEFAULT_LEDS  = 30;    // діодів на стрічці, поки не змінять на сторінці /setup
constexpr uint16_t MAX_LEDS      = 300;   // найдовша стрічка, яку можна вибрати
constexpr uint32_t MAX_MILLIAMPS = 1500;  // ліміт струму стрічки при 5V (під блок живлення 2A);
                                          // вимикається зі сторінки або командою "limit 0"

// ---------- Кнопка ----------
// BOOT на платі: клік — увімк/вимк, утримання — наступний ефект
constexpr uint8_t  BUTTON_PIN    = 0;
constexpr uint32_t LONG_PRESS_MS = 700;

// ---------- Bluetooth ----------
constexpr const char* BLE_NAME = "TheLamp";
// Wi-Fi setup/control is the default. Keep BLE dormant unless explicitly needed.
constexpr bool ENABLE_BLE = false;

// ---------- Wi-Fi ----------
constexpr const char* WIFI_SETUP_NAME = "TheLamp-Setup";     // точка доступу для налаштування
constexpr const char* WIFI_SETUP_PASSWORD = "";  // "" = відкрита мережа; інакше мінімум 8 символів
constexpr const char* WIFI_HOSTNAME = "thelamp";             // http://thelamp.local
