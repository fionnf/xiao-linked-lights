// ============================================================
//  lamp.ino - two lamps that always show the same colour
// ============================================================
// Seeed XIAO ESP32-S3 + Wio-SX1262 + 39 SK6812 RGBW LEDs + a capacitive pad.
// Tap a lamp and both lamps change to the same new scene.
//
// Wiring (see README.md for why these pins and nothing else):
//     D6 (GPIO43) --[330R]--> DIN     SK6812 RGBW strip
//     D7 (GPIO44) --+-------> touch pad
//                   +--[1M]-> GND
//     strip VCC/GND from an external 5 V supply, ground shared with the XIAO
//
// Serial control, at 115200 - so colours can be driven without touching a pad:
//     tap            new random scene, broadcast to the other lamp
//     seed <n>       apply a specific scene by seed
//     power          toggle both lamps on/off
//     pos <0..1>     put the whole strip at one palette position
//     status         print state, radio stats and touch readings
//     cal            re-baseline the touch sensor
//     touchmon       stream live touch readings - use this WHILE wiring the pad
//     test           walk one pixel down the strip to check length and colour order
//     chain          replay the startup animation
//     sync           announce state so the other lamp can catch up
//     groups <0-4>   force a group count; 0 = random 2-3 per scene (default)
//     bright <0-1>   global brightness
//     fade <n>       crossfade length in frames (60 ~= 1 s)
//     breathe <0-1>  idle brightness shimmer depth; 0 = off
//     drift <secs>   seconds between autonomous scene changes; 0 = off

#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "colour.h"
#include "touch.h"
#include "link.h"

static Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRBW + NEO_KHZ800);
static ColourEngine engine;
static TouchSensor touch;
// named radioLink, not link: POSIX link(2) from unistd.h shadows it
static LampLink radioLink;

static uint32_t nodeId = 0;
static uint32_t lastFrame = 0;
static uint32_t lastDrift = 0;
static bool radioOk = false;
// Runtime-adjustable so the drift can be silenced while tuning by hand.
static uint32_t driftSecs = IDLE_DRIFT_INTERVAL_S;

// A seed that differs per lamp and per tap. The scene must look random, but
// both lamps must be able to reproduce it exactly, so the seed is what travels.
static uint32_t makeSeed() {
  return (uint32_t)micros() ^ (nodeId << 7) ^ (uint32_t)esp_random();
}

// Startup chain: light the strip one LED at a time, with a bright spark running
// ahead of the fill. Each LED lands on the colour it will actually settle to, so
// the animation resolves INTO the scene rather than flashing something unrelated
// and then jumping.
//
// Run on boot and whenever the lamp is switched on. It blocks for about a second,
// which is fine in both cases: at boot nothing else is happening yet, and on
// power-on the lamp is meant to be putting on a show.
static void startupChain() {
  const int SPARK = 4;          // how far the bright head extends behind itself
  const int STEP_MS = 22;       // 39 LEDs * 22 ms is a bit under a second

  for (int head = 0; head <= NUM_LEDS + SPARK; head++) {
    for (int i = 0; i < NUM_LEDS; i++) {
      if (i > head) {
        strip.setPixelColor(i, 0);          // not reached yet
        continue;
      }
      Rgbw c = engine.ledColour(i);         // the colour this LED settles to
      int behind = head - i;
      if (behind < SPARK) {
        // The moving head: lift toward white so the leading edge reads as a
        // spark travelling up the lamp rather than a dull wipe.
        float t = 1.0f - (float)behind / SPARK;   // 1 at the head, 0 at the tail
        uint8_t w = (uint8_t)min(255.0f, c.w + (255 - c.w) * t);
        uint8_t r = (uint8_t)min(255.0f, c.r + (255 - c.r) * t * 0.5f);
        uint8_t g = (uint8_t)min(255.0f, c.g + (255 - c.g) * t * 0.5f);
        uint8_t b = (uint8_t)min(255.0f, c.b + (255 - c.b) * t * 0.5f);
        strip.setPixelColor(i, strip.Color(r, g, b, w));
      } else {
        strip.setPixelColor(i, strip.Color(c.r, c.g, c.b, c.w));
      }
    }
    strip.show();
    delay(STEP_MS);
  }
}

static void showFrame() {
  for (int i = 0; i < NUM_LEDS; i++) {
    Rgbw c = engine.ledColour(i);
    strip.setPixelColor(i, strip.Color(c.r, c.g, c.b, c.w));
  }
  strip.show();
}

static void applyLocalTap() {
  uint32_t seed = makeSeed();
  engine.applyScene(seed);
  if (radioOk) radioLink.broadcastScene(seed);
  Serial.printf("[tap] new scene seed=%lu counter=%lu%s\n",
                (unsigned long)seed, (unsigned long)radioLink.counter(),
                radioOk ? " (broadcast)" : " (radio down, local only)");
}

