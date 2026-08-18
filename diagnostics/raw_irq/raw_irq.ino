// raw_irq - ask the SX1262 directly what it is doing, bypassing RadioLib.
//
// Puzzle so far: begin(), setIrqFlags() and startTransmit() all return 0
// (success), but RadioLib's getIrqFlags() stays 0x00000000 forever and TX_DONE
// never appears. Two very different explanations fit:
//
//   (a) the chip really is not transmitting, or
//   (b) it transmits fine and RadioLib's getIrqFlags() is not reading what we
//       think it is.
//
// Telling them apart needs the chip's own registers, read with hand-rolled SPI:
//
//   GetStatus (0xC0)     -> chipMode field: 2=STBY_RC 3=STBY_XOSC 4=FS 5=RX 6=TX
//   GetIrqStatus (0x12)  -> the raw 16-bit IRQ word
//
// Watching chipMode step into TX(6) and back settles (a) on its own, without
// trusting any library.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2, PIN_DIO1 = 4;

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

static bool waitBusy(uint32_t ms) {
  uint32_t t = millis();
  while (digitalRead(PIN_BUSY) == HIGH) { if (millis() - t > ms) return false; delayMicroseconds(50); }
  return true;
}

// Raw command helper. RadioLib owns the SPI settings, so borrow them.
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

static uint8_t chipMode() {
  uint8_t r[1]; rawCmd(0xC0, r, 1);          // GetStatus
  return (r[0] >> 4) & 0x07;
}

static uint16_t irqStatus() {
  uint8_t r[3]; rawCmd(0x12, r, 3);          // GetIrqStatus: status + 2 bytes
  return ((uint16_t)r[1] << 8) | r[2];
}

static const char *modeName(uint8_t m) {
  switch (m) {
    case 2: return "STBY_RC"; case 3: return "STBY_XOSC"; case 4: return "FS";
    case 5: return "RX";      case 6: return "TX";        default: return "?";
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  Serial.println("\n########################################");
  Serial.println("# raw SX1262 IRQ / mode watcher");
  Serial.println("########################################");

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  int st = radio.begin(868.0, 250.0, 11, 5, 0x2B, 22, 8, 1.8, false);
  Serial.printf("begin -> %d\n", st);
  if (st != RADIOLIB_ERR_NONE) { while (true) { Serial.println("INIT FAILED"); delay(3000); } }
  Serial.printf("setDio2AsRfSwitch -> %d\n", radio.setDio2AsRfSwitch(true));
  Serial.printf("setIrqFlags(ALL)  -> %d\n", radio.setIrqFlags(RADIOLIB_SX126X_IRQ_ALL));

  pinMode(PIN_CS, OUTPUT);

  Serial.printf("\nidle: chipMode=%s irqRaw=0x%04X\n", modeName(chipMode()), irqStatus());
}

void loop() {
  Serial.println("\n---- transmit attempt ----");
  const char *payload = "RAWPROBE";
  int st = radio.startTransmit((uint8_t *)payload, strlen(payload));
  Serial.printf("startTransmit -> %d\n", st);

  // Sample fast enough to catch a short transmission (SF11/250k, ~8 bytes is
  // roughly 100 ms on air).
  uint32_t t0 = millis();
  uint8_t lastMode = 0xFF;
  uint16_t lastIrq = 0xFFFF;
  bool sawTx = false, sawTxDone = false;
  while (millis() - t0 < 4000) {
    uint8_t m = chipMode();
    uint16_t irq = irqStatus();
    if (m != lastMode || irq != lastIrq) {
      Serial.printf("  t=%4lu ms  chipMode=%-9s irqRaw=0x%04X%s\n",
                    (unsigned long)(millis() - t0), modeName(m), irq,
                    (irq & 0x0001) ? "  <-- TX_DONE" : "");
      lastMode = m; lastIrq = irq;
    }
    if (m == 6) sawTx = true;
    if (irq & 0x0001) sawTxDone = true;
    delay(2);
  }

  Serial.printf("summary: entered TX mode = %s ; TX_DONE seen = %s\n",
                sawTx ? "YES" : "NO", sawTxDone ? "YES" : "NO");
  if (!sawTx)
    Serial.println("  => chip never entered TX. It is accepting commands but not "
                   "transmitting (clock/TCXO or calibration problem).");
  else if (!sawTxDone)
    Serial.println("  => chip transmitted but TX_DONE never latched.");
  else
    Serial.println("  => transmission completes normally; polling raw IRQ works.");

  radio.standby();
  delay(4000);
}
