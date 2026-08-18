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

    Adafruit_NeoPixel strip_;
    ColourEngine engine_;
    TouchSensor touch_;
    uint32_t counter_ = 0;
    uint32_t owner_ = 0;
    uint32_t code_ = 0;
    uint32_t lastState_ = 0;
    uint32_t lastFrame_ = 0;
    bool booted_ = false;
};

extern LampModule *lampModule;
