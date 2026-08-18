// pa_config - the chip says "SX1261", so try the SX1261 power-amplifier config.
//
// The decisive new fact: register 0x0320 reads ASCII "SX1261", not "SX1262".
// Those two parts need DIFFERENT SetPaConfig arguments, and every SetTx attempt
// so far used the SX1262 form:
//
//   SX1262: paDutyCycle=0x04 hpMax=0x07 deviceSel=0x00 paLut=0x01, up to +22 dBm
//   SX1261: paDutyCycle=0x04 hpMax=0x00 deviceSel=0x01 paLut=0x01, up to +15 dBm
//
// deviceSel is the field that names the part. Handing an SX1261 deviceSel=0x00 is
// an invalid configuration, and the datasheet's own warning is that an out-of-range
// PA setting can leave the PA unable to ramp - which is exactly what we see:
// SetTx accepted, no device error, but the chip never leaves STBY_RC.
//
// This also retires the "oscillator is dead" conclusion from the previous test:
// SetTx starts the oscillator by itself on the way to FS/TX, so a failed SetTx
// looks identical to a failed oscillator from the outside.
//
// Try every plausible PA/power combination and report which one reaches TX.

#include <SPI.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2;

static SPIClass spi(FSPI);
static String LOG;
static void LOGF(const char *fmt, ...) {
  char b[220]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  LOG += b; Serial.print(b);
}

static bool waitBusy(uint32_t ms) {
  uint32_t t = millis();
  while (digitalRead(PIN_BUSY) == HIGH) { if (millis() - t > ms) return false; delayMicroseconds(20); }
  return true;
}
static void xfer(const uint8_t *tx, uint8_t *rx, int n) {
  waitBusy(100);
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW); delayMicroseconds(10);
  for (int i = 0; i < n; i++) { uint8_t v = spi.transfer(tx ? tx[i] : 0); if (rx) rx[i] = v; }
  delayMicroseconds(10); digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
}
static void cmd(uint8_t op, const uint8_t *args, int n) {
  uint8_t b[16]; b[0] = op;
  for (int i = 0; i < n; i++) b[1 + i] = args[i];
  xfer(b, nullptr, 1 + n);
}
static uint8_t chipMode() { uint8_t tx[2] = {0xC0, 0}, rx[2]; xfer(tx, rx, 2); return (rx[1] >> 4) & 7; }
static uint16_t devErrors() { uint8_t tx[4] = {0x17,0,0,0}, rx[4]; xfer(tx, rx, 4); return ((uint16_t)rx[2] << 8) | rx[3]; }
static uint16_t irqStat()  { uint8_t tx[4] = {0x12,0,0,0}, rx[4]; xfer(tx, rx, 4); return ((uint16_t)rx[2] << 8) | rx[3]; }
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}

// One full attempt: reset, configure, transmit, watch.
static bool attempt(const char *label, uint8_t dutyCycle, uint8_t hpMax,
                    uint8_t deviceSel, int8_t dbm, bool useTcxo) {
  uint8_t a[8];
  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);

  a[0] = 0x00; cmd(0x80, a, 1); delay(5);               // SetStandby(STBY_RC)
  a[0] = 0x00; a[1] = 0x00; cmd(0x07, a, 2);            // ClearDeviceErrors

  if (useTcxo) {
    a[0] = 0x02; a[1] = 0x00; a[2] = 0x10; a[3] = 0x00; // DIO3 -> 1.8V, 64ms
    cmd(0x97, a, 4); delay(5);
    a[0] = 0x7F; cmd(0x89, a, 1); delay(100);           // Calibrate(all)
  }

  a[0] = 0x01; cmd(0x8A, a, 1);                          // SetPacketType(LORA)
  a[0] = 0x36; a[1] = 0x40; a[2] = 0x00; a[3] = 0x00;    // SetRfFrequency 868 MHz
  cmd(0x86, a, 4);

  a[0] = dutyCycle; a[1] = hpMax; a[2] = deviceSel; a[3] = 0x01;
  cmd(0x95, a, 4);                                       // SetPaConfig
  a[0] = (uint8_t)dbm; a[1] = 0x04;                      // SetTxParams(power, 200us ramp)
  cmd(0x8E, a, 2);

  // modulation + packet params, otherwise SetTx has nothing coherent to send
  a[0] = 0x0B; a[1] = 0x04; a[2] = 0x01; a[3] = 0x00;    // SF11, BW250, CR4/5, LDRO off
  cmd(0x8B, a, 4);
  a[0] = 0x00; a[1] = 0x08; a[2] = 0x00; a[3] = 0x08;
  a[4] = 0x01; a[5] = 0x00; a[6] = 0x00; a[7] = 0x00;    // preamble 8, explicit hdr, len 8, CRC on
  cmd(0x8C, a, 8);

  a[0] = 0xFF; a[1] = 0xFF; a[2] = 0xFF; a[3] = 0xFF;
  a[4] = 0x00; a[5] = 0x00; a[6] = 0x00; a[7] = 0x00;
  cmd(0x08, a, 8);                                       // SetDioIrqParams: unmask all

  a[0] = 0x00; a[1] = 0x00; cmd(0x8F, a, 2);             // SetBufferBaseAddress
  { uint8_t w[9] = {0x0E, 0x00, 'P','A','T','E','S','T','!'}; xfer(w, nullptr, 9); }

  a[0] = 0x00; a[1] = 0x00; a[2] = 0x00; cmd(0x83, a, 3); // SetTx, no timeout
  delay(2);

  bool sawTx = false, sawDone = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 2500) {
    if (chipMode() == 6) sawTx = true;
    if (irqStat() & 0x0001) { sawDone = true; break; }
    delay(2);
  }
  uint16_t e = devErrors();
  LOGF("  %-34s reachedTX=%-3s TX_DONE=%-3s mode=%-9s errs=0x%04X\n",
       label, sawTx ? "YES" : "no", sawDone ? "YES" : "no", mn(chipMode()), e);
  a[0] = 0x00; cmd(0x80, a, 1);
  return sawDone;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# SX1261 vs SX1262 PA configuration\n");
  LOGF("########################################\n");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);
  uint8_t v[8]; uint8_t tx[12] = {0x1D, 0x03, 0x20}, rx[12];
  xfer(tx, rx, 10);
  for (int i = 0; i < 6; i++) v[i] = rx[4 + i];
  v[6] = 0;
  LOGF("version register = \"%s\"\n\n", (char *)v);

  bool ok = false;
  // SX1261 PA config, which is what the version register implies
  ok |= attempt("SX1261 PA, +14 dBm, TCXO",  0x04, 0x00, 0x01, 14, true);
  ok |= attempt("SX1261 PA, +14 dBm, no TCXO", 0x04, 0x00, 0x01, 14, false);
  ok |= attempt("SX1261 PA, +10 dBm, TCXO",  0x04, 0x00, 0x01, 10, true);
  // and the SX1262 form, for comparison
  ok |= attempt("SX1262 PA, +14 dBm, TCXO",  0x04, 0x07, 0x00, 14, true);
  ok |= attempt("SX1262 PA, +22 dBm, TCXO",  0x04, 0x07, 0x00, 22, true);

  LOGF("\n=== RESULT ===\n");
  LOGF(ok ? "  A working PA configuration was found - see which line says TX_DONE=YES.\n"
          : "  No PA configuration reached TX. The oscillator really is not starting.\n");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
