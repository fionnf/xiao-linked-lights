// busy_dio1 - settle BUSY and DIO1 now that reset actually works.
//
// The earlier attempt to find BUSY (diagnostics/map_signals) was worthless: it ran
// before RST was known, so the chip had never been reset, Calibrate never really
// executed, and naturally no pin ever pulsed. Everything downstream inherited that
// uncertainty.
//
// Why it matters now: RadioLib waits on the BUSY pin around every command. With
// BUSY pointed at the wrong GPIO those waits are meaningless, which is exactly
// what the last run looked like - the receiver never entered RX mode, and chip
// mode readings on the transmitter lagged seconds behind reality.
//
// Two honest tests, both requiring a real reset first (RST = GPIO3):
//
//   BUSY - an OUTPUT, HIGH while the chip is working. Calibrate(0x7F) takes
//          several ms after a genuine reset. Sample every candidate hard and find
//          the pin that goes HIGH during it and LOW after.
//
//   DIO1 - an OUTPUT, asserted when an unmasked IRQ fires. Unmask everything,
//          transmit, and find the pin that goes HIGH exactly when TX_DONE latches
//          and LOW again when the IRQ is cleared.

#include <SPI.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_NSS = 5, PIN_RST = 3;
static const int CAND[] = {1, 2, 4, 6};
static const int N_CAND = 4;

