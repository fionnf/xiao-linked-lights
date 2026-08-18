// spi_stability - is the SPI link to the SX1262 reliable at all?
//
// The smoking gun: identical code read the LoRa sync-word register as 0x1424 in
// one run and 0x24B4 in another. Those are the same bytes shifted by one
// position, i.e. the link is slipping bytes. That single fact explains every
// oddity so far:
//
//   * register reads that "work" are landing correctly by luck
//   * every state-changing command (SetStandby(XOSC), Calibrate, SetTx) arrives
//     garbled, so the chip executes none of them - mode never leaves STBY_RC and
//     GetDeviceErrors stays 0x0000 because nothing ever actually ran
//   * the earlier pin scan saw a repeating 0x2A on other CS pins - misalignment
//   * RadioLib begin() "succeeds" because it only needs one lucky read
//
// So: hammer the same register many times, at several clock speeds, and count how
// often the answer is exactly 0x1424. A healthy link is 100% at every speed. A
// link that degrades as the clock rises points at wiring - long jumpers, a poor
// solder joint, or a missing/shared ground.

#include <SPI.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2;
static const uint16_t REG_SYNC = 0x0740;
static const uint16_t EXPECT   = 0x1424;

static SPIClass spi(FSPI);

static String LOG;
static void LOGF(const char *fmt, ...) {
  char buf[200];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  LOG += buf; Serial.print(buf);
}

static bool waitBusy(uint32_t ms) {
  uint32_t t = millis();
  while (digitalRead(PIN_BUSY) == HIGH) { if (millis() - t > ms) return false; delayMicroseconds(20); }
  return true;
}

static uint16_t readSync(uint32_t hz) {
  waitBusy(50);
  spi.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(10);
  spi.transfer(0x1D);
  spi.transfer((REG_SYNC >> 8) & 0xFF);
  spi.transfer(REG_SYNC & 0xFF);
  spi.transfer(0x00);
  uint8_t hi = spi.transfer(0x00);
  uint8_t lo = spi.transfer(0x00);
  delayMicroseconds(10);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return ((uint16_t)hi << 8) | lo;
}

static const uint32_t SPEEDS[] = {100000, 250000, 500000, 1000000, 2000000, 4000000, 8000000};
static const int N_SPEEDS = sizeof(SPEEDS) / sizeof(uint32_t);
static const int TRIALS = 40;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# SPI stability sweep\n");
  LOGF("########################################\n");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);
  LOGF("BUSY after reset = %d (expect 0)\n\n", digitalRead(PIN_BUSY));
  LOGF("expecting 0x%04X from register 0x%04X, %d reads per speed\n\n", EXPECT, REG_SYNC, TRIALS);

  int bestSpeedIdx = -1;
  for (int s = 0; s < N_SPEEDS; s++) {
    int ok = 0;
    uint16_t seen[6]; int nSeen = 0;
    for (int i = 0; i < TRIALS; i++) {
      uint16_t v = readSync(SPEEDS[s]);
      if (v == EXPECT) ok++;
      else {
        bool known = false;
        for (int k = 0; k < nSeen; k++) if (seen[k] == v) known = true;
        if (!known && nSeen < 6) seen[nSeen++] = v;
      }
      delayMicroseconds(200);
    }
    LOGF("%7lu Hz : %2d/%2d correct", (unsigned long)SPEEDS[s], ok, TRIALS);
    if (ok < TRIALS) {
      LOGF("   wrong values seen:");
      for (int k = 0; k < nSeen; k++) LOGF(" 0x%04X", seen[k]);
    }
    LOGF("\n");
    if (ok == TRIALS && bestSpeedIdx < 0) bestSpeedIdx = s;
  }

  LOGF("\n=== RESULT ===\n");
  if (bestSpeedIdx < 0) {
    LOGF("The link is unreliable at EVERY speed tested.\n");
    LOGF("This is a physical connection problem, not firmware:\n");
    LOGF("  - check the module is fully seated / joints are sound\n");
    LOGF("  - check GND is shared between XIAO and module\n");
    LOGF("  - shorten any jumper wires\n");
  } else {
    LOGF("Link is 100%% reliable at %lu Hz and below.\n", (unsigned long)SPEEDS[bestSpeedIdx]);
    LOGF("=> drive the radio at that clock and the ignored-command problem\n");
    LOGF("   should disappear.\n");
  }
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