static void handleSerial() {
  static char line[64];
  static int n = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (n < (int)sizeof(line) - 1) line[n++] = ch;
      continue;
    }
    line[n] = 0;
    n = 0;
    if (!strlen(line)) continue;

    if (!strcmp(line, "tap")) {
      applyLocalTap();
    } else if (!strncmp(line, "seed ", 5)) {
      uint32_t s = strtoul(line + 5, nullptr, 10);
      engine.applyScene(s);
      if (radioOk) radioLink.broadcastScene(engine.seed());
      Serial.printf("[seed] applied %lu and broadcast\n", (unsigned long)s);
    } else if (!strcmp(line, "power")) {
      engine.togglePower();
      radioLink.setPowered(engine.poweredOn());
      if (radioOk) radioLink.broadcastPower(engine.poweredOn());
      if (engine.poweredOn()) startupChain();
      Serial.printf("[power] now %s\n", engine.poweredOn() ? "ON" : "OFF");
    } else if (!strncmp(line, "pos ", 4)) {
      // Whole strip at one palette position - handy for eyeballing the palette.
      float p = atof(line + 4);
      Rgbw c = paletteColour(p);
      for (int i = 0; i < NUM_LEDS; i++)
        strip.setPixelColor(i, strip.Color(c.r * LED_BRIGHTNESS, c.g * LED_BRIGHTNESS,
                                           c.b * LED_BRIGHTNESS, c.w * LED_BRIGHTNESS));
      strip.show();
      Serial.printf("[pos] %.3f -> rgbw(%u,%u,%u,%u) held until next frame\n",
                    p, c.r, c.g, c.b, c.w);
      delay(1500);
    } else if (!strcmp(line, "status")) {
      Serial.printf("groups=%d sizes=", engine.activeGroups());
      for (int i = 0; i < MAX_GROUPS; i++)
        if (engine.groupSize(i)) Serial.printf("%d ", engine.groupSize(i));
      Serial.printf(" bright=%.2f fade=%d breathe=%.2f drift=%lus\n",
                    engine.brightness(), engine.fadeSteps(), engine.breatheDepth(),
                    (unsigned long)driftSecs);
      Serial.printf("node=%08lx counter=%lu seed=%lu power=%s radio=%s\n",
                    (unsigned long)nodeId, (unsigned long)radioLink.counter(),
                    (unsigned long)engine.seed(), engine.poweredOn() ? "on" : "off",
                    radioOk ? "ok" : "DOWN");
      Serial.printf("touch: last=%.1f baseline=%.1f threshold=%.1f\n",
                    touch.last(), touch.baseline(), touch.baseline() * TOUCH_THRESHOLD);
      Serial.printf("radio: last rssi=%.1f dBm snr=%.1f dB\n",
                    radioLink.lastRssi(), radioLink.lastSnr());
    } else if (!strncmp(line, "groups ", 7)) {
      int n = atoi(line + 7);
      engine.setGroupsOverride(n);
      engine.applyScene(engine.seed());     // re-lay the current scene
      Serial.printf("[groups] %s\n", n <= 0 ? "random 2-3 per scene"
                                             : String(n).c_str());
    } else if (!strncmp(line, "bright ", 7)) {
      engine.setBrightness(atof(line + 7));
      Serial.printf("[bright] %.2f\n", engine.brightness());
    } else if (!strncmp(line, "fade ", 5)) {
      engine.setFadeSteps(atoi(line + 5));
      Serial.printf("[fade] %d frames (~%.1f s)\n", engine.fadeSteps(),
                    engine.fadeSteps() / 60.0f);
    } else if (!strncmp(line, "breathe ", 8)) {
      engine.setBreatheDepth(atof(line + 8));
      Serial.printf("[breathe] depth %.2f\n", engine.breatheDepth());
    } else if (!strncmp(line, "drift ", 6)) {
      driftSecs = strtoul(line + 6, nullptr, 10);
      Serial.printf("[drift] %s\n", driftSecs ? (String(driftSecs) + " s").c_str()
                                              : "off");
    } else if (!strcmp(line, "chain")) {
      Serial.println("[chain] replaying the startup animation");
      startupChain();
    } else if (!strcmp(line, "sync")) {
      if (radioOk) radioLink.announceState();
      Serial.printf("[sync] announced counter=%lu seed=%lu - the other lamp will\n"
                    "       adopt it if it is behind, or correct us if it is ahead\n",
                    (unsigned long)radioLink.counter(), (unsigned long)engine.seed());
    } else if (!strcmp(line, "touchmon")) {
      // Live readings while the pad is being wired. A pin with nothing attached
      // sits at the 20000 safety cap because it never discharges; once the 1M
      // resistor is present the value drops to a few hundred and rises when
      // touched. Watching that transition is far quicker than guessing.
      Serial.println("[touchmon] 15 s of live readings - touch the pad to see it move");
      Serial.println("           raw    baseline   ratio   (ratio > threshold = touch)");
      uint32_t until = millis() + 15000;
      while (millis() < until) {
        touch.update();
        float raw = touch.last(), base = touch.baseline();
        float ratio = base > 0 ? raw / base : 0;
        Serial.printf("        %7.0f  %8.0f   %5.2f  %s\n", raw, base, ratio,
                      ratio > TOUCH_THRESHOLD ? "<< TOUCH" :
                      (raw > 19000 ? "(nothing connected?)" : ""));
        delay(250);
      }
      Serial.println("[touchmon] done");
    } else if (!strcmp(line, "test")) {
      // Walks a single white pixel down the strip. Confirms the LED count is
      // right and that the byte order really is GRBW - a wrong order shows as
      // the pixel being the wrong colour rather than white.
      Serial.printf("[test] walking one pixel down %d LEDs\n", NUM_LEDS);
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.clear();
        strip.setPixelColor(i, strip.Color(0, 0, 0, 120));   // W channel only
        strip.setPixelColor(i, strip.Color(60, 0, 0, 60));   // plus red tint
        strip.show();
        delay(60);
      }
      strip.clear();
      strip.show();
      Serial.println("[test] done - if the last lit LED was not the strip end, "
                     "NUM_LEDS is wrong");
    } else if (!strcmp(line, "cal")) {
      touch.calibrate();
      Serial.printf("[cal] baseline now %.1f\n", touch.baseline());
    } else {
      Serial.printf("? unknown '%s'\n  tap | seed N | power | pos F | status | cal | "
                    "touchmon | test\n  groups N | bright F | fade N | breathe F | drift N\n",
                    line);
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);

  uint64_t mac = ESP.getEfuseMac();
  nodeId = (uint32_t)((mac >> 24) & 0xFFFFFFFFull);

  Serial.println("\n========================================");
  Serial.printf("  lamp %08lx starting\n", (unsigned long)nodeId);
  Serial.println("========================================");

  strip.begin();
  strip.clear();
  strip.show();

  touch.begin(PIN_TOUCH);
  Serial.printf("touch baseline %.1f on GPIO%d\n", touch.baseline(), PIN_TOUCH);

  radioOk = radioLink.begin(nodeId);
  Serial.printf("radio %s\n", radioOk ? "ready" : "FAILED TO START");

  engine.begin(makeSeed());
  radioLink.setSeed(engine.seed());
  radioLink.setPowered(true);
  startupChain();
  if (radioOk) radioLink.announceState();   // ask the other lamp where we should be
  lastDrift = millis();
  Serial.println("commands: tap | seed N | power | pos F | status | cal | touchmon | test");
  Serial.println("tuning:   groups N | bright F | fade N | breathe F | drift N");
}

