// ============================================================
//  lamp.ino - two lamps that always show the same colour
// ============================================================
// Seeed XIAO ESP32-S3 + Wio-SX1262 + 39 SK6812 RGBW LEDs + a capacitive pad.
// Tap a lamp and both lamps change to the same new scene.
//
// Wiring (see README.md for why these pins and nothing else):
//     D6 (GPIO43) --[330R]--> DIN     SK6812 RGBW strip
//     D7 (GPIO44) <---------- OUT     TTP223 touch module (VCC/GND -> 3V3/GND)
//     strip VCC/GND from an external 5 V supply, ground shared with the XIAO
//
// Serial control, at 115200 - so colours can be driven without touching a pad:
//     tap            new random scene, broadcast to the other lamp
//     seed <n>       apply a specific scene by seed
//     power          toggle both lamps on/off
//     pos <0..1>     put the whole strip at one palette position
//     colour R G B W an explicitly chosen colour, synced to the other lamp
//     status         print state, radio stats and touch readings
//     cal            re-baseline the touch sensor
//     touchmon       stream live touch readings - use this WHILE wiring the pad
//     test           walk one pixel down the strip to check length and colour order
//     chain          replay the startup animation
//     sync           announce state so the other lamp can catch up
//     mesh           reboot into the Meshtastic build (long range, phone app)
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
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <Preferences.h>

static Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRBW + NEO_KHZ800);
static ColourEngine engine;
static TouchSensor touch;
// named radioLink, not link: POSIX link(2) from unistd.h shadows it
static LampLink radioLink;

// Remembered across power cuts. Without this a lamp comes back showing a random
// scene and stays wrong until the other lamp's next announcement - up to 15 s of
// two lamps visibly disagreeing after every power blip, which is exactly when
// someone is looking at them.
static Preferences prefs;
static uint32_t pendingSaveAt = 0;
static uint32_t savedCode = 0;

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

// Debounced so a burst of changes costs one write, not one per frame. NVS
// wear-levels, but there is no reason to hammer it.
static void scheduleSave() { pendingSaveAt = millis() + 3000; }

static void saveStateNow() {
  pendingSaveAt = 0;
  uint32_t code = engine.visualCode();
  if (code == savedCode) return;          // nothing actually changed
  savedCode = code;
  prefs.putBool("solid", engine.isSolid());
  prefs.putUInt("payload", engine.isSolid() ? engine.packedSolid() : engine.seed());
  prefs.putUInt("counter", radioLink.counter());
  prefs.putBool("power", engine.poweredOn());
}

