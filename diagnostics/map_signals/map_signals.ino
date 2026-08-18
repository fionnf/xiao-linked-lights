// map_signals - identify every LoRa control line by experiment.
//
// The Seeed schematic (docs/Schematic_Diagram_Wio-SX1262_for_XIAO.pdf) lists the
// module's signals as: DIO1, BUSY, RST, NSS, RF_SW1, SCK, MOSI, MISO - broken out
// on two 1x7 headers (J1/J2) as well as the 30-pin B2B connector. This board is
// wired through the headers, which is why the pin map does not match Meshtastic's
// variant.
//
// Already proved: SCK=7, MISO=8, MOSI=9, NSS=5 (GPIO5 gates the chip).
// Unknown: which of GPIO1/2/3/4/6 carry BUSY, RST, DIO1 and RF_SW1.
//
// Guessing has cost a lot of time, so identify each one with a test that cannot be
// misread:
//
//  BUSY  - an OUTPUT from the module. Issue Calibrate(0x7F), which takes several
//          ms, and sample every candidate fast. The pin that goes HIGH during the
//          calibration and returns LOW after it IS BUSY.
//
//  RST   - an INPUT to the module. Write a marker value into a register, pulse a
//          candidate LOW, then read the register back. Only the real reset line
//          restores the register to its power-on value.
//
//  RF_SW1 - an INPUT. Cannot be found by observation; it is whatever remains. It
//          matters because it gates the module's antenna switch, and nothing has
//          ever driven it.
//
// Why this matters for the real bug: RadioLib's blocking transmit() returned
// "success" in 1 ms, because it waits for DIO1 to go HIGH and the pin we told it
// was DIO1 sits permanently HIGH. Until these lines are right, no library call
// means anything.

#include <SPI.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9, PIN_NSS = 5;
static const int CAND[] = {1, 2, 3, 4, 6};
static const int N_CAND = 5;

static SPIClass spi(FSPI);
static String LOG;
static void LOGF(const char *fmt, ...) {
  char b[220]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  LOG += b; Serial.print(b);
}

static void xfer(const uint8_t *tx, uint8_t *rx, int n) {
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW); delayMicroseconds(10);
  for (int i = 0; i < n; i++) { uint8_t v = spi.transfer(tx ? tx[i] : 0); if (rx) rx[i] = v; }
  delayMicroseconds(10); digitalWrite(PIN_NSS, HIGH);
  spi.endTransaction();
}
static void cmd(uint8_t op, const uint8_t *args, int n) {
  uint8_t b[16]; b[0] = op;
  for (int i = 0; i < n; i++) b[1 + i] = args[i];
  xfer(b, nullptr, 1 + n);
}
static void readReg(uint16_t a, uint8_t *out, int n) {
  uint8_t tx[24] = {0}, rx[24];
  tx[0] = 0x1D; tx[1] = a >> 8; tx[2] = a & 0xFF;
  xfer(tx, rx, 4 + n);
  for (int i = 0; i < n; i++) out[i] = rx[4 + i];
}
static void writeReg(uint16_t a, const uint8_t *d, int n) {
  uint8_t tx[24];
  tx[0] = 0x0D; tx[1] = a >> 8; tx[2] = a & 0xFF;
  for (int i = 0; i < n; i++) tx[3 + i] = d[i];
  xfer(tx, nullptr, 3 + n);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# identify BUSY / RST / DIO1 / RF_SW1\n");
  LOGF("########################################\n");

  pinMode(PIN_NSS, OUTPUT); digitalWrite(PIN_NSS, HIGH);
  for (int i = 0; i < N_CAND; i++) pinMode(CAND[i], INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  delay(20);

  uint8_t v[8];
  readReg(0x0320, v, 6); v[6] = 0;
  LOGF("version = \"%s\"  (chip is alive)\n\n", (char *)v);

  LOGF("resting states:  ");
  for (int i = 0; i < N_CAND; i++) LOGF("GPIO%d=%d  ", CAND[i], digitalRead(CAND[i]));
  LOGF("\n");

  // ---------- find BUSY ----------
  LOGF("\n--- finding BUSY (watch for a pin that pulses HIGH during Calibrate) ---\n");
  int highCount[N_CAND] = {0};
  uint8_t a[4];
  a[0] = 0x00; cmd(0x80, a, 1); delay(5);            // SetStandby(STBY_RC)
  a[0] = 0x7F; cmd(0x89, a, 1);                      // Calibrate(all) - takes ms
  uint32_t t1 = micros();
  while (micros() - t1 < 20000) {                    // sample hard for 20 ms
    for (int i = 0; i < N_CAND; i++) if (digitalRead(CAND[i])) highCount[i]++;
  }
  delay(50);
  LOGF("  during calibrate: ");
  for (int i = 0; i < N_CAND; i++) LOGF("GPIO%d=%d%%  ", CAND[i], highCount[i] ? 100 : 0);
  LOGF("\n  after:            ");
  for (int i = 0; i < N_CAND; i++) LOGF("GPIO%d=%d   ", CAND[i], digitalRead(CAND[i]));
  LOGF("\n");
  int busyPin = -1;
  for (int i = 0; i < N_CAND; i++)
    if (highCount[i] > 0 && digitalRead(CAND[i]) == 0) { busyPin = CAND[i]; break; }
  if (busyPin > 0) LOGF("  => BUSY is GPIO%d (pulsed high, then returned low)\n", busyPin);
  else            LOGF("  => no pin pulsed; BUSY may be unconnected or calibration was instant\n");

  // ---------- find RST ----------
  LOGF("\n--- finding RST (only a real reset restores a register default) ---\n");
  int rstPin = -1;
  for (int i = 0; i < N_CAND; i++) {
    int p = CAND[i];
    if (p == busyPin) continue;

    uint8_t marker[2] = {0x5A, 0xA5};
    writeReg(0x0740, marker, 2);
    uint8_t chk[2]; readReg(0x0740, chk, 2);
    if (chk[0] != 0x5A || chk[1] != 0xA5) { LOGF("  GPIO%-2d skipped (marker write failed)\n", p); continue; }

    pinMode(p, OUTPUT); digitalWrite(p, LOW); delay(2);
    digitalWrite(p, HIGH); pinMode(p, INPUT); delay(30);

    uint8_t after[2]; readReg(0x0740, after, 2);
    bool reset = !(after[0] == 0x5A && after[1] == 0xA5);
    LOGF("  pulsing GPIO%-2d low -> reg 0x0740 = %02X %02X  %s\n",
         p, after[0], after[1], reset ? "<<< RESET HAPPENED - this is RST" : "(unchanged)");
    if (reset && rstPin < 0) rstPin = p;
  }
  if (rstPin < 0) LOGF("  => no pin acted as reset\n");

  // ---------- what's left ----------
  LOGF("\n--- remaining lines ---\n");
  LOGF("  known: SCK=%d MISO=%d MOSI=%d NSS=%d", PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  if (busyPin > 0) LOGF(" BUSY=%d", busyPin);
  if (rstPin > 0)  LOGF(" RST=%d", rstPin);
  LOGF("\n  unaccounted for (DIO1 and RF_SW1 must be among these): ");
  for (int i = 0; i < N_CAND; i++)
    if (CAND[i] != busyPin && CAND[i] != rstPin) LOGF("GPIO%d ", CAND[i]);
  LOGF("\n");

  LOGF("\n=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
