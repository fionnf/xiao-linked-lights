// cs_verify - is GPIO5 really the chip select?
//
// Symptom driving this: every state-changing command (SetStandby(XOSC),
// Calibrate, SetTx) is silently ignored - mode never leaves STBY_RC, device
// errors stay 0x0000 - while register READS return believable data, including
// the correct SX126x LoRa sync word 0x1424.
//
// That combination is what a stuck chip-select looks like. SX126x commands are
// framed by NSS: the chip treats NSS going high as "command over, execute it".
// If NSS never deasserts, the chip sees one endless byte stream, executes
// nothing, and yet a read can still land on plausible-looking bytes when the
// stream happens to align. It would also explain the earlier pin scan, where
// CS=1 and CS=2 returned a repeating 0x2A - misalignment, not real answers.
//
// The test is simple and cannot be argued with. A pin that is genuinely NSS
// GATES the chip's replies:
//
//   toggled normally  -> chip answers  (0x1424)
//   held HIGH always  -> chip is deselected, must NOT answer (0x00 / 0xFF)
//
// If the answer is 0x1424 in both cases, GPIO5 is not doing anything and the
// real NSS is tied low or lives on another pin. The sketch then hunts for a pin
// that does gate the replies.

#include <SPI.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS_GUESS = 5, PIN_RST = 1, PIN_BUSY = 2;
static const uint16_t REG_SYNC = 0x0740;   // reads 0x1424 on a healthy SX126x

static SPIClass spi(FSPI);

// The USB CDC enumerates after setup() has already run, so a report printed once
// is routinely lost. Buffer it and reprint from loop() until someone reads it.
static String LOG;
static void LOGF(const char *fmt, ...) {
  char buf[192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  LOG += buf; Serial.print(buf);
}

// Read the sync-word register. csPin < 0 means "do not touch any CS at all",
// which is how we test whether the chip is permanently selected.
static uint16_t readSync(int csPin) {
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  if (csPin >= 0) { digitalWrite(csPin, LOW); delayMicroseconds(5); }
  spi.transfer(0x1D);
  spi.transfer((REG_SYNC >> 8) & 0xFF);
  spi.transfer(REG_SYNC & 0xFF);
  spi.transfer(0x00);
  uint8_t hi = spi.transfer(0x00);
  uint8_t lo = spi.transfer(0x00);
  if (csPin >= 0) { delayMicroseconds(5); digitalWrite(csPin, HIGH); }
  spi.endTransaction();
  return ((uint16_t)hi << 8) | lo;
}

static const int CANDIDATES[] = {1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 14, 15, 16,
                                 17, 18, 21, 38, 39, 40, 41, 42, 43, 44, 47, 48};
static const int N_CAND = sizeof(CANDIDATES) / sizeof(int);

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(2000);

  LOGF("\n########################################\n");
  LOGF("# chip-select verification\n");
  LOGF("########################################\n");

  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_CS_GUESS, OUTPUT); digitalWrite(PIN_CS_GUESS, HIGH);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  digitalWrite(PIN_RST, LOW); delay(2); digitalWrite(PIN_RST, HIGH); delay(20);

  uint16_t toggled = readSync(PIN_CS_GUESS);

  digitalWrite(PIN_CS_GUESS, HIGH);              // deselect and keep it there
  uint16_t heldHigh = readSync(-1);

  digitalWrite(PIN_CS_GUESS, LOW);               // permanently selected
  uint16_t heldLow = readSync(-1);
  digitalWrite(PIN_CS_GUESS, HIGH);

  LOGF("\nGPIO%d toggled as CS : 0x%04X\n", PIN_CS_GUESS, toggled);
  LOGF("GPIO%d held HIGH     : 0x%04X\n", PIN_CS_GUESS, heldHigh);
  LOGF("GPIO%d held LOW      : 0x%04X\n", PIN_CS_GUESS, heldLow);

  bool gates = (toggled == 0x1424) && (heldHigh != 0x1424);
  LOGF("\n");
  if (gates) {
    LOGF("GPIO%d GATES the chip - it really is NSS.\n", PIN_CS_GUESS);
    LOGF("So framing is fine and the ignored-commands problem lies elsewhere.\n");
  } else if (heldHigh == 0x1424) {
    LOGF("GPIO%d does NOT gate the chip: it still answers while deselected.\n",
                  PIN_CS_GUESS);
    LOGF("=> NSS is stuck low or is on another pin. Commands can never be\n");
    LOGF("   framed, which is exactly why none of them execute.\n");
    LOGF("\nHunting for a pin that does gate the replies...\n");
    int found = 0;
    for (int i = 0; i < N_CAND; i++) {
      int p = CANDIDATES[i];
      if (p == PIN_SCK || p == PIN_MISO || p == PIN_MOSI) continue;
      int prevMode = -1;
      pinMode(p, OUTPUT);
      digitalWrite(p, HIGH);                 // try to deselect using this pin
      delayMicroseconds(50);
      uint16_t v = readSync(-1);
      pinMode(p, INPUT);                     // release immediately
      (void)prevMode;
      if (v != 0x1424) {
        LOGF("  driving GPIO%-2d high silences the chip (0x%04X) <<< candidate NSS\n",
                      p, v);
        found++;
      }
      delay(2);
    }
    if (!found) LOGF("  no pin silenced the chip - NSS is tied low on the module.\n");
  } else {
    LOGF("Unexpected: chip did not return 0x1424 even when toggled (0x%04X).\n", toggled);
  }
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
