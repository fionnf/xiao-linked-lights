#pragma once
// ============================================================
//  link.h - lamp-to-lamp radio link over raw LoRa
// ============================================================
// Meshtastic is NOT used for colour. Measured on this hardware, a colour packet
// took 7-20 s to arrive (mean 15 s), and even Meshtastic's fastest legal preset
// only reached ~11 s while dropping packets. That is its transmit scheduling,
// not airtime - the raw radio completes a transmission in 91 ms. A lamp you tap
// and wait fifteen seconds for is broken, so colour rides on RadioLib directly.
// The Meshtastic build still exists as a separate firmware for the phone app.
//
// Wire format, 15 bytes:
//     0       magic 0xC1
//     1       type: 1 = SCENE, 2 = POWER, 3 = STATE
//     2..5    counter, uint32 little-endian
//     6..9    seed,    uint32 little-endian
//     10..13  node id, uint32 little-endian
//     14..17  visual code, uint32 little-endian - a fingerprint of exactly what
//             the sender's strip is showing (mode + colour/seed + band layout)
//     18      flags (bit0 = powered on)
//
// The node id is carried explicitly rather than derived from the payload. It is
// the tiebreak when two lamps claim the same counter, and a tiebreak has to be
// stable and unique - an earlier version folded seed and counter together, which
// is neither.
//
// Only a seed travels, not the scene: both lamps regenerate identical group
// sizes, hues and white levels from it (see colour.h).
//
// Conflict rule: last-write-wins on a Lamport counter, ties broken by the
// higher node id. Two lamps tapped in the same instant must converge on ONE
// scene rather than ping-ponging between two.
//
// STATE messages are what make the lamps SELF-HEALING. A tap is sent once, with
// no acknowledgement and no retry, so a single lost packet would otherwise leave
// the lamps showing different colours indefinitely - and LoRa does lose packets.
// Every lamp therefore announces its current (counter, seed) periodically. A lamp
// hearing a HIGHER counter adopts that scene; a lamp hearing a LOWER one answers
// immediately with its own state, so the stale lamp catches up within a second
// instead of waiting for the next announcement. Convergence does not depend on
// any single packet arriving.

#include <Arduino.h>
#include <RadioLib.h>
#include "config.h"

static const uint8_t LAMP_MAGIC = 0xC1;
static const uint8_t LAMP_SCENE = 1;
static const uint8_t LAMP_POWER = 2;
static const uint8_t LAMP_STATE = 3;
// A colour picked by a human. The four seed bytes carry R,G,B,W instead of a
// seed - a scene is generated FROM a seed, but a chosen colour IS the payload,
// and RGBW happens to be exactly four bytes.
static const uint8_t LAMP_COLOUR = 4;
static const int LAMP_PACKET_LEN = 19;

// How often a lamp announces its state. At SF9/BW250 this packet is ~60 ms on
// air, so one every 20 s is ~0.3% duty cycle - comfortably inside the 1% limit
// for the 868.0-868.6 MHz band.
static const uint32_t STATE_INTERVAL_MS = 15000;

struct LampMsg {
  uint8_t type;
  uint32_t counter;
  uint32_t seed;
  uint32_t nodeId;
  uint32_t code;      // sender's visual fingerprint
  uint8_t flags;
};

class LampLink {
 public:
  bool begin(uint32_t nodeId) {
    nodeId_ = nodeId;
    spi_.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, -1);

    // A real hardware reset before anything else. An SX126x that has not been
    // reset answers SPI reads and executes register writes while silently
    // refusing to start its oscillator - which is exactly how this board
    // presented as "working but never transmitting" for a long time.
    pinMode(PIN_LORA_RST, OUTPUT);
    digitalWrite(PIN_LORA_RST, LOW);
    delay(5);
    digitalWrite(PIN_LORA_RST, HIGH);
    delay(50);

