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

// A seed that differs per lamp and per tap. The scene must look random, but
// both lamps must be able to reproduce it exactly, so the seed is what travels.
static uint32_t makeSeed() {
  return (uint32_t)micros() ^ (nodeId << 7) ^ (uint32_t)esp_random();
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
      if (radioOk) radioLink.broadcastScene(s);
      Serial.printf("[seed] applied %lu and broadcast\n", (unsigned long)s);
    } else if (!strcmp(line, "power")) {
      engine.togglePower();
      if (radioOk) radioLink.broadcastPower(engine.poweredOn());
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
      Serial.printf("node=%08lx counter=%lu seed=%lu power=%s radio=%s\n",
                    (unsigned long)nodeId, (unsigned long)radioLink.counter(),
                    (unsigned long)engine.seed(), engine.poweredOn() ? "on" : "off",
                    radioOk ? "ok" : "DOWN");
      Serial.printf("touch: last=%.1f baseline=%.1f threshold=%.1f\n",
                    touch.last(), touch.baseline(), touch.baseline() * TOUCH_THRESHOLD);
      Serial.printf("radio: last rssi=%.1f dBm snr=%.1f dB\n",
                    radioLink.lastRssi(), radioLink.lastSnr());
    } else if (!strcmp(line, "cal")) {
      touch.calibrate();
      Serial.printf("[cal] baseline now %.1f\n", touch.baseline());
    } else {
      Serial.printf("? unknown command '%s' (tap|seed N|power|pos F|status|cal)\n", line);
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
  lastDrift = millis();
  Serial.println("commands: tap | seed N | power | pos F | status | cal");
}

void loop() {
  handleSerial();

  // ---- radio ----
  if (radioOk) {
    LampMsg m;
    if (radioLink.poll(m)) {
      if (m.type == LAMP_SCENE) {
        engine.applyScene(m.seed);
        Serial.printf("[rx] scene seed=%lu counter=%lu rssi=%.1f snr=%.1f\n",
                      (unsigned long)m.seed, (unsigned long)m.counter,
                      radioLink.lastRssi(), radioLink.lastSnr());
      } else if (m.type == LAMP_POWER) {
        if (engine.poweredOn() != (bool)(m.flags & 1)) engine.togglePower();
        Serial.printf("[rx] power %s\n", (m.flags & 1) ? "ON" : "OFF");
      }
    }
  }

  // ---- touch ----
  TouchSensor::Event ev = touch.update();
  if (ev == TouchSensor::TAP) {
    applyLocalTap();
  } else if (ev == TouchSensor::HOLD) {
    engine.togglePower();
    if (radioOk) radioLink.broadcastPower(engine.poweredOn());
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
  if (now - lastDrift > IDLE_DRIFT_INTERVAL_S * 1000UL) {
    lastDrift = now;
    applyLocalTap();
  }
}
