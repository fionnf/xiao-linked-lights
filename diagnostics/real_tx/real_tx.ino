// real_tx - transmit for real, now that the reset line is known.
//
// THE decisive discovery: RST is GPIO3 (D2), not GPIO1. Pulsing GPIO3 low restored
// register 0x0740 to 14 24 - the documented SX126x default sync word 0x1424 -
// which only a genuine chip reset can do.
//
// This means the chip had never once been reset during the whole investigation:
// every "hardware reset" pulsed GPIO1, which is not connected to reset. An
// SX126x that has never been reset sits in an undefined state, and that alone
// explains the central mystery - it accepted commands, reported no errors, and
// still refused to leave STBY_RC.
//
// Confirmed pin map so far:
//     SCK=7(D8)  MISO=8(D9)  MOSI=9(D10)  NSS=5(D4)  RST=3(D2)
// Still to place: BUSY and DIO1, which idle LOW, so they are among GPIO2 and
// GPIO6. GPIO1 and GPIO4 idle HIGH and are inputs to the module - one of them is
// RF_SW1, the antenna-switch enable that nothing has ever driven.
//
// Try each remaining arrangement, with and without RF_SW1 asserted, and demand
// real evidence of transmission: the chip must be seen in TX mode AND the TX_DONE
// IRQ must latch, read straight from the chip rather than trusting a library.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_NSS = 5, PIN_RST = 3;

static SPIClass spi(FSPI);
static String LOG;
static void LOGF(const char *fmt, ...) {
  char b[240]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  LOG += b; Serial.print(b);
}

// --- raw helpers, so success is never taken on trust ---
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

static bool attempt(int busyPin, int dio1Pin, int rfswPin, bool useSX1261) {
  char label[80];
  snprintf(label, sizeof(label), "busy=%d dio1=%d rfsw=%s %s",
           busyPin, dio1Pin, rfswPin < 0 ? "none" : String(rfswPin).c_str(),
           useSX1261 ? "SX1261" : "SX1262");

  // Drive the antenna-switch enable, if we are testing one this round.
  if (rfswPin >= 0) { pinMode(rfswPin, OUTPUT); digitalWrite(rfswPin, HIGH); }

  Module *mod = new Module(PIN_NSS, dio1Pin, PIN_RST, busyPin, spi);
  int st;
  if (useSX1261) { SX1261 r(mod); st = r.begin(868.0, 250.0, 11, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 1.8, false);
                   if (st == 0) { r.setDio2AsRfSwitch(true); r.setCRC(true); r.startTransmit("REALTX"); } }
  else           { SX1262 r(mod); st = r.begin(868.0, 250.0, 11, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 22, 8, 1.8, false);
                   if (st == 0) { r.setDio2AsRfSwitch(true); r.setCRC(true); r.startTransmit("REALTX"); } }

  if (st != RADIOLIB_ERR_NONE) {
    LOGF("  %-42s begin FAILED %d\n", label, st);
    if (rfswPin >= 0) pinMode(rfswPin, INPUT);
    return false;
  }

  // Watch the chip itself, not the library.
  bool sawTx = false, sawDone = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    if (rawMode() == 6) sawTx = true;
    if (rawIrq() & 0x0001) { sawDone = true; break; }
    delay(1);
  }
  uint32_t took = millis() - t0;
  LOGF("  %-42s TX=%-3s DONE=%-3s mode=%-9s err=0x%04X %lums\n",
       label, sawTx ? "YES" : "no", sawDone ? "YES" : "no", mn(rawMode()), rawErr(),
       (unsigned long)took);

  if (rfswPin >= 0) pinMode(rfswPin, INPUT);
  return sawDone;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# real transmission, with RST = GPIO3\n");
  LOGF("########################################\n");

  pinMode(PIN_NSS, OUTPUT); digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  // A proper reset at last.
  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);
  uint8_t t[12] = {0x1D, 0x07, 0x40}, r[12];
  rawXfer(t, r, 6);
  LOGF("after real reset: reg 0x0740 = %02X %02X (1424 = proper reset)\n", r[4], r[5]);
  LOGF("mode = %s, errors = 0x%04X\n\n", mn(rawMode()), rawErr());

  const int BUSY_CAND[] = {2, 6};
  const int RFSW_CAND[] = {-1, 1, 4};

  bool win = false;
  for (int b = 0; b < 2 && !win; b++)
    for (int d = 0; d < 2 && !win; d++) {
      if (BUSY_CAND[b] == BUSY_CAND[d]) continue;
      for (int rf = 0; rf < 3 && !win; rf++)
        for (int c = 0; c < 2 && !win; c++)
          if (attempt(BUSY_CAND[b], BUSY_CAND[d], RFSW_CAND[rf], c == 0)) win = true;
    }

  LOGF("\n=== RESULT ===\n");
  LOGF(win ? "  *** A REAL TRANSMISSION COMPLETED - see the winning line above ***\n"
           : "  Still no transmission. Reset is now correct, so look elsewhere.\n");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
