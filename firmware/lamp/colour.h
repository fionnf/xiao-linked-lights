#pragma once
// ============================================================
//  colour.h - the organic colour engine
// ============================================================
// A direct port of colour.py from the Pico/MQTT build, with one important
// change: the whole scene is derived from a single 32-bit SEED.
//
// Why the seed matters. The original engine randomises group sizes, hues and
// white levels on every tap. To make two lamps agree, the old build sent the
// resulting scene over MQTT. Over LoRa that payload is expensive, so instead
// both lamps run the SAME deterministic generator from the SAME seed - four
// bytes on air reproduce an identical scene on both ends, randomness and all.
//
// The generator therefore must not use rand(): its sequence differs between
// implementations and even builds. A small explicit PRNG (xorshift32) is used
// so both lamps - and any future firmware - agree exactly.

#include <Arduino.h>
#include "config.h"

struct Rgbw {
  uint8_t r, g, b, w;
};

// ---- deterministic PRNG ----------------------------------------------------
// xorshift32: tiny, fast, and identical everywhere. Never swap this for rand().
class Rng {
 public:
  explicit Rng(uint32_t seed) : s_(seed ? seed : 0x1234567u) {}
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  float unit() { return (next() >> 8) / 16777216.0f; }     // 0.0 .. 1.0
  float range(float lo, float hi) { return lo + unit() * (hi - lo); }
  int intRange(int lo, int hi) {                            // inclusive
    if (hi <= lo) return lo;
    return lo + (int)(next() % (uint32_t)(hi - lo + 1));
  }

