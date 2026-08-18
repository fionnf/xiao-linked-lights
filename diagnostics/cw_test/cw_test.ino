// cw_test - is RF actually leaving the antenna? Continuous wave vs RSSI.
//
// State of play: the transmitter reports TX_DONE and the receiver sits in RX mode
// with no errors, yet the receiver's IRQ register is perfectly clean - not even
// PREAMBLE_DETECTED - with the boards 20 cm apart. Either no RF is being radiated,
// or the two ends disagree about packet format.
//
// This test removes every packet-level variable. The transmitter emits an
// unmodulated carrier with SetTxContinuousWave (0xD1); the receiver simply reads
// GetRssiInst (0x15) in RX mode. Sync word, spreading factor, coding rate, CRC and
// payload length become irrelevant - the only question left is whether energy at
// 868 MHz reaches the other antenna.
//
//   RSSI jumps when the carrier is on  -> RF works; the fault is packet config.
//   RSSI stays at the noise floor      -> no RF; antenna / RF_SW / PA problem.
//
// The transmitter keys the carrier on for 6 s and off for 6 s, so the receiver's
// reading can be correlated rather than guessed at.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_NSS = 5, PIN_RST = 3, PIN_BUSY = 2, PIN_DIO1 = 6;
static const int PIN_RFSW_A = 1, PIN_RFSW_B = 4;

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static uint16_t myId = 0;
static bool amTx = false;

static void rawXfer(const uint8_t *tx, uint8_t *rx, int n) {
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW); delayMicroseconds(10);
  for (int i = 0; i < n; i++) { uint8_t v = spi.transfer(tx ? tx[i] : 0); if (rx) rx[i] = v; }
  delayMicroseconds(10); digitalWrite(PIN_NSS, HIGH);
  spi.endTransaction();
}
static uint8_t rawMode() { uint8_t t[2] = {0xC0, 0}, r[2]; rawXfer(t, r, 2); return (r[1] >> 4) & 7; }
static uint16_t rawErr() { uint8_t t[4] = {0x17,0,0,0}, r[4]; rawXfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
// GetRssiInst: opcode 0x15 -> status, rssi. dBm = -rssi/2
static float rawRssi() { uint8_t t[3] = {0x15,0,0}, r[3]; rawXfer(t, r, 3); return -((float)r[2]) / 2.0f; }
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 6000) delay(10);
  delay(1200);

  uint64_t mac = ESP.getEfuseMac();
  myId = (uint16_t)((mac >> 32) & 0xFFFF);
  amTx = (myId == 0x30D1);

  Serial.println("\n########################################");
  Serial.printf("# CW test  NODE %04X  role=%s\n", myId, amTx ? "CARRIER" : "RSSI METER");
  Serial.println("########################################");

  pinMode(PIN_NSS, OUTPUT); digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_RFSW_A, OUTPUT); digitalWrite(PIN_RFSW_A, HIGH);
  pinMode(PIN_RFSW_B, OUTPUT); digitalWrite(PIN_RFSW_B, HIGH);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);

  int st = radio.begin(868.0, 250.0, 9, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 1.8, false);
  if (st != RADIOLIB_ERR_NONE) { while (true) { Serial.printf("INIT FAILED %d\n", st); delay(3000); } }
  radio.setDio2AsRfSwitch(true);
  Serial.printf("radio ready (mode=%s err=0x%04X)\n", mn(rawMode()), rawErr());

  if (!amTx) {
    radio.startReceive();
    Serial.println("measuring RSSI in RX mode; noise floor should be near -110 dBm");
  }
}

void loop() {
  if (amTx) {
    // Carrier ON for 6 s. RadioLib exposes this as transmitDirect() on SX126x,
    // which issues SetTxContinuousWave.
    Serial.println("\n>>> CARRIER ON  (6 s)");
    int st = radio.transmitDirect();
    Serial.printf("    transmitDirect -> %d   mode=%s err=0x%04X\n", st, mn(rawMode()), rawErr());
    uint32_t t0 = millis();
    while (millis() - t0 < 6000) {
      if (millis() % 1500 < 20) Serial.printf("    ...on, mode=%s\n", mn(rawMode()));
      delay(20);
    }

    Serial.println(">>> CARRIER OFF (6 s)");
    radio.standby();
    Serial.printf("    mode=%s\n", mn(rawMode()));
    delay(6000);
  } else {
    // Track the range of RSSI so a carrier appearing is unmistakable.
    static float lo = 999, hi = -999;
    static uint32_t last = 0, windowStart = 0;
    if (windowStart == 0) windowStart = millis();

    float r = rawRssi();
    if (r < lo) lo = r;
    if (r > hi) hi = r;

    if (millis() - last > 2000) {
      last = millis();
      Serial.printf("[rssi] now=%.1f  window min=%.1f max=%.1f  mode=%s%s\n",
                    r, lo, hi, mn(rawMode()),
                    (hi - lo > 15.0) ? "   <<< CARRIER DETECTED" : "");
      if (millis() - windowStart > 14000) { lo = 999; hi = -999; windowStart = millis(); }
    }
    if (rawMode() != 5) radio.startReceive();
    delay(5);
  }
}
