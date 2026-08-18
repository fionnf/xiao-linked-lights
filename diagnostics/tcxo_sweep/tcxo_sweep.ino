// tcxo_sweep - find the TCXO setting that actually lets this module transmit.
//
// Established by diagnostics/raw_irq: the SX1262 accepts every command
// (begin/setIrqFlags/startTransmit all return 0) but stays in STBY_RC and never
// enters TX. Reaching TX requires the crystal oscillator to start; staying in
// STBY_RC is the classic signature of XOSC_START_ERR - i.e. the TCXO is not
// being supplied the voltage it expects.
//
// The 1.8 V we have been using came from Meshtastic's seeed_xiao_s3 variant,
// which is written for a DIFFERENT board (its pins are wrong for ours too), so
// there is no reason to trust its TCXO voltage either.
//
// So: try every voltage the SX1262 supports, plus the no-TCXO case, and for each
// one ask the chip two questions that cannot be fudged -
//   GetDeviceErrors (0x17) -> does XOSC_START_ERR / PLL_LOCK_ERR come up?
//   GetStatus       (0xC0) -> does chipMode actually reach TX(6)?

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2, PIN_DIO1 = 4;

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static const float TCXO_OPTIONS[] = {0.0, 1.6, 1.7, 1.8, 2.2, 2.4, 2.7, 3.0, 3.3};
static const int N_TCXO = sizeof(TCXO_OPTIONS) / sizeof(float);

static bool waitBusy(uint32_t ms) {
  uint32_t t = millis();
  while (digitalRead(PIN_BUSY) == HIGH) { if (millis() - t > ms) return false; delayMicroseconds(50); }
  return true;
}

static void rawCmd(uint8_t opcode, uint8_t *rx, int nRx) {
  waitBusy(100);
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(5);
  spi.transfer(opcode);
  for (int i = 0; i < nRx; i++) rx[i] = spi.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
}

static uint8_t chipMode() { uint8_t r[1]; rawCmd(0xC0, r, 1); return (r[0] >> 4) & 0x07; }

// GetDeviceErrors - the chip's own account of what went wrong during calibration.
static uint16_t deviceErrors() { uint8_t r[3]; rawCmd(0x17, r, 3); return ((uint16_t)r[1] << 8) | r[2]; }
static uint16_t irqStatus()    { uint8_t r[3]; rawCmd(0x12, r, 3); return ((uint16_t)r[1] << 8) | r[2]; }

static void printErrors(uint16_t e) {
  if (!e) { Serial.print("none"); return; }
  if (e & (1 << 0)) Serial.print("RC64K_CALIB ");
  if (e & (1 << 1)) Serial.print("RC13M_CALIB ");
  if (e & (1 << 2)) Serial.print("PLL_CALIB ");
  if (e & (1 << 3)) Serial.print("ADC_CALIB ");
  if (e & (1 << 4)) Serial.print("IMG_CALIB ");
  if (e & (1 << 5)) Serial.print("XOSC_START ");   // the one we expect
  if (e & (1 << 6)) Serial.print("PLL_LOCK ");
  if (e & (1 << 8)) Serial.print("PA_RAMP ");
}

static bool tryTcxo(float volts) {
  Serial.printf("\n--- TCXO = %.1f V %s---\n", volts, volts == 0.0 ? "(plain XTAL) " : "");

  int st = radio.begin(868.0, 250.0, 11, 5, 0x2B, 22, 8, volts, false);
  Serial.printf("  begin -> %d\n", st);
  if (st != RADIOLIB_ERR_NONE) return false;

  radio.setDio2AsRfSwitch(true);
  radio.setIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  pinMode(PIN_CS, OUTPUT);

  uint16_t errs = deviceErrors();
  Serial.print("  device errors after init: "); printErrors(errs); Serial.printf("  (0x%04X)\n", errs);

  const char *payload = "TCXOTEST";
  st = radio.startTransmit((uint8_t *)payload, strlen(payload));

  bool sawTx = false, sawDone = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 2500) {
    uint8_t m = chipMode();
    if (m == 6) sawTx = true;
    if (irqStatus() & 0x0001) { sawDone = true; break; }
    delay(2);
  }
  errs = deviceErrors();
  Serial.printf("  startTransmit=%d  reachedTX=%s  TX_DONE=%s  errors=",
                st, sawTx ? "YES" : "no", sawDone ? "YES" : "no");
  printErrors(errs); Serial.println();

  radio.standby();
  return sawDone;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  Serial.println("\n########################################");
  Serial.println("# TCXO voltage sweep");
  Serial.println("########################################");
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  float winner = -1;
  for (int i = 0; i < N_TCXO; i++) {
    if (tryTcxo(TCXO_OPTIONS[i])) { winner = TCXO_OPTIONS[i]; break; }
  }

  Serial.println("\n=== RESULT ===");
  if (winner < 0)
    Serial.println("No TCXO setting produced a completed transmission.");
  else
    Serial.printf("*** WORKING TCXO SETTING: %.1f V ***\n", winner);
  Serial.println("=== DONE ===");
}

void loop() { delay(10000); Serial.println("(sweep finished - see RESULT above)"); }
