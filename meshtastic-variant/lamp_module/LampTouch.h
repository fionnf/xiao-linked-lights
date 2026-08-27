#pragma once
// ============================================================
//  LampTouch.h - TTP223 digital touch module
// ============================================================
// Wiring:
//
//     TTP223 VCC -> 3V3
//     TTP223 GND -> GND
//     TTP223 OUT -> PIN_TOUCH
//
// The TTP223 does its own capacitance sensing on-board and just presents a
// clean digital level on OUT (HIGH while touched, by default - some boards
// have a solder jumper for active-low instead; flip TOUCH_ACTIVE_HIGH in
// config.h to match). No discharge resistor, no charge-time measurement.
//
// Sensitivity itself is set on the module (trimmer or solder jumper, if it
// has one) and is NOT adjustable from here - once the TTP223 says "touched"
// there is no analog level left for firmware to threshold against.
//
// So the "raw data" and "threshold" this file provides are built, not read:
// level() is an exponential moving average of recent raw samples, so an
// oversensitive/chattery line still produces a real number between 0 (quiet)
// and 1 (solidly touched) rather than just an instantaneous 0/1 blip, and
// TOUCH_THRESHOLD is a genuine, tunable line drawn in that number - the same
// role TOUCH_THRESHOLD played for the old charge-time pad. A cooldown after
// each accepted tap catches the case a smoothed signal still crosses the
// threshold repeatedly (a hand held near the module, not a deliberate tap).

#include <Arduino.h>
#include "LampConfig.h"

class TouchSensor {
 public:
  void begin(int pin) {
    pin_ = pin;
    pinMode(pin_, INPUT);
  }

  // Kept for interface compatibility with the rest of the firmware (status
  // printout, serial "cal" command); there is no baseline to (re)learn here,
  // just reset the smoothing so a noisy period doesn't linger.
  void calibrate() { level_ = 0.0f; }

  enum Event { NONE, TAP, HOLD };

  Event update() {
    uint32_t now = millis();
    bool raw = digitalRead(pin_) == HIGH;
    bool rawTouched = TOUCH_ACTIVE_HIGH ? raw : !raw;
    raw_ = rawTouched;
    level_ += TOUCH_SMOOTHING * ((rawTouched ? 1.0f : 0.0f) - level_);
    bool touched = level_ >= TOUCH_THRESHOLD;

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
      // hold (one long press should not fire both events), and we're not
      // still inside the cooldown from the previous accepted tap.
      bool cooledDown = (now - lastTapAt_) >= TAP_COOLDOWN_MS;
      if (!holdFired_ && cooledDown) {
        ev = TAP;
        lastTapAt_ = now;
      }
    }
    wasTouched_ = touched;
    return ev;
  }

  // The raw instantaneous pin reading, before any smoothing - for
  // touchmon/status so wiring can be verified against the sensor directly.
  bool raw() const { return raw_; }
  // Smoothed 0..1 level; compare against TOUCH_THRESHOLD by eye while tuning.
  float level() const { return level_; }

  // Kept so older status/touchmon call sites still compile.
  float baseline() const { return 0.0f; }
  float last() const { return level_; }

 private:
  int pin_ = -1;
  bool raw_ = false;
  float level_ = 0;
  bool wasTouched_ = false;
  bool holdFired_ = false;
  uint32_t touchStart_ = 0;
  uint32_t lastTapAt_ = 0xFFFF0000;  // far enough in the past that boot isn't in cooldown
};
