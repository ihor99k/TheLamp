#include "lamp.h"

#include <FastLED.h>
#include <Preferences.h>

#include "effects.h"

namespace {

constexpr uint8_t  SETTINGS_VERSION = 3;
constexpr uint32_t SAVE_DELAY_MS    = 3000;   // wait for sliders to settle before writing flash
constexpr uint32_t FRAME_MS         = 16;     // ~60 fps for effects
constexpr uint32_t FADE_MS          = 600;    // on/off transitions
constexpr uint32_t BRI_FADE_MS      = 150;    // brightness changes: short, so the slider feels live
constexpr uint32_t SLEEP_FADE_MS    = 30000;  // slow fade-out when the sleep timer ends
constexpr uint32_t MAX_TIMER_MIN    = 720;

const Settings DEFAULTS = {
    SETTINGS_VERSION, NUM_LEDS,
    80,            // brightness
    255, 147, 41,  // warm white
    0,             // effect: solid color
    128,           // speed
    NUM_LEDS,      // count
    0,             // align
    1,             // current limit on
};

CRGB leds[NUM_LEDS];   // what is sent to the strip
CRGB frame[NUM_LEDS];  // effect canvas, the first `count` pixels are used
Preferences prefs;

// Where the k-th of `count` lit LEDs sits on the strip.
uint8_t ledPosition(uint8_t k, uint8_t count, uint8_t align) {
  switch (align) {
    case 1: return (NUM_LEDS - count) / 2 + k;
    case 2: return NUM_LEDS - count + k;
    case 3: return (2 * k + 1) * NUM_LEDS / (2 * count);  // middle of each equal share
    default: return k;
  }
}

bool parseColor(const char* a, CRGB& out) {
  if (*a == '#') a++;
  if (strlen(a) == 6 && strspn(a, "0123456789abcdefABCDEF") == 6) {
    out = CRGB(strtoul(a, nullptr, 16));
    return true;
  }
  int r, g, b;
  if (sscanf(a, "%d%*[ ,]%d%*[ ,]%d", &r, &g, &b) == 3) {
    out = CRGB(constrain(r, 0, 255), constrain(g, 0, 255), constrain(b, 0, 255));
    return true;
  }
  return false;
}

}  // namespace

void Lamp::begin() {
  loadSettings();
  applySpeed();
  fadeMs_ = FADE_MS;

  FastLED.addLeds<LED_CHIPSET, LED_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  applyPowerLimit();
  FastLED.setBrightness(0);
  FastLED.clear(true);
}

void Lamp::update() {
  uint32_t now = millis();

  if (timerActive_ && (int32_t)(now - sleepAt_) >= 0) {
    timerActive_ = false;
    on_ = false;
    fadeMs_ = SLEEP_FADE_MS;
    markChanged(false);
  }

  uint32_t dt = now - lastFrame_;
  if (dt >= FRAME_MS) {
    lastFrame_ = now;
    if (dt > 100) dt = 100;
    stepLevel(dt);
    if (level_ > 0) renderFrame(dt);
  }

  // Pushing the strip more often than frames are drawn lets FastLED dither,
  // which keeps colors smooth at the low brightness a night light runs at.
  FastLED.show();
  saveIfDue(now);
}

void Lamp::stepLevel(uint32_t dt) {
  float target = on_ ? s_.brightness : 0;
  float step = 255.0f * dt / fadeMs_;
  level_ = level_ < target ? fminf(level_ + step, target) : fmaxf(level_ - step, target);
  if (!on_ && level_ == 0) fadeMs_ = FADE_MS;

  // Perceptual curve: the slider feels linear to the eye.
  uint8_t pwm = (uint8_t)(level_ * level_ / 255);
  if (pwm == 0 && level_ > 0.5f) pwm = 1;
  FastLED.setBrightness(pwm);
}

void Lamp::renderFrame(uint32_t dt) {
  fxClockQ8_ += (uint64_t)dt * speedQ8_;
  uint32_t t = fxClockQ8_ >> 8;
  FxContext c{frame, s_.count, CRGB(s_.r, s_.g, s_.b), t, t - fxPrevT_};
  fxPrevT_ = t;
  EFFECTS[s_.effect].render(c);

  // Effects draw only the lit LEDs, so animations stay continuous even when spread out.
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t k = 0; k < s_.count; k++) leds[ledPosition(k, s_.count, s_.align)] = frame[k];
}

// ---------- commands ----------

void Lamp::handleCommands(const char* text) {
  char buf[160];
  strlcpy(buf, text, sizeof(buf));
  char* save = nullptr;
  for (char* tok = strtok_r(buf, ";\r\n", &save); tok; tok = strtok_r(nullptr, ";\r\n", &save)) {
    handleOne(tok);
  }
}

