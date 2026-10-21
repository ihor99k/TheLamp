#include "effects.h"

#include "config.h"

namespace {

// Per-frame fade amount for a trail that should mostly vanish after `lifeMs`.
uint8_t trailFade(uint32_t dt, uint32_t lifeMs) {
  uint32_t a = dt * 255 / lifeMs;
  if (a == 0 && dt > 0) a = 1;
  return a > 255 ? 255 : a;
}

CRGB scaled(CRGB c, uint8_t level) {
  c.nscale8_video(level);
  return c;
}

// Effect clock -> phase 0..255 of a cycle `periodMs` long.
uint8_t phase8(uint32_t t, uint32_t periodMs) {
  return (t % periodMs) * 256 / periodMs;
}

// Calm noise lookup into a palette (ocean, lava, aurora).
void noisePalette(FxContext& c, const CRGBPalette16& pal, uint16_t scale, uint8_t slowdown) {
  for (uint16_t i = 0; i < c.n; i++) {
    uint8_t v = inoise8(i * scale, c.t / slowdown);
    v = qsub8(v, 16);              // inoise8 rarely hits the extremes: stretch it
    v = qadd8(v, scale8(v, 39));
    c.px[i] = ColorFromPalette(pal, v, 255, LINEARBLEND);
  }
}

// ---------- effects ----------

void fxSolid(FxContext& c) {
  fill_solid(c.px, c.n, c.color);
}

void fxBreathe(FxContext& c) {
  uint8_t level = lerp8by8(40, 255, cubicwave8(phase8(c.t, 5000)));
  fill_solid(c.px, c.n, scaled(c.color, level));
}

void fxCandle(FxContext& c) {
  const CRGB flame(255, 96, 12), ember(255, 36, 0);
  for (uint16_t i = 0; i < c.n; i++) {
    uint8_t slow = inoise8(i * 50, c.t / 3);
    uint8_t fast = inoise8(i * 50 + 5000, c.t);
    uint8_t level = 45 + scale8(slow, 150) + scale8(fast, 60);
    c.px[i] = scaled(blend(ember, flame, level), level);
  }
}

uint8_t heat[NUM_LEDS];
uint32_t fireAcc;

void fireStep(uint16_t n) {
  const uint16_t cooling = 55, sparking = 120;
  uint16_t maxCool = cooling * 10 / n + 2;
  if (maxCool > 255) maxCool = 255;
  for (uint16_t i = 0; i < n; i++) heat[i] = qsub8(heat[i], random8(0, maxCool));
  for (int k = n - 1; k >= 2; k--) heat[k] = (heat[k - 1] + heat[k - 2] * 2) / 3;
  if (random8() < sparking) {
    uint8_t y = random8(n < 7 ? n : 7);
    heat[y] = qadd8(heat[y], random8(160, 255));
  }
}

void fxFire(FxContext& c) {
  fireAcc += c.dt;
  for (uint8_t steps = 0; fireAcc >= 16 && steps < 10; steps++) {
    fireAcc -= 16;
    fireStep(c.n);
  }
  if (fireAcc > 160) fireAcc = 0;
  for (uint16_t i = 0; i < c.n; i++) c.px[i] = HeatColor(heat[i]);
}

void fxRainbow(FxContext& c) {
  fill_rainbow(c.px, c.n, c.t / 30, 255 / c.n);
}

void fxHueCycle(FxContext& c) {
  fill_solid(c.px, c.n, CHSV(c.t / 60, 240, 255));
}

void fxWave(FxContext& c) {
  for (uint16_t i = 0; i < c.n; i++) {
    uint8_t ph = i * 21 - c.t / 8;
    c.px[i] = scaled(c.color, lerp8by8(30, 255, sin8(ph)));
  }
}

void fxComet(FxContext& c) {
  fadeToBlackBy(c.px, c.n, trailFade(c.dt, 350));
  c.px[(c.t / 60) % c.n] = c.color;
}

void fxScanner(FxContext& c) {
  fadeToBlackBy(c.px, c.n, trailFade(c.dt, 250));
  uint8_t eased = ease8InOutQuad(triwave8(phase8(c.t, 3000)));
  c.px[(uint32_t)eased * (c.n - 1) / 255] = c.color;
}

// Simplified TwinkleFOX: every pixel has its own pseudo-random clock.
uint8_t attackDecayWave8(uint8_t i) {
  if (i < 86) return i * 3;
  i -= 86;
  return 255 - (i + i / 2);
}

void fxStars(FxContext& c) {
  uint16_t prng = 11337;
  for (uint16_t i = 0; i < c.n; i++) {
    prng = prng * 2053 + 1384;
    uint16_t offset = prng;
    prng = prng * 2053 + 1384;
    uint8_t mul = ((((prng & 0xFF) >> 4) + (prng & 0x0F)) & 0x0F) + 0x08;
    uint8_t salt = prng >> 8;

    uint32_t clk = (uint32_t)(((uint64_t)c.t * mul) >> 3) + offset;
    uint32_t ticks = clk >> 4;
    uint16_t slow = (ticks >> 8) + salt;
    slow += sin8(slow);
    slow = slow * 2053 + 1384;
    uint8_t slow8 = (slow & 0xFF) + (slow >> 8);

    uint8_t bright = ((slow8 & 0x0E) / 2) < 4 ? attackDecayWave8(ticks) : 0;
    c.px[i] = scaled(c.color, bright);
  }
}

uint32_t confettiAcc;

void fxConfetti(FxContext& c) {
  fadeToBlackBy(c.px, c.n, trailFade(c.dt, 900));
  confettiAcc += c.dt;
  if (confettiAcc > 2000) confettiAcc = 0;
  while (confettiAcc >= 120) {
    confettiAcc -= 120;
    c.px[random16(c.n)] += CHSV(random8(), 200, 255);
  }
}

void fxOcean(FxContext& c) {
  noisePalette(c, OceanColors_p, 40, 10);
}

void fxLava(FxContext& c) {
  noisePalette(c, LavaColors_p, 30, 14);
}

DEFINE_GRADIENT_PALETTE(aurora_gp){
    0,   0,   4,   2,
   50,   0,  90,  30,
  100,   0, 255, 110,
  150,  10, 140, 160,
  200,  80,  10, 150,
  255,   0,   4,   2,
};
const CRGBPalette16 auroraPalette = aurora_gp;

void fxAurora(FxContext& c) {
  noisePalette(c, auroraPalette, 35, 12);
}

void fxJuggle(FxContext& c) {
  fadeToBlackBy(c.px, c.n, trailFade(c.dt, 300));
  for (uint8_t d = 0; d < 4; d++) {
    int16_t s = sin16((uint16_t)(c.t * (7 + d * 2)));
    uint16_t pos = (uint32_t)(s + 32768) * (c.n - 1) / 65535;
    c.px[pos] |= CHSV(d * 64, 200, 255);
  }
}

void fxChase(FxContext& c) {
  uint8_t step = (c.t / 180) % 3;
  CRGB dim = scaled(c.color, 25);
  for (uint16_t i = 0; i < c.n; i++) c.px[i] = (i + step) % 3 == 0 ? c.color : dim;
}

}  // namespace

const Effect EFFECTS[] = {
    {"solid", fxSolid},       {"breathe", fxBreathe}, {"candle", fxCandle},   {"fire", fxFire},
    {"rainbow", fxRainbow},   {"huecycle", fxHueCycle}, {"wave", fxWave},     {"comet", fxComet},
    {"scanner", fxScanner},   {"stars", fxStars},     {"confetti", fxConfetti}, {"ocean", fxOcean},
    {"lava", fxLava},         {"aurora", fxAurora},   {"juggle", fxJuggle},   {"chase", fxChase},
};
const uint8_t EFFECT_COUNT = sizeof(EFFECTS) / sizeof(EFFECTS[0]);

void effectsReset() {
  memset(heat, 0, sizeof(heat));
  fireAcc = 0;
  confettiAcc = 0;
}
