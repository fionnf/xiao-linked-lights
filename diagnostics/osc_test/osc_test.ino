// osc_test - drive the SX1262 with nothing but raw SPI commands.
//
// Where we are: the chip answers SPI perfectly (sync word 0x1424, begin() OK),
// reports NO device errors at any TCXO voltage, and yet never leaves STBY_RC.
// Because GetDeviceErrors is clean, XOSC_START_ERR is not the story, so the
// TCXO-voltage theory is dead and RadioLib itself is now the main suspect.
//
// This sketch removes every library from the path and walks the state machine by
// hand, in the order the datasheet prescribes:
//
//   SetStandby(STDBY_RC)      -> mode must read 2
//   SetDIO3AsTcxoCtrl + Calibrate
//   SetStandby(STDBY_XOSC)    -> mode must read 3. THIS is the real oscillator
//                                test: reaching STBY_XOSC proves the crystal
//                                starts. Staying at 2 proves it does not.
//   SetRfFrequency / SetPaConfig / SetTxParams / buffer / SetTx
//   SetTx                     -> mode must read 6
//
// Every step reports chip mode and device errors, so whichever one fails is
// visible rather than inferred.

#include <SPI.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2;

static SPIClass spi(FSPI);

// The native-USB CDC only enumerates a moment AFTER setup() has run, so anything
// printed once during the test is routinely lost before the host can attach.
// Buffer the whole report and reprint it from loop() until someone reads it.
static String LOG;

