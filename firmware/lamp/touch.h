#pragma once
// ============================================================
//  touch.h - capacitive touch by charge-time measurement
// ============================================================
// The same technique as touch.py on the Pico, and for the same wiring:
//
//     GPIO ──┬── copper pad / foil
//            └──[1M]── GND
//
// Drive the pin HIGH to charge the pad, release it to float, then count how
// long it takes to discharge through the 1M resistor. A finger adds
// capacitance, so a touched pad takes measurably longer.
//
// Why not the ESP32-S3's hardware touch peripheral: it only covers GPIO1-14,
// and every one of those is taken by the LoRa module on this board. So this is
// not a fallback - it is the only method available, and it is already proven
// against this exact pad and resistor.

#include <Arduino.h>
#include "config.h"

class TouchSensor {
 public:
  void begin(int pin) {
    pin_ = pin;
    baseline_ = measureAvg(32);
    lastCal_ = millis();
  }

  // Re-learn the resting value. Capacitance drifts with temperature and
  // humidity, so a baseline captured once at boot slowly stops being true.
  void calibrate() { baseline_ = measureAvg(16); }

  enum Event { NONE, TAP, HOLD };

  Event update() {
    uint32_t now = millis();
    float v = measureAvg(4);
    bool touched = v > baseline_ * TOUCH_THRESHOLD;

    Event ev = NONE;
    if (touched && !wasTouched_) {
      touchStart_ = now;
      holdFired_ = false;
    } else if (touched && wasTouched_) {
      if (!holdFired_ && now - touchStart_ >= HOLD_TIME_MS) {
        holdFired_ = true;
        ev = HOLD;
      }
    } else if (!touched && wasTouched_) {
      // A release only counts as a tap if it was not already reported as a
      // hold, otherwise one long press would fire both events.
      if (!holdFired_ && now - touchStart_ > 20) ev = TAP;
    }
    wasTouched_ = touched;

    // Drift-track the baseline only while untouched, and only slowly, so a
    // finger resting on the pad can never be absorbed into "normal".
    if (!touched && now - lastCal_ > 2000) {
      lastCal_ = now;
      baseline_ = baseline_ * 0.95f + v * 0.05f;
    }
    return ev;
  }

  float baseline() const { return baseline_; }
  float last() const { return last_; }

 private:
  uint32_t measureRaw() {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, HIGH);
    delayMicroseconds(20);          // charge the pad
    pinMode(pin_, INPUT);           // release and let it discharge through 1M
    uint32_t count = 0;
    while (digitalRead(pin_) == HIGH) {
      if (++count > 20000) break;   // safety cap: never spin forever
    }
    return count;
  }

  float measureAvg(int samples) {
    uint32_t total = 0;
    for (int i = 0; i < samples; i++) total += measureRaw();
    last_ = (float)total / samples;
    return last_;
  }

  int pin_ = -1;
  float baseline_ = 0;
  float last_ = 0;
  bool wasTouched_ = false;
  bool holdFired_ = false;
  uint32_t touchStart_ = 0;
  uint32_t lastCal_ = 0;
};
