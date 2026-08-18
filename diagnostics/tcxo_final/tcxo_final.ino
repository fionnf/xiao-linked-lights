// tcxo_final - find the TCXO setting, now that reset and TX entry both work.
//
// Progress that makes this test meaningful at last:
//   * RST is GPIO3. With a real reset, register 0x0740 returns to 0x1424.
//   * With a real reset, the chip ENTERS TX MODE - the first time that has ever
//     happened. So the command path and PA config are fine.
//   * And crucially, a bare reset now reports device error 0x0020 = bit 5 =
//     XOSC_START_ERR. That error bit was invisible during the whole earlier
//     investigation precisely because the chip was never being reset.
//
// So the remaining fault is the crystal/TCXO: TX is entered but never completes,
// because the PLL has no good reference. An earlier TCXO sweep was worthless -
// it ran without a working reset - so it must be redone from a clean state.
//
// For each candidate voltage: real reset, configure, transmit, and report whether
// XOSC_START_ERR clears and whether TX_DONE latches. Also watch GPIO2 and GPIO6
// during transmission to settle which one is BUSY (it must be HIGH while the chip
// is busy transmitting).

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_NSS = 5, PIN_RST = 3;
static const int PIN_A = 2, PIN_B = 6;      // BUSY / DIO1, order unknown

static SPIClass spi(FSPI);
static String LOG;
static void LOGF(const char *fmt, ...) {
  char b[240]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  LOG += b; Serial.print(b);
}

static void rawXfer(const uint8_t *tx, uint8_t *rx, int n) {
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW); delayMicroseconds(10);
  for (int i = 0; i < n; i++) { uint8_t v = spi.transfer(tx ? tx[i] : 0); if (rx) rx[i] = v; }
  delayMicroseconds(10); digitalWrite(PIN_NSS, HIGH);
  spi.endTransaction();
}
static uint8_t rawMode() { uint8_t t[2] = {0xC0, 0}, r[2]; rawXfer(t, r, 2); return (r[1] >> 4) & 7; }
static uint16_t rawIrq() { uint8_t t[4] = {0x12,0,0,0}, r[4]; rawXfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
static uint16_t rawErr() { uint8_t t[4] = {0x17,0,0,0}, r[4]; rawXfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}

static void hardReset() {
  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);
}

static bool tryVoltage(float v, int busyPin, int dio1Pin) {
  hardReset();
  uint16_t errAfterReset = rawErr();

  Module *mod = new Module(PIN_NSS, dio1Pin, PIN_RST, busyPin, spi);
  SX1262 radio(mod);
  int st = radio.begin(868.0, 250.0, 11, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, v, false);
  if (st != RADIOLIB_ERR_NONE) {
    LOGF("  tcxo %.1fV  begin FAILED %d\n", v, st);
    return false;
  }
  radio.setDio2AsRfSwitch(true);
  radio.setCRC(true);

  uint16_t errAfterInit = rawErr();

  // Does the oscillator reach STBY_XOSC now?
  radio.standby(RADIOLIB_SX126X_STANDBY_XOSC);
  delay(30);
  bool xosc = (rawMode() == 3);

  radio.startTransmit("TCXOFINAL");

  bool sawTx = false, sawDone = false;
  int aHigh = 0, bHigh = 0, samples = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    uint8_t m = rawMode();
    if (m == 6) {
      sawTx = true;
      if (digitalRead(PIN_A)) aHigh++;
      if (digitalRead(PIN_B)) bHigh++;
      samples++;
    }
    if (rawIrq() & 0x0001) { sawDone = true; break; }
    delay(1);
  }
  uint32_t took = millis() - t0;

  LOGF("  tcxo %.1fV  errReset=0x%04X errInit=0x%04X XOSC=%-3s TX=%-3s DONE=%-3s %4lums",
       v, errAfterReset, errAfterInit, xosc ? "YES" : "no",
       sawTx ? "YES" : "no", sawDone ? "YES" : "no", (unsigned long)took);
  if (samples) LOGF("  [during TX: GPIO2 high %d%%, GPIO6 high %d%%]",
                    (aHigh * 100) / samples, (bHigh * 100) / samples);
  LOGF("\n");
  return sawDone;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# TCXO sweep with working reset (GPIO3)\n");
  LOGF("########################################\n");
  LOGF("error bit 5 (0x0020) = XOSC_START_ERR\n\n");

  pinMode(PIN_NSS, OUTPUT); digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_A, INPUT);
  pinMode(PIN_B, INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  const float VOLTS[] = {1.8, 3.3, 3.0, 2.7, 2.4, 2.2, 1.7, 1.6, 0.0};
  bool win = false;
  float winner = -1;
  for (unsigned i = 0; i < sizeof(VOLTS) / sizeof(float) && !win; i++) {
    if (tryVoltage(VOLTS[i], PIN_A, PIN_B)) { win = true; winner = VOLTS[i]; }
  }

  if (!win) {
    LOGF("\n(retrying with busy/dio1 swapped)\n");
    for (unsigned i = 0; i < sizeof(VOLTS) / sizeof(float) && !win; i++) {
      if (tryVoltage(VOLTS[i], PIN_B, PIN_A)) { win = true; winner = VOLTS[i]; }
    }
  }

  LOGF("\n=== RESULT ===\n");
  if (win) LOGF("  *** TRANSMISSION COMPLETED with TCXO = %.1f V ***\n", winner);
  else     LOGF("  No voltage completed a transmission. TX is entered but never finishes.\n");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
