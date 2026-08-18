// lora_roles - settle, unambiguously, whether these two radios can talk.
//
// Why this exists: an interrupt-driven build appeared to receive hundreds of
// packets at RSSI -40 dBm, but every one had an EMPTY payload and byte-identical
// RSSI/SNR. That is what stale registers look like, not radio traffic. A polled
// build then received nothing at all. So reception is NOT yet proven and must be
// tested without any room for wishful reading.
//
// Design: one board only transmits, the other only receives - no role switching,
// no shared timers, no interrupts. The role comes from the board's own MAC, so
// both units run this identical binary.
//
//   node 30D1 (MAC cc:ba:97:16:d1:30) -> TRANSMITTER
//   node 44D1 (MAC cc:ba:97:16:d1:44) -> RECEIVER
//
// The receiver uses RadioLib's blocking receive(), the simplest and least
// error-prone path, and prints the outcome of EVERY attempt including timeouts.
// A timeout printed once per second is meaningful evidence; silence is not.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2, PIN_DIO1 = 4;

static const float FREQ_MHZ   = 869.525;
static const float BW_KHZ     = 250.0;
static const int   SF         = 11;
static const int   CR         = 5;
static const uint8_t SYNCWORD = 0x2B;
static const int   TX_DBM     = 14;
static const int   PREAMBLE   = 8;

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static uint16_t myId = 0;
static bool amTransmitter = false;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  uint64_t mac = ESP.getEfuseMac();
  myId = (uint16_t)((mac >> 32) & 0xFFFF);
  amTransmitter = (myId == 0x30D1);

  Serial.println("\n########################################");
  Serial.printf("# NODE %04X  role=%s\n", myId, amTransmitter ? "TRANSMITTER" : "RECEIVER");
  Serial.println("########################################");

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNCWORD, TX_DBM, PREAMBLE, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) {
    while (true) { Serial.printf("RADIO INIT FAILED %d\n", st); delay(3000); }
  }
  Serial.printf("begin OK  dio2rfsw=%d crc=%d\n",
                radio.setDio2AsRfSwitch(true), radio.setCRC(true));
  Serial.println(amTransmitter ? "transmitting every 3 s..." : "receiving (blocking)...");
}

void loop() {
  if (amTransmitter) {
    static uint32_t seq = 0;
    char msg[48];
    snprintf(msg, sizeof(msg), "PING %04X #%lu", myId, (unsigned long)seq);
    uint32_t t0 = millis();
    int st = radio.transmit((uint8_t *)msg, strlen(msg));
    Serial.printf("[TX %lu] \"%s\" st=%d (%lu ms on air)\n",
                  (unsigned long)seq, msg, st, (unsigned long)(millis() - t0));
    seq++;
    delay(3000);
  } else {
    uint8_t buf[64];
    // Blocking receive. RadioLib derives its own timeout from the symbol rate,
    // so this returns on its own every second or so. Printing the timeout case
    // matters: it proves the receiver is alive and actually listening.
    int st = radio.receive(buf, sizeof(buf) - 1);
    if (st == RADIOLIB_ERR_NONE) {
      size_t len = radio.getPacketLength();
      if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
      buf[len] = 0;
      Serial.printf("[RX] len=%u \"%s\"  rssi=%.1f  snr=%.1f  <<< REAL PACKET\n",
                    (unsigned)len, (char *)buf, radio.getRSSI(), radio.getSNR());
    } else if (st == RADIOLIB_ERR_RX_TIMEOUT) {
      Serial.println("[RX] .. timeout (listening, nothing heard)");
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.printf("[RX] CRC MISMATCH - a signal WAS heard, rssi=%.1f\n", radio.getRSSI());
    } else {
      Serial.printf("[RX] error %d\n", st);
    }
  }
}
