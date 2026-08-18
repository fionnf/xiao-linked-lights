// lora_pingpong - prove the two boards can talk over 868 MHz LoRa.
//
// Pin map measured by diagnostics/pinmap_hunt (RadioLib begin() returned
// ERR_NONE, so SPI / CS / RESET / BUSY are confirmed):
//
//     SCK=7(D8)  MISO=8(D9)  MOSI=9(D10)  CS=5(D4)  RST=1(D0)
//     BUSY=2(D1)  DIO1=4(D3)   TCXO on the SX1262's DIO3 @ 1.8V
//
// These are NOT the pins Meshtastic's seeed_xiao_s3 variant drives (cs=41
// rst=42 busy=40 dio1=39), which is why stock Meshtastic never found the radio.
//
// Two deliberate changes after the first run:
//
//  * Reception is POLLED over SPI rather than driven by the DIO1 interrupt.
//    begin() proves SPI and BUSY but says nothing about DIO1, and the first run
//    retriggered hundreds of times per beacon with empty payloads - the
//    signature of a wrong or stuck IRQ line. Polling the chip's own IRQ
//    register sidesteps the question; DIO1 can be pinned down later if we want
//    interrupt-driven sleep.
//
//  * GPIO3 (D2) is left as an INPUT and merely watched. It read driven-high in
//    the pin survey, which is what a button with a pull-up looks like, and there
//    is a button on the radio module. Driving it high as an output would short
//    it to ground the moment someone pressed it.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2, PIN_DIO1 = 4;
static const int PIN_UNKNOWN_D2 = 3;   // watched, never driven

static const float FREQ_MHZ   = 869.525;  // EU_868 SRD860, 10% duty cycle sub-band
static const float BW_KHZ     = 250.0;
static const int   SF         = 11;
static const int   CR         = 5;
static const uint8_t SYNCWORD = 0x2B;
static const int   TX_DBM     = 14;       // within EU 14 dBm ERP
static const int   PREAMBLE   = 8;

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static uint16_t myId = 0;
static uint32_t txCount = 0, rxCount = 0;
static uint32_t txPeriod = 5000;   // recomputed after each beacon, see loop()

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  // ESP.getEfuseMac() returns the six MAC bytes in reverse order, so the low
  // 16 bits are the vendor prefix - identical on every board, which is why both
  // units first reported themselves as BACC. The bytes that actually differ
  // between our units live in bits 32..47.
  uint64_t mac = ESP.getEfuseMac();
  myId = (uint16_t)((mac >> 32) & 0xFFFF);

  pinMode(PIN_UNKNOWN_D2, INPUT);
  randomSeed((uint32_t)mac);          // different jitter sequence per board
  txPeriod = 1000 + (myId % 5) * 700; // and a different first beacon

  Serial.println("\n########################################");
  Serial.printf("# LoRa ping-pong   NODE %04X\n", myId);
  Serial.println("########################################");
  Serial.printf("efuse mac raw = %012llX\n", (unsigned long long)mac);
  Serial.printf("freq=%.3f MHz bw=%.0f kHz sf=%d cr=4/%d tx=%d dBm\n",
                FREQ_MHZ, BW_KHZ, SF, CR, TX_DBM);

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNCWORD, TX_DBM, PREAMBLE, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) {
    while (true) { Serial.printf("RADIO INIT FAILED: %d\n", st); delay(3000); }
  }
  Serial.println("radio.begin OK");

  st = radio.setDio2AsRfSwitch(true);
  Serial.printf("setDio2AsRfSwitch -> %d\n", st);
  st = radio.setCRC(true);
  Serial.printf("setCRC -> %d\n", st);

  radio.standby();
  st = radio.startReceive();
  Serial.printf("startReceive -> %d\n", st);
  Serial.println("listening (polled)...\n");
}

void loop() {
  static uint32_t lastTx = 0;
  static uint32_t lastPinReport = 0;

  // ---- polled receive: ask the chip directly whether a packet landed ----
  uint32_t irq = radio.getIrqFlags();
  if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
    size_t len = radio.getPacketLength();
    uint8_t buf[64];
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    int st = radio.readData(buf, len);
    buf[len] = 0;
    float rssi = radio.getRSSI(), snr = radio.getSNR();
    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    if (st == RADIOLIB_ERR_NONE && len > 0) {
      rxCount++;
      Serial.printf("[RX #%lu] len=%u \"%s\"  rssi=%.1f dBm  snr=%.1f dB\n",
                    (unsigned long)rxCount, (unsigned)len, (char *)buf, rssi, snr);
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("[RX] CRC mismatch (packet corrupted in flight)");
    } else {
      Serial.printf("[RX] readData st=%d len=%u\n", st, (unsigned)len);
    }
    radio.startReceive();
  }

  // ---- beacon, staggered so the two nodes cannot talk over each other ----
  // Both boards get reset at the same instant, so a fixed 5 s period had them
  // transmitting in lockstep: each was mid-transmission exactly when the other
  // transmitted, and neither ever heard a thing (rx=0). Offset the phase by node
  // id and add jitter so they drift past one another instead.
  if (millis() - lastTx > txPeriod) {
    lastTx = millis();
    txPeriod = 4000 + (myId % 7) * 300 + (uint32_t)random(0, 1200);
    char msg[48];
    snprintf(msg, sizeof(msg), "HELLO from %04X seq=%lu", myId, (unsigned long)txCount);
    Serial.printf("[TX #%lu] \"%s\"\n", (unsigned long)txCount, msg);
    int st = radio.transmit((uint8_t *)msg, strlen(msg));
    if (st != RADIOLIB_ERR_NONE) Serial.printf("[TX] error %d\n", st);
    txCount++;
    radio.startReceive();
  }

  // ---- occasional note on the mystery pin, to identify the module button ----
  if (millis() - lastPinReport > 15000) {
    lastPinReport = millis();
    Serial.printf("[pin] GPIO3(D2)=%d   tx=%lu rx=%lu\n",
                  digitalRead(PIN_UNKNOWN_D2),
                  (unsigned long)txCount, (unsigned long)rxCount);
  }

  delay(5);
}