    mod_ = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, spi_);
    radio_ = new SX1262(mod_);
    int st = radio_->begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                           RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_DBM,
                           LORA_PREAMBLE, LORA_TCXO_V, false);
    if (st != RADIOLIB_ERR_NONE) return false;
    radio_->setDio2AsRfSwitch(true);
    radio_->setCRC(true);
    radio_->startReceive();
    return true;
  }

  uint32_t counter() const { return counter_; }
  uint32_t nodeId() const { return nodeId_; }

  // Claim a new scene locally and put it on air.
  void setCode(uint32_t c) { code_ = c; }

  void broadcastScene(uint32_t seed) {
    counter_++;
    owner_ = nodeId_;
    seed_ = seed;
    send(LAMP_SCENE, counter_, seed, 0);
    lastState_ = millis();
  }

  // Re-announce what we are currently showing. Costs one short packet and is
  // what lets a lamp that missed a tap - or that just booted - catch up.
  void announceState() {
    send(solid_ ? LAMP_COLOUR : LAMP_STATE, counter_, seed_, poweredOn_ ? 1 : 0);
    lastState_ = millis();
  }

  // Call every loop; announces on a jittered interval. The jitter matters:
  // without it two lamps that booted together would announce in lockstep and
  // could collide on air every single time.
  void tick() {
    uint32_t now = millis();
    if (now - lastState_ > STATE_INTERVAL_MS + (nodeId_ % 3000)) announceState();
  }

  void setPowered(bool on) { poweredOn_ = on; }
  void setSeed(uint32_t s) { seed_ = s; }

  void broadcastColour(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    counter_++;
    owner_ = nodeId_;
    solid_ = true;
    seed_ = ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)w << 24);
    send(LAMP_COLOUR, counter_, seed_, poweredOn_ ? 1 : 0);
    lastState_ = millis();
  }

  // State announcements must say WHICH kind of thing we are showing, or a lamp
  // in picked-colour mode would re-announce it as a seed and the other end would
  // generate a random scene from four colour bytes.
  void setSolidFlag(bool s) { solid_ = s; }

  void broadcastPower(bool on) {
    counter_++;
    owner_ = nodeId_;
    poweredOn_ = on;
    send(LAMP_POWER, counter_, seed_, on ? 1 : 0);
  }

  // Poll for an inbound message. Returns true and fills `out` when a packet
  // arrives that genuinely wins against what we already have.
  //
  // The IRQ register is read over SPI rather than waiting on the DIO1 pin.
  // That is deliberate: an earlier build trusted RadioLib's blocking calls
  // while DIO1 was misconfigured, and every call returned success instantly
  // for transmissions that never happened.
  bool poll(LampMsg &out) {
    uint16_t irq = radio_->getIrqFlags();
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) return false;

    uint8_t buf[32];
    size_t len = radio_->getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    int st = radio_->readData(buf, len);
    lastRssi_ = radio_->getRSSI();
    lastSnr_ = radio_->getSNR();
    bool crcBad = (irq & RADIOLIB_SX126X_IRQ_CRC_ERR);
    radio_->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    radio_->startReceive();

    if (st != RADIOLIB_ERR_NONE || crcBad) return false;
    if (len != LAMP_PACKET_LEN || buf[0] != LAMP_MAGIC) return false;

    LampMsg m;
    m.type = buf[1];
    memcpy(&m.counter, buf + 2, 4);
    memcpy(&m.seed, buf + 6, 4);
    memcpy(&m.nodeId, buf + 10, 4);
    memcpy(&m.code, buf + 14, 4);
    m.flags = buf[18];
    if (m.nodeId == nodeId_) return false;      // ignore our own echo

    // If the fingerprints already match we are showing the same thing, whatever
    // the counters say. Nothing to do, and nothing to argue about.
    if (m.code == code_ && m.counter <= counter_) return false;

    // Compare BEFORE advancing our own counter, or we would be comparing the
    // remote counter against a copy of itself and every message would tie.
    //
    // The extra clause matters: equal counters with DIFFERENT codes means the
    // lamps genuinely disagree about what they are showing. Counters alone
    // cannot break that - both sides think they are current - so fall back to
    // the node id, which is stable and unique, and let the higher one win.
    bool wins = m.counter > counter_ ||
                (m.counter == counter_ && m.code != code_ && m.nodeId > owner_);

    if (!wins) {
      // We are ahead of them. Say so straight away rather than waiting for the
      // next scheduled announcement - this is what makes a lamp that missed a
      // tap converge in about a second.
      // We are ahead, or we win the tie. Either way the other lamp is showing
      // something different, so tell it what we have rather than waiting.
      if (m.code != code_) announceState();
      return false;
    }

    counter_ = m.counter;
    owner_ = m.nodeId;
    seed_ = m.seed;
    out = m;
    return true;
  }

  float lastRssi() const { return lastRssi_; }
  float lastSnr() const { return lastSnr_; }

 private:
  void send(uint8_t type, uint32_t counter, uint32_t seed, uint8_t flags) {
    uint8_t buf[LAMP_PACKET_LEN];
    buf[0] = LAMP_MAGIC;
    buf[1] = type;
    memcpy(buf + 2, &counter, 4);
    memcpy(buf + 6, &seed, 4);
    memcpy(buf + 10, &nodeId_, 4);
    memcpy(buf + 14, &code_, 4);
    buf[18] = flags;
    radio_->transmit(buf, LAMP_PACKET_LEN);   // ~91 ms at SF9/BW250
    radio_->startReceive();
  }

  SPIClass spi_{FSPI};
  Module *mod_ = nullptr;
  SX1262 *radio_ = nullptr;
  uint32_t nodeId_ = 0;
  uint32_t counter_ = 0;
  uint32_t owner_ = 0;
  uint32_t seed_ = 1;
  uint32_t code_ = 0;
  uint32_t lastState_ = 0;
  bool poweredOn_ = true;
  bool solid_ = false;
  float lastRssi_ = 0, lastSnr_ = 0;
};
