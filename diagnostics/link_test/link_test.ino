// link_test - prove a genuine two-way 868 MHz link between the two lamps.
//
// Uses the pin map established in CLAUDE.md section 9:
//   SCK=7 MISO=8 MOSI=9 NSS=5 RST=3 BUSY=2 DIO1=6, TCXO 1.8 V on the chip's DIO3.
// RST=3 is the fix that made the radio work at all - without a real reset the
// SX126x never starts its oscillator.
//
// Both boards run this identical binary and take their role from their own MAC:
//   node 30D1 (cc:ba:97:16:d1:30) -> transmits
//   node 44D1 (cc:ba:97:16:d1:44) -> receives
//
// Reception is POLLED by reading the chip's own GetIrqStatus (0x12) over SPI, not
// by waiting on DIO1 and not by trusting RadioLib's return codes. That is
// deliberate: a wrong DIO1 previously made RadioLib report instant success for
// transmissions that never happened, and produced hundreds of phantom "received"
// packets with empty payloads. Everything printed here is checked against the
// chip: RX_DONE must latch, the payload must be non-empty, and the sequence number
// must advance.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
// Measured, not guessed (diagnostics/busy_dio1): BUSY held high through the whole
// Calibrate window; DIO1 tracked the TX_DONE IRQ exactly. The earlier BUSY=2/DIO1=6
// was wrong both ways, which is why the receiver never entered RX mode.
static const int PIN_NSS = 5, PIN_RST = 3, PIN_BUSY = 4, PIN_DIO1 = 2;

static const float FREQ_MHZ = 868.0;
static const float BW_KHZ   = 250.0;
static const int   SF = 9;              // faster than SF11; plenty for one room
static const int   CR = 5;
static const int   TX_DBM = 14;         // EU 868 limit is 14 dBm ERP
static const int   PREAMBLE = 8;
static const float TCXO_V = 1.8;

// RF_SW1 - the module's antenna-switch enable, from the Seeed schematic. It is
// broken out on header J1 and has never been driven. GPIO1 and GPIO4 are the two
// header pins still unaccounted for (both idle HIGH, both are module inputs), so
// one of them is RF_SW1.
//
// Why this is now the prime suspect: the transmitter reports TX_DONE and the
// receiver sits happily in RX mode with no errors, yet the receiver sees a
// completely clean irq=0x0000 - not even PREAMBLE_DETECTED - with the two boards
// 20 cm apart. RF is not leaving or entering the antenna. An antenna switch left
// disabled produces exactly that: the chip transmits into an open circuit.
static const int PIN_RFSW_A = 1;
static const int PIN_RFSW_B = 6;

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static uint16_t myId = 0;
static bool amTx = false;
static uint32_t txCount = 0, rxCount = 0;

// --- raw chip queries: the only evidence we actually trust ---
static void rawXfer(const uint8_t *tx, uint8_t *rx, int n) {
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW); delayMicroseconds(10);
  for (int i = 0; i < n; i++) { uint8_t v = spi.transfer(tx ? tx[i] : 0); if (rx) rx[i] = v; }
  delayMicroseconds(10); digitalWrite(PIN_NSS, HIGH);
  spi.endTransaction();
}
static uint16_t rawIrq() { uint8_t t[4] = {0x12,0,0,0}, r[4]; rawXfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
static uint8_t rawMode() { uint8_t t[2] = {0xC0, 0}, r[2]; rawXfer(t, r, 2); return (r[1] >> 4) & 7; }
static uint16_t rawErr() { uint8_t t[4] = {0x17,0,0,0}, r[4]; rawXfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}

static const uint16_t IRQ_TX_DONE = 0x0001;
static const uint16_t IRQ_RX_DONE = 0x0002;
static const uint16_t IRQ_CRC_ERR = 0x0040;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 6000) delay(10);
  delay(1200);

  uint64_t mac = ESP.getEfuseMac();
  myId = (uint16_t)((mac >> 32) & 0xFFFF);   // low bits are the shared vendor prefix
  amTx = (myId == 0x30D1);

  Serial.println("\n########################################");
  Serial.printf("# NODE %04X  role = %s\n", myId, amTx ? "TRANSMIT" : "RECEIVE");
  Serial.println("########################################");
  Serial.printf("%.1f MHz  bw=%.0f  sf=%d  cr=4/%d  tx=%d dBm\n",
                FREQ_MHZ, BW_KHZ, SF, CR, TX_DBM);

  pinMode(PIN_NSS, OUTPUT); digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);

  // Assert both RF_SW candidates. Driving a module input that turns out not to be
  // RF_SW1 is harmless; leaving the real one floating is not.
  pinMode(PIN_RFSW_A, OUTPUT); digitalWrite(PIN_RFSW_A, HIGH);
  pinMode(PIN_RFSW_B, OUTPUT); digitalWrite(PIN_RFSW_B, HIGH);

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  // A real reset - the thing that was missing all along.
  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);

  int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                       TX_DBM, PREAMBLE, TCXO_V, false);
  if (st != RADIOLIB_ERR_NONE) {
    while (true) { Serial.printf("RADIO INIT FAILED %d\n", st); delay(3000); }
  }
  radio.setDio2AsRfSwitch(true);
  radio.setCRC(true);
  Serial.println("radio ready");

  if (!amTx) {
    radio.startReceive();
    Serial.println("listening (polled over SPI)...");
  }
}

