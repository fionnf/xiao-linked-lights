// spi_dump - stop guessing the SPI framing and just look at the bytes.
//
// Corrected understanding: 0x24B4 came back 40/40 at every clock from 100 kHz to
// 8 MHz. That is rock-solid deterministic, so the link is NOT flaky - my earlier
// "byte slip" reading was wrong. What is actually happening is that the reply
// sits one byte away from where my code looks for it, consistently.
//
// Rather than argue about how many NOP bytes SX126x ReadRegister needs, dump the
// entire transaction: clock out the opcode plus a long tail of NOPs and print
// every byte that comes back. The position of 0x14 0x24 (the LoRa sync word) in
// that dump defines the correct framing, with no assumptions.
//
// Also sweep the four SPI modes. If the reply only appears in one of them, the
// clock polarity/phase was the real problem and the "ignored commands" follow
// directly: a command shifted by a bit or a byte is a command the chip cannot
// execute.

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

static void waitBusy() {
  uint32_t t = millis();
  while (digitalRead(PIN_BUSY) == HIGH && millis() - t < 50) delayMicroseconds(20);
}

// Clock out `txLen` given bytes then `tail` NOPs, printing everything received.
static void dump(const char *label, const uint8_t *tx, int txLen, int tail, uint8_t mode) {
  uint8_t rx[20];
  int n = txLen + tail;
  if (n > 20) n = 20;
  waitBusy();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, mode));
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(10);
  for (int i = 0; i < n; i++) rx[i] = spi.transfer(i < txLen ? tx[i] : 0x00);
  delayMicroseconds(10);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();

  LOGF("  %-22s tx:", label);
  for (int i = 0; i < txLen; i++) LOGF(" %02X", tx[i]);
  LOGF("  rx:");
  for (int i = 0; i < n; i++) LOGF(" %s%02X", i == txLen ? "|" : "", rx[i]);
  LOGF("\n");
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  LOGF("\n########################################\n");
  LOGF("# raw SPI transaction dump\n");
  LOGF("########################################\n");
  LOGF("looking for the sync word bytes 14 24 in the rx stream.\n");
  LOGF("'|' marks where our transmitted bytes end and NOPs begin.\n");

  pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  digitalWrite(PIN_RST, LOW); delay(5); digitalWrite(PIN_RST, HIGH); delay(50);
  LOGF("BUSY after reset = %d\n", digitalRead(PIN_BUSY));

  const uint8_t rdSync[3] = {0x1D, 0x07, 0x40};   // ReadRegister(0x0740)
  const uint8_t getSt[1]  = {0xC0};               // GetStatus
  const uint8_t getErr[1] = {0x17};               // GetDeviceErrors
  const uint8_t rdVer[3]  = {0x1D, 0x03, 0x20};   // 0x0320: undocumented but stable

  for (uint8_t mode = 0; mode < 4; mode++) {
    LOGF("\n--- SPI_MODE%d ---\n", mode);
    dump("ReadReg 0x0740", rdSync, 3, 8, mode);
    dump("GetStatus", getSt, 1, 5, mode);
    dump("GetDeviceErrors", getErr, 1, 5, mode);
    dump("ReadReg 0x0320", rdVer, 3, 6, mode);
  }

  LOGF("\n=== INTERPRETATION ===\n");
  LOGF("If '14 24' appears in MODE0 right after the 3 tx bytes, the correct\n");
  LOGF("framing has NO dummy byte. If it appears one later, one dummy is needed.\n");
  LOGF("If it only shows up in a mode other than 0, the SPI mode was wrong all\n");
  LOGF("along - which would explain why every write command was ignored.\n");
  LOGF("=== DONE ===\n");
}

void loop() {
  delay(5000);
  Serial.println("\n===== REPORT (repeating) =====");
  Serial.print(LOG);
}