void loop() {
  handleSerial();

  // ---- radio ----
  if (radioOk) {
    LampMsg m;
    if (radioLink.poll(m)) {
      if (m.type == LAMP_SCENE || m.type == LAMP_STATE) {
        // A STATE message is handled exactly like a SCENE. poll() only returns
        // messages that genuinely win, so a STATE that arrives because we missed
        // a tap corrects us, and one that merely repeats what we already show
        // never gets here.
        if (m.seed != engine.seed()) {
          engine.applyScene(m.seed);
          Serial.printf("[rx] %s seed=%lu counter=%lu rssi=%.1f snr=%.1f%s\n",
                        m.type == LAMP_STATE ? "resync" : "scene",
                        (unsigned long)m.seed, (unsigned long)m.counter,
                        radioLink.lastRssi(), radioLink.lastSnr(),
                        m.type == LAMP_STATE ? "   <- recovered a missed tap" : "");
        }
        bool wantOn = (m.flags & 1) || m.type == LAMP_SCENE;
        if (m.type == LAMP_STATE && engine.poweredOn() != wantOn) {
          engine.togglePower();
          radioLink.setPowered(engine.poweredOn());
        }
      } else if (m.type == LAMP_POWER) {
        if (engine.poweredOn() != (bool)(m.flags & 1)) {
          engine.togglePower();
          if (engine.poweredOn()) startupChain();   // both lamps animate together
        }
        radioLink.setPowered(engine.poweredOn());
        Serial.printf("[rx] power %s\n", (m.flags & 1) ? "ON" : "OFF");
      }
    }
  }

  // Periodic state announcement. This is the safety net: a tap is sent once
  // with no retry, so without it a single lost packet would leave the lamps
  // showing different colours until somebody tapped again.
  if (radioOk) radioLink.tick();

  // ---- touch ----
  TouchSensor::Event ev = touch.update();
  if (ev == TouchSensor::TAP) {
    applyLocalTap();
  } else if (ev == TouchSensor::HOLD) {
    engine.togglePower();
    radioLink.setPowered(engine.poweredOn());
    if (radioOk) radioLink.broadcastPower(engine.poweredOn());
    if (engine.poweredOn()) startupChain();
    Serial.printf("[hold] power %s\n", engine.poweredOn() ? "ON" : "OFF");
  }

  // ---- 60 fps render ----
  uint32_t now = millis();
  if (now - lastFrame >= 16) {
    lastFrame = now;
    engine.tick(now);
    showFrame();
  }

  // ---- idle drift ----
  // A slow autonomous hue wander so the lamps are never completely static.
  // Only the lamp that drifts broadcasts, so both still agree.
  if (driftSecs && now - lastDrift > driftSecs * 1000UL) {
    lastDrift = now;
    applyLocalTap();
  }
}