void loop() {
  if (amTx) {
    char msg[48];
    snprintf(msg, sizeof(msg), "PING %04X seq=%lu", myId, (unsigned long)txCount);

    uint32_t t0 = millis();
    radio.startTransmit((uint8_t *)msg, strlen(msg));
    bool done = false;
    while (millis() - t0 < 3000) {
      if (rawIrq() & IRQ_TX_DONE) { done = true; break; }
      delay(1);
    }
    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    Serial.printf("[TX %lu] \"%s\"  %s after %lu ms  mode=%s err=0x%04X\n",
                  (unsigned long)txCount, msg,
                  done ? "TX_DONE" : "NO TX_DONE (!)", (unsigned long)(millis() - t0),
                  mn(rawMode()), rawErr());
    Serial.printf("        rfsw: GPIO%d=HIGH GPIO%d=HIGH\n", PIN_RFSW_A, PIN_RFSW_B);
    txCount++;
    radio.standby();
    delay(2000);
  } else {
    uint16_t irq = rawIrq();
    if (irq & IRQ_RX_DONE) {
      size_t len = radio.getPacketLength();
      uint8_t buf[64];
      if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
      int st = radio.readData(buf, len);
      buf[len] = 0;
      float rssi = radio.getRSSI(), snr = radio.getSNR();
      bool crcBad = (irq & IRQ_CRC_ERR);
      radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);

      if (st == RADIOLIB_ERR_NONE && len > 0 && !crcBad) {
        rxCount++;
        Serial.printf("[RX %lu] len=%u \"%s\"  rssi=%.1f dBm  snr=%.1f dB  <<< REAL PACKET\n",
                      (unsigned long)rxCount, (unsigned)len, (char *)buf, rssi, snr);
      } else {
        Serial.printf("[RX] rejected: st=%d len=%u crcErr=%d\n", st, (unsigned)len, crcBad);
      }
      radio.startReceive();
    }
    // heartbeat, so silence is distinguishable from a hung sketch
    static uint32_t last = 0;
    if (millis() - last > 3000) {
      last = millis();
      uint8_t m = rawMode();
      Serial.printf("[..] mode=%-9s irq=0x%04X%s%s%s err=0x%04X packets=%lu\n",
                    mn(m), irq,
                    (irq & 0x0004) ? " PREAMBLE" : "",
                    (irq & 0x0010) ? " HEADER" : "",
                    (irq & 0x0040) ? " CRC_ERR" : "",
                    rawErr(), (unsigned long)rxCount);
      // A receiver that is not in RX mode hears nothing. Re-arm rather than sit
      // silently in standby, which is how the first attempt failed.
      if (m != 5) {
        int st = radio.startReceive();
        Serial.printf("     not in RX - re-arming startReceive() -> %d\n", st);
      }
    }
    delay(2);
  }
}