 private:
  uint32_t s_;
};

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Smoothstep: zero velocity at both ends, so a fade starts and finishes
// imperceptibly instead of visibly stepping.
static inline float ease(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

// Blend warm white toward a palette tint. `position` drives hue AND saturation
// together: at 0 the group is pure warm white, at 1 it is a fully saturated
// tint with the white channel off.
static inline Rgbw paletteColour(float position) {
  if (position < 0.0f) position = 0.0f;
  if (position > 1.0f) position = 1.0f;
  float scaled = position * (TINT_COUNT - 1);
  int idx = (int)scaled;
  float frac = scaled - idx;
  float tr, tg, tb;
  if (idx >= TINT_COUNT - 1) {
    tr = TINT_PALETTE[TINT_COUNT - 1][0];
    tg = TINT_PALETTE[TINT_COUNT - 1][1];
    tb = TINT_PALETTE[TINT_COUNT - 1][2];
  } else {
    tr = lerpf(TINT_PALETTE[idx][0], TINT_PALETTE[idx + 1][0], frac);
    tg = lerpf(TINT_PALETTE[idx][1], TINT_PALETTE[idx + 1][1], frac);
    tb = lerpf(TINT_PALETTE[idx][2], TINT_PALETTE[idx + 1][2], frac);
  }
  float sat = position;
  Rgbw c;
  c.r = (uint8_t)lerpf(BASE_WARM_WHITE[0], tr, sat);
  c.g = (uint8_t)lerpf(BASE_WARM_WHITE[1], tg, sat);
  c.b = (uint8_t)lerpf(BASE_WARM_WHITE[2], tb, sat);
  c.w = (uint8_t)(BASE_WARM_WHITE[3] * (1.0f - sat));
  return c;
}

// ---- the engine ------------------------------------------------------------
class ColourEngine {
 public:
  void begin(uint32_t seed) {
    poweredOn_ = true;
    powerLevel_ = 1.0f;
    for (int i = 0; i < MAX_GROUPS; i++) {
      pos_[i] = targetPos_[i] = 0.0f;
      wLevel_[i] = wTarget_[i] = wStart_[i] = 1.0f;
      startPos_[i] = 0.0f;
      fadeStep_[i] = fadeSteps_;              // start settled, not mid-fade
      breathePhase_[i] = i * (6.2831853f / MAX_GROUPS);
    }
    applyScene(seed);
  }

  // A tap. Everything about the new scene comes from `seed`, so a lamp that
  // receives the same seed produces an identical scene without further data.
  void applyScene(uint32_t seed) {
    if (!poweredOn_) return;
    seed_ = seed;
    Rng rng(seed);
    // Drawn from the seed, so both lamps pick the SAME number of groups.
    active_ = groupsOverride_ > 0 ? groupsOverride_
                                  : rng.intRange(GROUPS_MIN, GROUPS_MAX);
    if (active_ > MAX_GROUPS) active_ = MAX_GROUPS;
    partition(rng);
    for (int i = 0; i < active_; i++) {
      startPos_[i] = pos_[i];
      targetPos_[i] = rng.range(0.0f, 1.0f);
      wStart_[i] = wLevel_[i];
      wTarget_[i] = rng.range(0.6f, 1.0f);
      fadeStep_[i] = 0;
    }
  }

  void togglePower() {
    poweredOn_ = !poweredOn_;
    powerDir_ = poweredOn_ ? 1 : -1;
  }

  bool poweredOn() const { return poweredOn_; }
  uint32_t seed() const { return seed_; }
  int activeGroups() const { return active_; }

  // Runtime tuning. These exist because judging how a scene reads is a visual
  // decision, and a reflash cycle per adjustment makes that painful.
  void setBrightness(float b) { brightness_ = constrain(b, 0.0f, 1.0f); }
  float brightness() const { return brightness_; }
  void setFadeSteps(int n) { fadeSteps_ = max(1, n); }
  int fadeSteps() const { return fadeSteps_; }
  void setGroupsOverride(int n) { groupsOverride_ = (n <= 0) ? 0 : min(n, MAX_GROUPS); }
  int groupsOverride() const { return groupsOverride_; }
  void setBreatheDepth(float d) { breatheDepth_ = constrain(d, 0.0f, 1.0f); }
  float breatheDepth() const { return breatheDepth_; }
  int groupSize(int i) const { return (i >= 0 && i < MAX_GROUPS) ? groupSize_[i] : 0; }

  // Advance fades, breathing and the power ramp by one frame.
  void tick(uint32_t nowMs) {
    for (int i = 0; i < MAX_GROUPS; i++) {
      if (fadeStep_[i] < fadeSteps_) {
        fadeStep_[i]++;
        float t = ease((float)fadeStep_[i] / fadeSteps_);
        // Interpolate from the fade's START value, not the current one:
        // lerping from `current` each step compounds the easing and a
        // nominal 1 s fade finishes in about a third of that.
        pos_[i] = lerpf(startPos_[i], targetPos_[i], t);
        wLevel_[i] = lerpf(wStart_[i], wTarget_[i], t);
      }
      breathePhase_[i] += BREATHE_SPEED * 16.0f;
    }
    if (powerDir_ > 0 && powerLevel_ < 1.0f) powerLevel_ = min(1.0f, powerLevel_ + 0.02f);
    if (powerDir_ < 0 && powerLevel_ > 0.0f) powerLevel_ = max(0.0f, powerLevel_ - 0.02f);
    (void)nowMs;
  }

  // Which group owns LED `i`, honouring REVERSE_LEDS for a lamp joined at the
  // far end of the strip.
  Rgbw ledColour(int index) const {
    int i = REVERSE_LEDS ? (NUM_LEDS - 1 - index) : index;
    int g = 0, acc = 0;
    for (; g < MAX_GROUPS; g++) {
      acc += groupSize_[g];
      if (i < acc) break;
    }
    if (g >= MAX_GROUPS) g = active_ - 1;

    Rgbw c = paletteColour(pos_[g]);
    float breathe = 1.0f + breatheDepth_ * sinf(breathePhase_[g]);
    float scale = brightness_ * powerLevel_ * breathe;
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;

    Rgbw out;
    out.r = (uint8_t)(c.r * scale);
    out.g = (uint8_t)(c.g * scale);
    out.b = (uint8_t)(c.b * scale);
    out.w = (uint8_t)(c.w * scale * wLevel_[g]);
    return out;
  }

 private:
  // Random group sizes that always sum to exactly NUM_LEDS. Any shortfall
  // would leave trailing LEDs showing a stale colour from the previous scene.
  void partition(Rng &rng) {
    for (int i = 0; i < MAX_GROUPS; i++) groupSize_[i] = 0;
    int remaining = NUM_LEDS;
    for (int i = 0; i < active_; i++) {
      int left = active_ - i;
      int lo = max(GROUP_MIN_LEDS, remaining - (left - 1) * GROUP_MAX_LEDS);
      int hi = min(GROUP_MAX_LEDS, remaining - (left - 1) * GROUP_MIN_LEDS);
      if (lo > hi) lo = hi = max(1, remaining - (left - 1));
      int size = (i == active_ - 1) ? remaining : rng.intRange(lo, hi);
      groupSize_[i] = size;
      remaining -= size;
    }
    if (remaining != 0) groupSize_[active_ - 1] += remaining;
  }

  float pos_[MAX_GROUPS], startPos_[MAX_GROUPS], targetPos_[MAX_GROUPS];
  float wLevel_[MAX_GROUPS], wStart_[MAX_GROUPS], wTarget_[MAX_GROUPS];
  float breathePhase_[MAX_GROUPS];
  int fadeStep_[MAX_GROUPS];
  int groupSize_[MAX_GROUPS];
  bool poweredOn_ = true;
  float powerLevel_ = 1.0f;
  int powerDir_ = 0;
  uint32_t seed_ = 1;
  int active_ = GROUPS_MAX;
  int groupsOverride_ = 0;              // 0 = random per scene
  float brightness_ = LED_BRIGHTNESS;
  int fadeSteps_ = FADE_STEPS;
  float breatheDepth_ = BREATHE_DEPTH;
};
