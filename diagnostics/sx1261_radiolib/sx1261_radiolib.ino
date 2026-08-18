// sx1261_radiolib - drive the part as what it says it is: an SX1261.
//
// The version register reads ASCII "SX1261", and every previous attempt used
// RadioLib's SX1262 class or a hand-rolled command sequence. Two things could be
// wrong with that:
//
//   1. the PA configuration is for the wrong part, and
//   2. more subtly, the INIT ORDER matters on SX126x - the TCXO must be set up
//      before Calibrate(), and CalibrateImage() must run after the frequency is
//      set. My hand-written sequence may have got that ordering wrong, and a
//      mis-ordered init is enough to leave the PLL unable to lock, which looks
//      exactly like "never leaves STBY_RC with no error reported".
//
// RadioLib's SX1261 class gets both right. If begin() succeeds and a transmission
// completes here, the whole mystery was the wrong driver class.
//
// Also print RadioLib's own error code for every step, and read the chip mode
// through raw SPI afterwards, so success or failure is unambiguous.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2, PIN_DIO1 = 4;

static SPIClass spi(FSPI);
// SX1261, not SX1262 - this is the whole point of the sketch.
static SX1261 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static String LOG;
static void LOGF(const char *fmt, ...) {
  char b[220]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  LOG += b; Serial.print(b);
}

// raw mode read, so we are not taking RadioLib's word for anything
static uint8_t rawMode() {
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW); delayMicroseconds(10);
  spi.transfer(0xC0);
  uint8_t st = spi.transfer(0x00);
  delayMicroseconds(10); digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return (st >> 4) & 0x07;
}
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# RadioLib SX1261 driver\n");
  LOGF("########################################\n");

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  // SX1261 tops out at +15 dBm; EU 868 allows 14 dBm ERP, so 14 is the right ask.
  struct { const char *label; float tcxo; int pwr; } TRIES[] = {
    {"tcxo 1.8V, +14 dBm", 1.8, 14},
    {"no tcxo,   +14 dBm", 0.0, 14},
    {"tcxo 3.3V, +14 dBm", 3.3, 14},
    {"tcxo 1.8V, +10 dBm", 1.8, 10},
  };

  bool done = false;
  for (unsigned i = 0; i < sizeof(TRIES) / sizeof(TRIES[0]) && !done; i++) {
    LOGF("\n--- %s ---\n", TRIES[i].label);
    int st = radio.begin(868.0, 250.0, 11, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                         TRIES[i].pwr, 8, TRIES[i].tcxo, false);
    LOGF("  begin              -> %d %s\n", st, st == 0 ? "" : "(FAILED)");
    if (st != RADIOLIB_ERR_NONE) continue;

    LOGF("  setDio2AsRfSwitch  -> %d\n", radio.setDio2AsRfSwitch(true));
    LOGF("  setCRC             -> %d\n", radio.setCRC(true));
    LOGF("  mode before TX     -> %s\n", mn(rawMode()));

    // Blocking transmit: RadioLib waits for completion itself. Time it, because a
    // real SF11/250k transmission of this length takes on the order of 100 ms -
    // an instant return means nothing was sent.
    uint32_t t1 = millis();
    st = radio.transmit("HELLO SX1261");
    uint32_t took = millis() - t1;
    LOGF("  transmit           -> %d  (%lu ms)\n", st, (unsigned long)took);

    if (st == RADIOLIB_ERR_NONE && took > 20) {
      LOGF("\n  *** TRANSMISSION COMPLETED - THIS CONFIG WORKS ***\n");
      done = true;
    } else if (st == RADIOLIB_ERR_TX_TIMEOUT) {
      LOGF("  (TX timeout - RadioLib waited on DIO1, which is not wired)\n");
    }
  }

  LOGF("\n=== RESULT ===\n");
  LOGF(done ? "  SX1261 driver transmits. Use the SX1261 class from here on.\n"
            : "  Still no completed transmission with the SX1261 driver.\n");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
