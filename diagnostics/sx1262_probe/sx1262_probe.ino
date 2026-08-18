// sx1262_probe - hardware bring-up diagnostic for
// Seeed XIAO ESP32-S3 + Wio-SX1262 LoRa module.
//
// Meshtastic 2.7.26 hangs during setup() on this board: the boot log stops
// partway through hardware init and the node never advertises BLE. This sketch
// exercises the same peripherals Meshtastic touches, but with a hard timeout on
// every step, so a hang becomes a printed failure instead of a silent lockup.
//
// Everything here is deliberately dependency-free - no RadioLib, no Meshtastic -
// so a failure points at the hardware rather than at a library.

#include <Wire.h>
#include <SPI.h>

// Pin map from meshtastic/firmware variants/esp32s3/seeed_xiao_s3/variant.h
static const int PIN_LORA_CS    = 41;
static const int PIN_LORA_DIO1  = 39;
static const int PIN_LORA_RESET = 42;
static const int PIN_LORA_BUSY  = 40;
static const int PIN_LORA_SCK   = 7;
static const int PIN_LORA_MISO  = 8;
static const int PIN_LORA_MOSI  = 9;

static const int PIN_I2C_SDA = 5;
static const int PIN_I2C_SCL = 6;

// SX126x opcodes
static const uint8_t OP_GET_STATUS   = 0xC0;
static const uint8_t OP_READ_REGISTER = 0x1D;
static const uint8_t OP_SET_STANDBY  = 0x80;

// LoRa sync word register - reads 0x1424 on a healthy, freshly reset SX1262.
static const uint16_t REG_LORA_SYNC_WORD = 0x0740;

static SPIClass loraSpi(FSPI);

static void banner(const char *s) {
  Serial.println();
  Serial.print("=== ");
  Serial.print(s);
  Serial.println(" ===");
}

// Wait for BUSY to go low. Returns true on success, false on timeout.
// A permanently-high BUSY is the classic "radio is not answering" signature and
// is exactly where a naive driver would spin forever.
static bool waitBusy(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_LORA_BUSY) == HIGH) {
    if (millis() - start > timeoutMs) return false;
    delay(1);
  }
  return true;
}

static void spiBegin() {
  digitalWrite(PIN_LORA_CS, LOW);
  delayMicroseconds(2);
}

static void spiEnd() {
  delayMicroseconds(2);
  digitalWrite(PIN_LORA_CS, HIGH);
}

void setup() {
  Serial.begin(115200);

  // Native USB CDC: give the host time to enumerate and open the port, otherwise
  // the whole report is written into the void before anyone is listening.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(2000);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);  // XIAO onboard LED is active-low

  Serial.println();
  Serial.println("########################################");
  Serial.println("# XIAO ESP32-S3 + Wio-SX1262 probe");
  Serial.println("########################################");
  Serial.printf("chip      : %s rev %d, %d core(s)\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("flash     : %u bytes\n", (unsigned)ESP.getFlashChipSize());
  Serial.printf("psram     : %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("free heap : %u bytes\n", (unsigned)ESP.getFreeHeap());

  // ---------------------------------------------------------------- I2C ----
  // Meshtastic's boot log stops during its I2C scan, so check the bus can even
  // idle high before scanning it. Both lines are open-drain and must be pulled
  // up; a line stuck low means a jammed bus, which is a real hang candidate.
  banner("I2C bus");
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delay(5);
  int sdaIdle = digitalRead(PIN_I2C_SDA);
  int sclIdle = digitalRead(PIN_I2C_SCL);
  Serial.printf("SDA(gpio%d) idle = %s\n", PIN_I2C_SDA, sdaIdle ? "HIGH (ok)" : "LOW  (STUCK!)");
  Serial.printf("SCL(gpio%d) idle = %s\n", PIN_I2C_SCL, sclIdle ? "HIGH (ok)" : "LOW  (STUCK!)");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  Wire.setTimeOut(50);  // ms - never block forever
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X\n", addr);
      found++;
    }
    delay(2);
  }
  Serial.printf("i2c devices found: %d\n", found);
  Serial.println("i2c scan COMPLETED without hanging");

  // --------------------------------------------------------------- SX1262 --
  banner("SX1262 radio");
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_LORA_RESET, OUTPUT);
  digitalWrite(PIN_LORA_RESET, HIGH);
  pinMode(PIN_LORA_BUSY, INPUT);
  pinMode(PIN_LORA_DIO1, INPUT);

  Serial.printf("BUSY(gpio%d) before reset = %d\n", PIN_LORA_BUSY, digitalRead(PIN_LORA_BUSY));

  loraSpi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
  loraSpi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));

  // Hardware reset: datasheet asks for >100us low.
  digitalWrite(PIN_LORA_RESET, LOW);
  delay(2);
  digitalWrite(PIN_LORA_RESET, HIGH);
  delay(5);

  bool busyOk = waitBusy(1000);
  Serial.printf("BUSY after reset       = %s\n",
                busyOk ? "went LOW (radio responding)" : "STUCK HIGH after 1000ms (NO RADIO)");

  // GetStatus - the cheapest liveness check.
  spiBegin();
  loraSpi.transfer(OP_GET_STATUS);
  uint8_t status = loraSpi.transfer(0x00);
  spiEnd();
  uint8_t chipMode = (status >> 4) & 0x07;
  uint8_t cmdStat  = (status >> 1) & 0x07;
  Serial.printf("GetStatus             = 0x%02X (chipMode=%d cmdStatus=%d)\n",
                status, chipMode, cmdStat);

  waitBusy(100);

  // ReadRegister of the LoRa sync word. On a healthy SX1262 straight out of
  // reset this reads 0x1424. It is a much stronger presence test than
  // GetStatus, because a floating MISO yields 0x00/0xFF rather than 0x1424.
  spiBegin();
  loraSpi.transfer(OP_READ_REGISTER);
  loraSpi.transfer((REG_LORA_SYNC_WORD >> 8) & 0xFF);
  loraSpi.transfer(REG_LORA_SYNC_WORD & 0xFF);
  loraSpi.transfer(0x00);  // NOP while the chip fetches
  uint8_t syncHi = loraSpi.transfer(0x00);
  uint8_t syncLo = loraSpi.transfer(0x00);
  spiEnd();
  uint16_t sync = ((uint16_t)syncHi << 8) | syncLo;
  Serial.printf("SyncWord reg 0x0740   = 0x%04X (expect 0x1424)\n", sync);

  loraSpi.endTransaction();

  banner("VERDICT");
  bool radioPresent = busyOk && (sync == 0x1424);
  if (radioPresent) {
    Serial.println("SX1262 IS PRESENT and responding correctly over SPI.");
  } else if (status == 0x00 || status == 0xFF) {
    Serial.println("SX1262 NOT RESPONDING - SPI reads are all 0x00/0xFF.");
    Serial.println("Module is likely not seated, not powered, or on different pins.");
  } else {
    Serial.println("SX1262 responded but not as expected - check wiring/seating.");
  }
  Serial.println();
  Serial.println("Probe finished. Heartbeat follows; if you see it, nothing hung.");
}

void loop() {
  static uint32_t n = 0;
  digitalWrite(LED_BUILTIN, (n % 2) ? HIGH : LOW);
  Serial.printf("heartbeat %lu  uptime=%lus  heap=%u\n",
                (unsigned long)n++, (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap());
  delay(1000);
}