void Lamp::handleOne(char* cmd) {
  while (isspace((unsigned char)*cmd)) cmd++;
  if (!*cmd) return;

  char* arg = cmd + strcspn(cmd, " =:");
  if (*arg) {
    *arg++ = 0;
    while (isspace((unsigned char)*arg)) arg++;
  }
  for (char* p = cmd; *p; p++) *p = tolower((unsigned char)*p);
  long v = atol(arg);

  if (!strcmp(cmd, "on")) setPower(true);
  else if (!strcmp(cmd, "off")) setPower(false);
  else if (!strcmp(cmd, "toggle")) togglePower();
  else if (!strcmp(cmd, "power")) setPower(v != 0);
  else if (!strcmp(cmd, "bri")) {
    s_.brightness = constrain(v, 1, 255);
    if (on_) fadeMs_ = BRI_FADE_MS;
    else setPower(true);
    markChanged(true);
  } else if (!strcmp(cmd, "color")) {
    CRGB c;
    if (!parseColor(arg, c)) {
      Serial.printf("Bad color: %s\n", arg);
      return;
    }
    s_.r = c.r, s_.g = c.g, s_.b = c.b;
    setPower(true);
    markChanged(true);
  } else if (!strcmp(cmd, "fx")) setEffect(v);
  else if (!strcmp(cmd, "next")) nextEffect();
  else if (!strcmp(cmd, "speed")) {
    s_.speed = constrain(v, 1, 255);
    applySpeed();
    markChanged(true);
  } else if (!strcmp(cmd, "count")) {
    s_.count = constrain(v, 1, NUM_LEDS);
    markChanged(true);
  } else if (!strcmp(cmd, "align")) {
    s_.align = constrain(v, 0, 3);
    markChanged(true);
  } else if (!strcmp(cmd, "limit")) {
    s_.limit = v != 0;
    applyPowerLimit();
    markChanged(true);
  } else if (!strcmp(cmd, "timer")) setTimer(constrain(v, 0, (long)MAX_TIMER_MIN));
  else if (!strcmp(cmd, "get")) changed_ = true;
  else if (!strcmp(cmd, "help")) printHelp();
  else Serial.printf("Unknown command: %s (type 'help')\n", cmd);
}

void Lamp::setPower(bool value) {
  on_ = value;
  fadeMs_ = FADE_MS;
  markChanged(false);
}

void Lamp::togglePower() {
  setPower(!on_);
}

void Lamp::setEffect(int index) {
  if (index < 0 || index >= EFFECT_COUNT) return;
  s_.effect = index;
  effectsReset();
  fill_solid(frame, NUM_LEDS, CRGB::Black);
  setPower(true);
  markChanged(true);
}

void Lamp::nextEffect() {
  setEffect((s_.effect + 1) % EFFECT_COUNT);
}

void Lamp::setTimer(uint32_t minutes) {
  timerActive_ = minutes > 0;
  sleepAt_ = millis() + minutes * 60000UL;
  if (timerActive_) setPower(true);
  markChanged(false);
}

void Lamp::applyPowerLimit() {
  // FastLED has no "off" switch for the limiter: an unreachable budget does the same.
  FastLED.setMaxPowerInMilliWatts(s_.limit ? 5 * MAX_MILLIAMPS : 0xFFFFFFFF);
}

void Lamp::applySpeed() {
  // 1..255 -> 0.16x..6.2x, exponential so the slider feels even
  speedQ8_ = (uint32_t)(256.0f * powf(2.0f, (s_.speed - 128) / 48.0f));
}

// ---------- state ----------

String Lamp::stateJson() const {
  uint32_t left = timerActive_ ? (sleepAt_ - millis()) / 1000 : 0;
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"on\":%d,\"bri\":%u,\"color\":\"%02X%02X%02X\",\"fx\":%u,\"speed\":%u,"
           "\"count\":%u,\"align\":%u,\"limit\":%u,\"ma\":%lu,\"timer\":%lu,"
           "\"leds\":%u,\"nfx\":%u}",
           on_, s_.brightness, s_.r, s_.g, s_.b, s_.effect, s_.speed, s_.count, s_.align,
           s_.limit, (unsigned long)MAX_MILLIAMPS, (unsigned long)left, NUM_LEDS, EFFECT_COUNT);
  return buf;
}

bool Lamp::consumeChanged() {
  bool c = changed_;
  changed_ = false;
  return c;
}

void Lamp::markChanged(bool persist) {
  changed_ = true;
  if (persist) {
    dirty_ = true;
    lastChange_ = millis();
  }
}

void Lamp::loadSettings() {
  s_ = DEFAULTS;
  prefs.begin("lamp", false);
  Settings stored;
  if (prefs.getBytesLength("s") == sizeof(stored) && prefs.getBytes("s", &stored, sizeof(stored)) &&
      stored.version == SETTINGS_VERSION) {
    s_ = stored;
  }
  if (s_.stripLen != NUM_LEDS) {
    s_.stripLen = NUM_LEDS;
    s_.count = NUM_LEDS;
  }
  s_.brightness = constrain(s_.brightness, 1, 255);
  s_.speed = constrain(s_.speed, 1, 255);
  s_.count = constrain(s_.count, 1, NUM_LEDS);
  if (s_.effect >= EFFECT_COUNT) s_.effect = 0;
  if (s_.align > 3) s_.align = 0;
  s_.limit = s_.limit != 0;
}

void Lamp::saveIfDue(uint32_t now) {
  if (!dirty_ || now - lastChange_ < SAVE_DELAY_MS) return;
  prefs.putBytes("s", &s_, sizeof(s_));
  dirty_ = false;
}

void Lamp::printHelp() {
  Serial.println(
      "Commands (separate with ';'):\n"
      "  on | off | toggle        power\n"
      "  bri 1..255               brightness\n"
      "  color RRGGBB | r,g,b     color\n"
      "  fx N | next              effect\n"
      "  speed 1..255             effect speed (128 = normal)\n"
      "  count 1..N               how many LEDs are lit\n"
      "  align 0|1|2|3            lit LEDs at start | center | end | spread evenly\n"
      "  limit 1|0                strip current limit on | off\n"
      "  timer MIN                sleep timer, 0 = cancel\n"
      "  get                      print state\n"
      "  wifi | wifi forget       network status | forget the saved network\n"
      "Effects:");
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) Serial.printf("  %2u  %s\n", i, EFFECTS[i].name);
}
