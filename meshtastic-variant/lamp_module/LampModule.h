#pragma once
// ============================================================
//  LampModule - the linked lamps, running INSIDE Meshtastic
// ============================================================
// The standalone firmware (firmware/lamp/) talks to the SX1262 directly and
// syncs in ~211 ms, which is what a lamp you tap should feel like. Its one
// limitation is range: the two lamps must hear each other directly.
//
// This module is the answer to that. By living inside Meshtastic, a lamp becomes
// a real mesh node, so its colour packets can be relayed by any other Meshtastic
// node in between - which is the only way to reach a lamp that is out of direct
// range. The cost is latency: measured on this hardware, Meshtastic delivers a
// packet in 7-20 s because of its transmit scheduling, not its airtime.
//
// So the two builds are a deliberate trade, not a duplicate:
//     firmware/lamp/   fast, direct, same room or same building
//     this module      slow, relayed, works at mesh range
//
// Wire format is identical to the standalone firmware, so the two speak the same
// language and the protocol only had to be written once.

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include <Adafruit_NeoPixel.h>

#include "LampColour.h"
#include "LampTouch.h"

class LampModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    LampModule();

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual int32_t runOnce() override;

  private:
    void sendState(uint8_t type);
    void localTap();
    void startupChain();
    void render();

    // NOTE: do not add anything here that calls Serial.read()/available().
    // Meshtastic's own framed USB-serial API (what `meshtastic --info` and
    // the desktop/phone-over-USB clients use) shares this same UART, and it
    // has exactly one reader. A module stealing bytes off it - even ones it
    // doesn't understand and drops - starves that reader and breaks the CLI/
    // phone connection. An earlier version of this module did exactly that
    // to offer a typed serial console; it broke `meshtastic --info` on both
    // boards and was reverted. Writing to Serial (LOG_INFO, Serial.printf)
    // is fine and does not conflict - only reading does. If a console is
    // ever needed again inside Meshtastic, it has to go through Meshtastic's
    // own serial API/module, not a second consumer of the same bytes.
    void debugTouch();

    // Sunrise alarm - deliberately local-only (see LampModule.cpp): renders
    // straight to the strip, bypassing engine_/counter_/sendState entirely,
    // so one lamp's alarm never wakes the other one.
    void checkAlarm();
    void renderAlarmRamp(float progress);

    Adafruit_NeoPixel strip_;
    ColourEngine engine_;
    TouchSensor touch_;
    uint32_t counter_ = 0;
    uint32_t owner_ = 0;
    uint32_t code_ = 0;
    uint32_t lastState_ = 0;
    uint32_t lastFrame_ = 0;
    bool booted_ = false;
    bool touchWasTouched_ = false;
    uint32_t lastTouchLog_ = 0;

    bool alarmEnabled_ = false;
    uint8_t alarmHour_ = 7;
    uint8_t alarmMinute_ = 0;
    uint16_t alarmDurationMin_ = 10;
    bool alarmActive_ = false;
    uint32_t alarmStartMs_ = 0;
    int32_t alarmLastFiredKey_ = -1;  // (year<<9)|yday of the last day it fired
    uint32_t lastAlarmFrame_ = 0;
};

extern LampModule *lampModule;