static void LOGF(const char *fmt, ...) {
  char buf[192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  LOG += buf;
  Serial.print(buf);
}



static bool waitBusy(uint32_t ms) {
  uint32_t t = millis();
  while (digitalRead(PIN_BUSY) == HIGH) { if (millis() - t > ms) return false; delayMicroseconds(20); }
  return true;
}

static void xfer(const uint8_t *tx, uint8_t *rx, int n) {
  if (!waitBusy(200)) LOGF("   [!] BUSY stuck high before command\n");
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(5);
  for (int i = 0; i < n; i++) {
    uint8_t v = spi.transfer(tx ? tx[i] : 0x00);
    if (rx) rx[i] = v;
  }
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
}

static void cmd(uint8_t op, const uint8_t *args, int nArgs) {
  uint8_t buf[16]; buf[0] = op;
  for (int i = 0; i < nArgs; i++) buf[1 + i] = args[i];
  xfer(buf, nullptr, 1 + nArgs);
}

static uint8_t mode() {
  uint8_t tx[2] = {0xC0, 0x00}, rx[2];
  xfer(tx, rx, 2);
  return (rx[1] >> 4) & 0x07;
}
static uint16_t devErrors() {
  uint8_t tx[4] = {0x17, 0, 0, 0}, rx[4];
  xfer(tx, rx, 4);
  return ((uint16_t)rx[2] << 8) | rx[3];
}
static uint16_t irq() {
  uint8_t tx[4] = {0x12, 0, 0, 0}, rx[4];
  xfer(tx, rx, 4);
  return ((uint16_t)rx[2] << 8) | rx[3];
}
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}
static void report(const char *step) {
  LOGF("  %-28s mode=%-9s errs=0x%04X irq=0x%04X\n", step, mn(mode()), devErrors(), irq());
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  Serial.println();
  LOGF("########################################\n");
  LOGF("# raw SX1262 state machine walk\n");
  LOGF("########################################\n");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  // hardware reset
  digitalWrite(PIN_RST, LOW); delay(2); digitalWrite(PIN_RST, HIGH); delay(20);
  LOGF("after reset: BUSY=%d\n", digitalRead(PIN_BUSY));
  report("post-reset");

  uint8_t a[8];

  a[0] = 0x00; cmd(0x80, a, 1);              // SetStandby(STDBY_RC)
  report("SetStandby(RC)");

  // ClearDeviceErrors so anything we see next is genuinely new
  a[0] = 0x00; a[1] = 0x00; cmd(0x07, a, 2);
  report("ClearDeviceErrors");

  // --- oscillator test WITHOUT any TCXO configuration (assume plain XTAL) ---
  a[0] = 0x01; cmd(0x80, a, 1);              // SetStandby(STDBY_XOSC)
  delay(20);
  report("SetStandby(XOSC) no-tcxo");
  bool xoscBare = (mode() == 3);

  // --- now configure DIO3 as TCXO supply and retry ---
  a[0] = 0x00; cmd(0x80, a, 1); delay(5);    // back to STBY_RC
  // SetDIO3AsTcxoCtrl(tcxoVoltage=0x02 -> 1.8V, timeout = 64*15.625us ~ 10ms)
  a[0] = 0x02; a[1] = 0x00; a[2] = 0x02; a[3] = 0x80;
  cmd(0x97, a, 4);
  report("SetDIO3AsTcxoCtrl(1.8V)");
  a[0] = 0x7F; cmd(0x89, a, 1); delay(50);   // Calibrate(all)
  report("Calibrate(all)");
  a[0] = 0x01; cmd(0x80, a, 1); delay(20);   // SetStandby(STDBY_XOSC)
  report("SetStandby(XOSC) w/tcxo");
  bool xoscTcxo = (mode() == 3);

  // --- minimal TX attempt ---
  a[0] = 0x01; cmd(0x8A, a, 1);              // SetPacketType(LORA)
  report("SetPacketType(LORA)");

  // SetRfFrequency 868 MHz: freq * 2^25 / 32e6 = 868e6*33.554432 = 0x36400000
  a[0] = 0x36; a[1] = 0x40; a[2] = 0x00; a[3] = 0x00;
  cmd(0x86, a, 4);
  report("SetRfFrequency(868)");

  a[0] = 0x04; a[1] = 0x07; a[2] = 0x00; a[3] = 0x01;   // SetPaConfig for SX1262
  cmd(0x95, a, 4);
  a[0] = 0x16; a[1] = 0x04;                             // SetTxParams(22dBm, ramp 200us)
  cmd(0x8E, a, 2);
  report("PA + TxParams");

  a[0] = 0xFF; a[1] = 0xFF; a[2] = 0xFF; a[3] = 0xFF;   // unmask all IRQs, no DIO
  a[4] = 0x00; a[5] = 0x00; a[6] = 0x00; a[7] = 0x00;
  cmd(0x08, a, 8);
  report("SetDioIrqParams(all)");

  a[0] = 0x00; a[1] = 0x00; cmd(0x8F, a, 2);            // SetBufferBaseAddress
  { uint8_t w[6] = {0x0E, 0x00, 'H','I','!','!'}; xfer(w, nullptr, 6); }  // WriteBuffer

  // SetTx with a 5 s timeout (timeout unit 15.625us -> 0x04E200 ~ 5s)
  a[0] = 0x04; a[1] = 0xE2; a[2] = 0x00;
  cmd(0x83, a, 3);
  delay(5);
  report("SetTx");

  bool sawTx = false, sawDone = false;
  uint32_t t1 = millis();
  while (millis() - t1 < 3000) {
    uint8_t m = mode();
    if (m == 6) sawTx = true;
    if (irq() & 0x0001) { sawDone = true; break; }
    delay(2);
  }
  report("after SetTx wait");

  LOGF("\n=== RESULT ===\n");
  LOGF("  STBY_XOSC reachable without TCXO cfg : %s\n", xoscBare ? "YES" : "no");
  LOGF("  STBY_XOSC reachable with TCXO cfg    : %s\n", xoscTcxo ? "YES" : "no");
  LOGF("  entered TX mode                      : %s\n", sawTx ? "YES" : "no");
  LOGF("  TX_DONE latched                      : %s\n", sawDone ? "YES" : "no");
  if (!xoscBare && !xoscTcxo)
    LOGF("  => the oscillator never starts. Hardware: no crystal/TCXO running.\n");
  else if (sawDone)
    LOGF("  => raw path WORKS. The fault is in how RadioLib is configured.\n");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(6000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
