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
// Wire format, 11 bytes:
//     0      magic 0xC1
//     1      type: 1 = SCENE, 2 = POWER
//     2..5   counter, uint32 little-endian
//     6..9   seed,    uint32 little-endian
//     10     flags (bit0 = powered on, for POWER messages)
//
// Only a seed travels, not the scene: both lamps regenerate identical group
// sizes, hues and white levels from it (see colour.h).
//
// Conflict rule: last-write-wins on a Lamport counter, ties broken by the
// higher node id. Two lamps tapped in the same instant must converge on ONE
// scene rather than ping-ponging between two.

#include <Arduino.h>
#include <RadioLib.h>
#include "config.h"

static const uint8_t LAMP_MAGIC = 0xC1;
static const uint8_t LAMP_SCENE = 1;
static const uint8_t LAMP_POWER = 2;
static const int LAMP_PACKET_LEN = 11;

struct LampMsg {
  uint8_t type;
  uint32_t counter;
  uint32_t seed;
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
  void broadcastScene(uint32_t seed) {
    counter_++;
    owner_ = nodeId_;
    send(LAMP_SCENE, counter_, seed, 0);
  }

  void broadcastPower(bool on) {
    counter_++;
    owner_ = nodeId_;
    send(LAMP_POWER, counter_, 0, on ? 1 : 0);
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
    m.flags = buf[10];

    // Compare BEFORE advancing our own counter, or we would be comparing the
    // remote counter against a copy of itself and every message would tie.
    uint32_t src = m.seed ^ m.counter;   // stable tiebreak that needs no extra bytes
    bool wins = m.counter > counter_ || (m.counter == counter_ && src > owner_);
    if (m.counter > counter_) counter_ = m.counter;
    if (!wins) return false;
    owner_ = src;
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
    buf[10] = flags;
    radio_->transmit(buf, LAMP_PACKET_LEN);   // ~91 ms at SF9/BW250
    radio_->startReceive();
  }

  SPIClass spi_{FSPI};
  Module *mod_ = nullptr;
  SX1262 *radio_ = nullptr;
  uint32_t nodeId_ = 0;
  uint32_t counter_ = 0;
  uint32_t owner_ = 0;
  float lastRssi_ = 0, lastSnr_ = 0;
};
