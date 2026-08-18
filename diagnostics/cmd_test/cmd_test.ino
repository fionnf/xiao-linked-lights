// cmd_test - do write commands actually execute, or only reads work?
//
// Where the evidence now stands, after several wrong turns:
//   * The chip is genuine: register 0x0320 reads ASCII "SX126".
//   * SPI is solid in MODE0 - 40/40 identical reads from 100 kHz to 8 MHz.
//   * Framing is standard: opcode, address, one dummy byte, then data.
//   * GetDeviceErrors is clean (0x0000) and GetStatus reports STBY_RC.
//   * Yet SetStandby(XOSC), Calibrate and SetTx never change the mode.
//
// Two possibilities remain, and they need completely different fixes:
//   (A) write commands are not executing at all (bus/protocol problem), or
//   (B) writes work fine and only the oscillator refuses to start (hardware).
//
// A register write followed by a read-back separates them in one step, with no
// radio or oscillator involved: WriteRegister is pure digital logic. If the value
// comes back changed, commands execute and the fault is the oscillator. If it
// does not, nothing we send is landing.

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
  if (!waitBusy(100)) LOGF("   [!] BUSY stuck high\n");
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(10);
  for (int i = 0; i < n; i++) {
    uint8_t v = spi.transfer(tx ? tx[i] : 0x00);
    if (rx) rx[i] = v;
  }
  delayMicroseconds(10);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
}

static void cmd(uint8_t op, const uint8_t *args, int n) {
  uint8_t b[16]; b[0] = op;
  for (int i = 0; i < n; i++) b[1 + i] = args[i];
  xfer(b, nullptr, 1 + n);
}

// ReadRegister: 1D, addrHi, addrLo, dummy, data...  (framing confirmed by spi_dump)
static void readReg(uint16_t addr, uint8_t *out, int n) {
  uint8_t tx[24] = {0}, rx[24];
  tx[0] = 0x1D; tx[1] = addr >> 8; tx[2] = addr & 0xFF;
  int total = 4 + n;
  xfer(tx, rx, total);
  for (int i = 0; i < n; i++) out[i] = rx[4 + i];
}

// WriteRegister: 0D, addrHi, addrLo, data...
static void writeReg(uint16_t addr, const uint8_t *data, int n) {
  uint8_t tx[24];
  tx[0] = 0x0D; tx[1] = addr >> 8; tx[2] = addr & 0xFF;
  for (int i = 0; i < n; i++) tx[3 + i] = data[i];
  xfer(tx, nullptr, 3 + n);
}

static uint8_t statusByte() { uint8_t tx[2] = {0xC0, 0}, rx[2]; xfer(tx, rx, 2); return rx[1]; }
static uint8_t chipMode()   { return (statusByte() >> 4) & 0x07; }
static uint16_t devErrors() { uint8_t tx[4] = {0x17,0,0,0}, rx[4]; xfer(tx, rx, 4); return ((uint16_t)rx[2] << 8) | rx[3]; }
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
  LOGF("# do write commands execute?\n");
  LOGF("########################################\n");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);

  uint8_t v[8];
  readReg(0x0320, v, 6); v[6] = 0;
  LOGF("version register 0x0320 = \"%s\"\n", (char *)v);

  // ---------- test 1: register write / read-back ----------
  LOGF("\n--- test 1: WriteRegister round trip (sync word 0x0740) ---\n");
  uint8_t orig[2]; readReg(0x0740, orig, 2);
  LOGF("  before      : %02X %02X\n", orig[0], orig[1]);
  uint8_t probe[2] = {0xAB, 0xCD};
  writeReg(0x0740, probe, 2);
  uint8_t after[2]; readReg(0x0740, after, 2);
  LOGF("  wrote AB CD : %02X %02X  -> %s\n", after[0], after[1],
       (after[0] == 0xAB && after[1] == 0xCD) ? "WRITE WORKED" : "write did NOT take");
  bool writesWork = (after[0] == 0xAB && after[1] == 0xCD);
  writeReg(0x0740, orig, 2);       // put it back

  // ---------- test 2: SetPacketType then read it back ----------
  LOGF("\n--- test 2: SetPacketType(LORA) then GetPacketType ---\n");
  uint8_t a[8];
  a[0] = 0x01; cmd(0x8A, a, 1);                        // SetPacketType(LORA)
  uint8_t tx2[3] = {0x11, 0, 0}, rx2[3];
  xfer(tx2, rx2, 3);                                   // GetPacketType
  LOGF("  GetPacketType -> 0x%02X (1 = LORA) -> %s\n", rx2[2],
       rx2[2] == 0x01 ? "COMMAND EXECUTED" : "command ignored");
  bool cmdsWork = (rx2[2] == 0x01);

  // ---------- test 3: the oscillator, with generous timing ----------
  LOGF("\n--- test 3: oscillator ---\n");
  a[0] = 0x00; a[1] = 0x00; cmd(0x07, a, 2);           // ClearDeviceErrors
  a[0] = 0x00; cmd(0x80, a, 1); delay(10);            // SetStandby(STBY_RC)
  LOGF("  after SetStandby(RC)   mode=%-9s errs=0x%04X\n", mn(chipMode()), devErrors());

  a[0] = 0x01; cmd(0x80, a, 1);                        // SetStandby(STBY_XOSC)
  delay(100);
  LOGF("  after SetStandby(XOSC) mode=%-9s errs=0x%04X\n", mn(chipMode()), devErrors());
  bool xoscBare = (chipMode() == 3);

  // TCXO route: DIO3 at 1.8 V with a generous 64 ms startup timeout
  a[0] = 0x00; cmd(0x80, a, 1); delay(10);
  a[0] = 0x02; a[1] = 0x00; a[2] = 0x10; a[3] = 0x00;  // 1.8V, timeout 0x001000*15.625us = 64ms
  cmd(0x97, a, 4); delay(5);
  a[0] = 0x7F; cmd(0x89, a, 1); delay(100);            // Calibrate(all)
  LOGF("  after TCXO+Calibrate   mode=%-9s errs=0x%04X\n", mn(chipMode()), devErrors());
  a[0] = 0x01; cmd(0x80, a, 1); delay(150);
  LOGF("  after SetStandby(XOSC) mode=%-9s errs=0x%04X\n", mn(chipMode()), devErrors());
  bool xoscTcxo = (chipMode() == 3);

  LOGF("\n=== RESULT ===\n");
  LOGF("  register writes execute : %s\n", writesWork ? "YES" : "NO");
  LOGF("  commands execute        : %s\n", cmdsWork ? "YES" : "NO");
  LOGF("  STBY_XOSC (no tcxo)     : %s\n", xoscBare ? "YES" : "no");
  LOGF("  STBY_XOSC (with tcxo)   : %s\n", xoscTcxo ? "YES" : "no");
  if (writesWork && cmdsWork && !xoscBare && !xoscTcxo) {
    LOGF("\n  DIAGNOSIS: the SPI bus and command path arefine - the chip\n");
    LOGF("  executes everything we send. Only the crystal oscillator never\n");
    LOGF("  starts, so the radio can never reach FS/TX/RX.\n");
    LOGF("  That is a hardware fault in the module's clock, NOT firmware.\n");
  } else if (!writesWork || !cmdsWork) {
    LOGF("\n  DIAGNOSIS: commands are not landing. Protocol/bus problem.\n");
  } else {
    LOGF("\n  Oscillator starts - radio should now be usable.\n");
  }
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
