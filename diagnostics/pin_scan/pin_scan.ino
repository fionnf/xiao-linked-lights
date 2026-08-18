// pin_scan - hunt for an SX1262 that is not on the pins we expect.
//
// RadioLib reports CHIP_NOT_FOUND using the pin map from Meshtastic's
// seeed_xiao_s3 variant (sck=7 miso=8 mosi=9 cs=41 busy=40 rst=42). Before
// blaming the hardware, check whether the module is simply wired somewhere else.
//
// Method: an SX1262 sitting in STBY_RC answers GetStatus (0xC0) at any time
// without needing a reset. A disconnected MISO line reads back as all-zeroes or
// all-ones, so any other value is a candidate worth a closer look. Each
// candidate is then confirmed by reading the LoRa sync word register, which
// must read 0x1424 on a genuine SX126x.
//
// Pins excluded from the scan, deliberately:
//   19, 20      native USB D-/D+  - driving these kills the debug link
//   26..37      SPI flash / PSRAM - driving these bricks the running program
//   7, 8, 9     the SPI bus itself

#include <SPI.h>

static const uint8_t OP_GET_STATUS = 0xC0;
static const uint8_t OP_READ_REG   = 0x1D;
static const uint16_t REG_SYNC     = 0x0740;

static const int CANDIDATES[] = {
  1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
static const int N_CAND = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

static SPIClass spi(FSPI);

// One GetStatus transaction against a given chip-select pin.
static uint8_t probeStatus(int csPin) {
  pinMode(csPin, OUTPUT);
  digitalWrite(csPin, HIGH);
  delayMicroseconds(50);
  digitalWrite(csPin, LOW);
  delayMicroseconds(10);
  spi.transfer(OP_GET_STATUS);
  uint8_t st = spi.transfer(0x00);
  digitalWrite(csPin, HIGH);
  pinMode(csPin, INPUT);          // release, so we cannot fight another driver
  return st;
}

static uint16_t probeSync(int csPin) {
  pinMode(csPin, OUTPUT);
  digitalWrite(csPin, HIGH);
  delayMicroseconds(50);
  digitalWrite(csPin, LOW);
  delayMicroseconds(10);
  spi.transfer(OP_READ_REG);
  spi.transfer((REG_SYNC >> 8) & 0xFF);
  spi.transfer(REG_SYNC & 0xFF);
  spi.transfer(0x00);
  uint8_t hi = spi.transfer(0x00);
  uint8_t lo = spi.transfer(0x00);
  digitalWrite(csPin, HIGH);
  pinMode(csPin, INPUT);
  return ((uint16_t)hi << 8) | lo;
}

static bool interesting(uint8_t st) { return st != 0x00 && st != 0xFF; }

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(2000);

  Serial.println("\n########################################");
  Serial.println("# SX1262 pin scan");
  Serial.println("########################################");

  // Which pins are being driven by something other than us? An output pin held
  // firmly high or low is a hint about what is actually connected.
  Serial.println("\n-- floating-state survey (INPUT_PULLUP then INPUT_PULLDOWN) --");
  Serial.println("pin  pullup  pulldown  verdict");
  for (int i = 0; i < N_CAND; i++) {
    int p = CANDIDATES[i];
    pinMode(p, INPUT_PULLUP);   delay(2); int up = digitalRead(p);
    pinMode(p, INPUT_PULLDOWN); delay(2); int dn = digitalRead(p);
    pinMode(p, INPUT);
    const char *verdict = "floating (nothing attached)";
    if (up == 1 && dn == 1) verdict = "DRIVEN HIGH by something";
    else if (up == 0 && dn == 0) verdict = "DRIVEN LOW by something";
    Serial.printf("%-4d %-7d %-9d %s\n", p, up, dn, verdict);
  }

  Serial.println("\n-- SPI scan: sck=7 mosi=9 miso=8, sweeping CS --");
  spi.begin(7, 8, 9, -1);
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  int hits = 0;
  for (int i = 0; i < N_CAND; i++) {
    int cs = CANDIDATES[i];
    uint8_t st = probeStatus(cs);
    if (interesting(st)) {
      uint16_t sync = probeSync(cs);
      Serial.printf("  cs=%-3d status=0x%02X sync=0x%04X %s\n", cs, st, sync,
                    sync == 0x1424 ? "<<< SX126x CONFIRMED" : "(status odd, sync wrong)");
      hits++;
    }
  }
  spi.endTransaction();
  if (!hits) Serial.println("  no response on any CS pin with miso=8");

  // If MISO itself is elsewhere, the sweep above can never see anything.
  Serial.println("\n-- SPI scan: sweeping MISO too (cs=41, then cs=40,39,38) --");
  const int csTry[] = {41, 40, 39, 38, 44, 43};
  int hits2 = 0;
  for (int m = 0; m < N_CAND; m++) {
    int miso = CANDIDATES[m];
    if (miso == 7 || miso == 9) continue;
    spi.end();
    spi.begin(7, miso, 9, -1);
    spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    for (unsigned c = 0; c < sizeof(csTry) / sizeof(csTry[0]); c++) {
      int cs = csTry[c];
      if (cs == miso) continue;
      uint8_t st = probeStatus(cs);
      if (interesting(st)) {
        uint16_t sync = probeSync(cs);
        Serial.printf("  miso=%-3d cs=%-3d status=0x%02X sync=0x%04X %s\n",
                      miso, cs, st, sync,
                      sync == 0x1424 ? "<<< SX126x CONFIRMED" : "(unconfirmed)");
        hits2++;
      }
    }
    spi.endTransaction();
  }
  if (!hits2) Serial.println("  no response for any (miso, cs) pair");

  Serial.println("\n=== SCAN COMPLETE ===");
  if (!hits && !hits2)
    Serial.println("No SX126x found anywhere. The module is not electrically present.");
}

void loop() { Serial.println("heartbeat"); delay(3000); }