static SPIClass spi(FSPI);
static String LOG;
static void LOGF(const char *fmt, ...) {
  char b[240]; va_list ap; va_start(ap, fmt);
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
static void cmd(uint8_t op, const uint8_t *a, int n) {
  uint8_t b[16]; b[0] = op;
  for (int i = 0; i < n; i++) b[1 + i] = a[i];
  xfer(b, nullptr, 1 + n);
}
static uint8_t mode() { uint8_t t[2] = {0xC0, 0}, r[2]; xfer(t, r, 2); return (r[1] >> 4) & 7; }
static uint16_t irq() { uint8_t t[4] = {0x12,0,0,0}, r[4]; xfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
static uint16_t err() { uint8_t t[4] = {0x17,0,0,0}, r[4]; xfer(t, r, 4); return ((uint16_t)r[2] << 8) | r[3]; }
static const char *mn(uint8_t m) {
  switch (m) { case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
               case 5: return "RX"; case 6: return "TX"; default: return "?"; }
}
static void hardReset() { digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50); }

// Bring the radio up by hand, in datasheet order, so no library is in the way.
static void configure() {
  uint8_t a[8];
  a[0] = 0x00; cmd(0x80, a, 1); delay(5);                 // SetStandby(STBY_RC)
  a[0] = 0x00; a[1] = 0x00; cmd(0x07, a, 2);              // ClearDeviceErrors
  a[0] = 0x02; a[1] = 0x00; a[2] = 0x10; a[3] = 0x00;     // DIO3 -> TCXO 1.8V, 64ms
  cmd(0x97, a, 4); delay(5);
  a[0] = 0x7F; cmd(0x89, a, 1); delay(100);               // Calibrate(all)
  a[0] = 0x01; cmd(0x8A, a, 1);                            // SetPacketType(LORA)
  a[0] = 0x36; a[1] = 0x40; a[2] = 0x00; a[3] = 0x00;     // 868 MHz
  cmd(0x86, a, 4);
  a[0] = 0x04; a[1] = 0x07; a[2] = 0x00; a[3] = 0x01; cmd(0x95, a, 4);  // SetPaConfig
  a[0] = 0x0E; a[1] = 0x04; cmd(0x8E, a, 2);              // SetTxParams(+14 dBm)
  a[0] = 0x09; a[1] = 0x04; a[2] = 0x01; a[3] = 0x00; cmd(0x8B, a, 4);  // SF9 BW250 CR4/5
  a[0] = 0x00; a[1] = 0x08; a[2] = 0x00; a[3] = 0x08;
  a[4] = 0x01; a[5] = 0x00; a[6] = 0x00; a[7] = 0x00; cmd(0x8C, a, 8);  // packet params
  a[0] = 0xFF; a[1] = 0xFF; a[2] = 0xFF; a[3] = 0xFF;     // unmask all IRQs
  a[4] = 0xFF; a[5] = 0xFF; a[6] = 0x00; a[7] = 0x00;     // ...and map them all to DIO1
  cmd(0x08, a, 8);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 6000) delay(10);
  delay(1200);

  LOGF("\n########################################\n");
  LOGF("# identify BUSY and DIO1 (reset works now)\n");
  LOGF("########################################\n");

  pinMode(PIN_NSS, OUTPUT); digitalWrite(PIN_NSS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  for (int i = 0; i < N_CAND; i++) pinMode(CAND[i], INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  hardReset();
  uint8_t t[12] = {0x1D, 0x07, 0x40}, r[12]; xfer(t, r, 6);
  LOGF("reg 0x0740 after reset = %02X %02X (1424 = real reset)\n", r[4], r[5]);
  LOGF("mode=%s err=0x%04X\n", mn(mode()), err());

  LOGF("\nresting: ");
  for (int i = 0; i < N_CAND; i++) LOGF("GPIO%d=%d ", CAND[i], digitalRead(CAND[i]));
  LOGF("\n");

  // ---------- BUSY ----------
  LOGF("\n--- BUSY: sample hard during Calibrate ---\n");
  hardReset();
  uint8_t a[8];
  a[0] = 0x00; cmd(0x80, a, 1); delay(5);
  a[0] = 0x02; a[1] = 0x00; a[2] = 0x10; a[3] = 0x00; cmd(0x97, a, 4); delay(5);

  int hi[N_CAND] = {0}, tot = 0;
  a[0] = 0x7F; cmd(0x89, a, 1);                 // Calibrate - now it really runs
  uint32_t t1 = micros();
  while (micros() - t1 < 30000) {
    for (int i = 0; i < N_CAND; i++) if (digitalRead(CAND[i])) hi[i]++;
    tot++;
  }
  delay(100);
  int busyPin = -1;
  for (int i = 0; i < N_CAND; i++) {
    int pct = tot ? (hi[i] * 100) / tot : 0;
    int now = digitalRead(CAND[i]);
    LOGF("  GPIO%-2d high %3d%% during calibrate, %d after", CAND[i], pct, now);
    if (pct > 5 && pct < 100 && now == 0) { LOGF("   <<< BUSY"); if (busyPin < 0) busyPin = CAND[i]; }
    LOGF("\n");
  }
  if (busyPin < 0) LOGF("  (no clear BUSY pulse)\n");

  // ---------- DIO1 ----------
  LOGF("\n--- DIO1: which pin follows the TX_DONE IRQ ---\n");
  hardReset();
  configure();
  a[0] = 0x00; a[1] = 0x00; cmd(0x8F, a, 2);
  { uint8_t w[10] = {0x0E, 0x00, 'D','I','O','1','T','E','S','T'}; xfer(w, nullptr, 10); }

  int before[N_CAND];
  for (int i = 0; i < N_CAND; i++) before[i] = digitalRead(CAND[i]);

  a[0] = 0x00; a[1] = 0x00; a[2] = 0x00; cmd(0x83, a, 3);   // SetTx
  bool done = false;
  int during[N_CAND];
  for (int i = 0; i < N_CAND; i++) during[i] = 0;
  uint32_t t2 = millis();
  while (millis() - t2 < 3000) {
    if (irq() & 0x0001) { done = true; break; }
    delay(1);
  }
  for (int i = 0; i < N_CAND; i++) during[i] = digitalRead(CAND[i]);
  uint16_t irqNow = irq();

  a[0] = 0xFF; a[1] = 0xFF; cmd(0x02, a, 2);                // ClearIrqStatus
  delay(5);
  int after[N_CAND];
  for (int i = 0; i < N_CAND; i++) after[i] = digitalRead(CAND[i]);

  LOGF("  TX_DONE latched: %s (irq=0x%04X, mode=%s, %lums)\n",
       done ? "YES" : "no", irqNow, mn(mode()), (unsigned long)(millis() - t2));
  int dio1Pin = -1;
  for (int i = 0; i < N_CAND; i++) {
    LOGF("  GPIO%-2d  before=%d duringIRQ=%d afterClear=%d", CAND[i], before[i], during[i], after[i]);
    if (before[i] == 0 && during[i] == 1 && after[i] == 0) { LOGF("   <<< DIO1"); if (dio1Pin < 0) dio1Pin = CAND[i]; }
    LOGF("\n");
  }
  if (dio1Pin < 0) LOGF("  (no pin tracked the IRQ - DIO1 may be unconnected)\n");

  LOGF("\n=== RESULT ===\n");
  LOGF("  NSS=5 RST=3 SCK=7 MISO=8 MOSI=9\n");
  LOGF("  BUSY = %s\n", busyPin > 0 ? String(busyPin).c_str() : "UNKNOWN");
  LOGF("  DIO1 = %s\n", dio1Pin > 0 ? String(dio1Pin).c_str() : "UNKNOWN / not wired");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