static bool restoreState() {
  if (!prefs.isKey("payload")) return false;
  uint32_t payload = prefs.getUInt("payload", 0);
  bool solid = prefs.getBool("solid", false);
  if (solid) {
    Rgbw c{(uint8_t)(payload & 0xFF), (uint8_t)((payload >> 8) & 0xFF),
           (uint8_t)((payload >> 16) & 0xFF), (uint8_t)((payload >> 24) & 0xFF)};
    engine.setSolid(c);
  } else {
    engine.applyScene(payload);
  }
  if (!prefs.getBool("power", true)) engine.togglePower();
  savedCode = engine.visualCode();
  return true;
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
  radioLink.setSolidFlag(false);
  scheduleSave();
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
      scheduleSave();
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
      if (engine.isSolid()) {
        Rgbw c = engine.solidColour();
        Serial.printf("mode=colour rgbw(%u,%u,%u,%u)\n", c.r, c.g, c.b, c.w);
      } else {
        Serial.println("mode=scene");
      }
      Serial.printf("groups=%d sizes=", engine.activeGroups());
      for (int i = 0; i < MAX_GROUPS; i++)
        if (engine.groupSize(i)) Serial.printf("%d ", engine.groupSize(i));
      Serial.printf(" bright=%.2f fade=%d breathe=%.2f drift=%lus\n",
                    engine.brightness(), engine.fadeSteps(), engine.breatheDepth(),
                    (unsigned long)driftSecs);
      Serial.printf("code=%08lx  <- both lamps must show the same code\n",
                    (unsigned long)engine.visualCode());
      Serial.printf("node=%08lx counter=%lu seed=%lu power=%s radio=%s\n",
                    (unsigned long)nodeId, (unsigned long)radioLink.counter(),
                    (unsigned long)engine.seed(), engine.poweredOn() ? "on" : "off",
                    radioOk ? "ok" : "DOWN");
      Serial.printf("touch: pin GPIO%d raw=%d level=%.2f threshold=%.2f -> %s\n",
                    PIN_TOUCH, touch.raw() ? 1 : 0, touch.level(), TOUCH_THRESHOLD,
                    touch.level() >= TOUCH_THRESHOLD ? "TOUCHED" : "idle");
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
    } else if (!strncmp(line, "colour ", 7) || !strncmp(line, "color ", 6)) {
      // "colour R G B W" - an explicitly chosen colour, as opposed to a scene
      // generated from a seed. Broadcast so both lamps show the same one.
      const char *a = strchr(line, ' ') + 1;
      int r = 0, g = 0, b = 0, w = 0;
      int got = sscanf(a, "%d %d %d %d", &r, &g, &b, &w);
      if (got < 3) {
        Serial.println("? usage: colour R G B W   (0-255 each, W optional)");
      } else {
        Rgbw c{(uint8_t)constrain(r,0,255), (uint8_t)constrain(g,0,255),
               (uint8_t)constrain(b,0,255), (uint8_t)constrain(w,0,255)};
        engine.setSolid(c);
        scheduleSave();
        if (radioOk) radioLink.broadcastColour(c.r, c.g, c.b, c.w);
        Serial.printf("[colour] rgbw(%u,%u,%u,%u) applied and broadcast\n",
                      c.r, c.g, c.b, c.w);
      }
    } else if (!strcmp(line, "mesh")) {
      // Both firmwares live in flash at once - this build in app1, the
      // Meshtastic build in app0 - because they share an identical 8 MB
      // partition table. Switching is therefore just a matter of pointing the
      // bootloader at the other slot; nothing is erased and nothing is
      // downloaded, so it is instant and reversible.
      const esp_partition_t *other =
          esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                   ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (!other) {
        Serial.println("[mesh] app0 not found - is the Meshtastic build flashed?");
      } else if (esp_ota_set_boot_partition(other) != ESP_OK) {
        Serial.println("[mesh] could not set boot partition");
      } else {
        Serial.println("[mesh] rebooting into Meshtastic. To come back, send a");
        Serial.println("       Meshtastic text message saying: lamp fast");
        strip.clear(); strip.show();
        delay(300);
        esp_restart();
      }
    } else if (!strcmp(line, "chain")) {
      Serial.println("[chain] replaying the startup animation");
      startupChain();
    } else if (!strcmp(line, "sync")) {
      if (radioOk) radioLink.announceState();
      radioLink.setCode(engine.visualCode());
      Serial.printf("[sync] announced code=%08lx counter=%lu seed=%lu - the other lamp will\n"
                    "       adopt it if it is behind, or correct us if it is ahead\n",
                    (unsigned long)engine.visualCode(),
                    (unsigned long)radioLink.counter(), (unsigned long)engine.seed());
    } else if (!strcmp(line, "touchmon")) {
      // Live readings while the module is being wired or the threshold is
      // being tuned. raw is the instantaneous pin state; level is the
      // smoothed 0..1 value TOUCH_THRESHOLD is actually compared against -
      // watch level, not raw, to judge whether TOUCH_THRESHOLD needs moving.
      Serial.printf("[touchmon] 15 s of live readings, GPIO%d, threshold=%.2f\n",
                    PIN_TOUCH, TOUCH_THRESHOLD);
      Serial.println("           raw   level  bar");
      uint32_t until = millis() + 15000;
      while (millis() < until) {
        touch.update();
        int bars = (int)(touch.level() * 20.0f + 0.5f);
        char bar[21];
        for (int i = 0; i < 20; i++) bar[i] = i < bars ? '#' : '.';
        bar[20] = '\0';
        Serial.printf("        %3d   %.2f  [%s]%s\n", touch.raw() ? 1 : 0, touch.level(), bar,
                      touch.level() >= TOUCH_THRESHOLD ? " << TOUCHED" : "");
        delay(100);
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
      // No-op with a TTP223 - the module has no baseline to learn - kept as a
      // command so old muscle memory / scripts don't just error out.
      touch.calibrate();
      Serial.println("[cal] TTP223 is digital, nothing to calibrate");
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
  Serial.printf("touch: TTP223 module on GPIO%d\n", PIN_TOUCH);

  radioOk = radioLink.begin(nodeId);
  Serial.printf("radio %s\n", radioOk ? "ready" : "FAILED TO START");

  prefs.begin("lamp", false);
  engine.begin(makeSeed());
  bool restored = restoreState();
  Serial.printf("state: %s\n", restored ? "restored from flash" : "fresh (no saved state)");
  // Adopt the saved counter too, so a lamp that comes back does not look
  // "older" than its peer and get overwritten by a scene nobody chose.
  radioLink.setCounterFloor(prefs.getUInt("counter", 0));
  radioLink.setSolidFlag(engine.isSolid());
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
      if (m.type == LAMP_COLOUR) {
        Rgbw c{(uint8_t)(m.seed & 0xFF), (uint8_t)((m.seed >> 8) & 0xFF),
               (uint8_t)((m.seed >> 16) & 0xFF), (uint8_t)((m.seed >> 24) & 0xFF)};
        engine.setSolid(c);
        radioLink.setSolidFlag(true);
        scheduleSave();
        Serial.printf("[rx] colour rgbw(%u,%u,%u,%u) counter=%lu rssi=%.1f\n",
                      c.r, c.g, c.b, c.w, (unsigned long)m.counter, radioLink.lastRssi());
      } else if (m.type == LAMP_SCENE || m.type == LAMP_STATE) {
        // A STATE message is handled exactly like a SCENE. poll() only returns
        // messages that genuinely win, so a STATE that arrives because we missed
        // a tap corrects us, and one that merely repeats what we already show
        // never gets here.
        if (m.seed != engine.seed() || engine.isSolid()) {
          engine.applyScene(m.seed);
          radioLink.setSolidFlag(false);
        scheduleSave();
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

  // The link advertises our fingerprint, so it has to be recomputed whenever
  // the displayed state could have changed - cheap, and being stale here would
  // defeat the whole point of having a fingerprint.
  if (radioOk) radioLink.setCode(engine.visualCode());
  if (pendingSaveAt && millis() > pendingSaveAt) saveStateNow();

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
