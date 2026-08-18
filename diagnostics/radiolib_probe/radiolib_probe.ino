// radiolib_probe - second opinion on whether the SX1262 is really there.
//
// The hand-rolled SPI probe in ../sx1262_probe read back all-zeroes. Rather than
// trust that, this repeats the test with RadioLib - the same driver Meshtastic
// uses - with the exact pin map from
// meshtastic/firmware variants/esp32s3/seeed_xiao_s3/variant.h @ v2.7.26.
//
// RadioLib::begin() returns RADIOLIB_ERR_CHIP_NOT_FOUND (-2) when SPI reads do
// not match the expected SX126x signature, which is the specific answer we want.

#include <RadioLib.h>

// variant.h: LORA_SCK 7, LORA_MISO 8, LORA_MOSI 9, LORA_CS 41,
//            SX126X_BUSY 40, LORA_DIO1 39, LORA_RESET 42,
//            SX126X_DIO3_TCXO_VOLTAGE 1.8, SX126X_DIO2_AS_RF_SWITCH, SX126X_RXEN 38
static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 41, PIN_DIO1 = 39, PIN_RST = 42, PIN_BUSY = 40;

static SPIClass loraSpi(FSPI);
static SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, loraSpi);

static void report(const char *what, int st) {
  Serial.printf("%-28s = %d", what, st);
  switch (st) {
    case RADIOLIB_ERR_NONE:            Serial.print("  (SUCCESS)"); break;
    case RADIOLIB_ERR_CHIP_NOT_FOUND:  Serial.print("  (CHIP NOT FOUND - SPI signature mismatch)"); break;
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT: Serial.print("  (SPI CMD TIMEOUT - BUSY never released)"); break;
    case RADIOLIB_ERR_SPI_CMD_INVALID: Serial.print("  (SPI CMD INVALID)"); break;
    case RADIOLIB_ERR_SPI_CMD_FAILED:  Serial.print("  (SPI CMD FAILED)"); break;
    default: break;
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(2000);

  Serial.println();
  Serial.println("########################################");
  Serial.println("# RadioLib SX1262 second-opinion probe");
  Serial.println("########################################");
  Serial.printf("pins: sck=%d miso=%d mosi=%d cs=%d busy=%d dio1=%d rst=%d\n",
                PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS, PIN_BUSY, PIN_DIO1, PIN_RST);

  loraSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  // Attempt 1: the way Meshtastic configures this board - 1.8V TCXO on DIO3.
  Serial.println("\n-- attempt 1: 868.0 MHz, TCXO 1.8V (Meshtastic's config) --");
  int st = radio.begin(868.0, 250.0, 11, 5, 0x2B, 22, 8, 1.8, false);
  report("begin(tcxo=1.8)", st);

  // Attempt 2: assume a plain crystal instead of a TCXO. If the module were
  // fitted with an XTAL rather than a TCXO, attempt 1 fails and this succeeds -
  // a completely different fault to "no chip at all".
  if (st != RADIOLIB_ERR_NONE) {
    Serial.println("\n-- attempt 2: same, but tcxoVoltage=0 (plain XTAL) --");
    st = radio.begin(868.0, 250.0, 11, 5, 0x2B, 22, 8, 0.0, false);
    report("begin(tcxo=0)", st);
  }

  if (st == RADIOLIB_ERR_NONE) {
    Serial.println("\n*** SX1262 FOUND AND INITIALISED ***");
    float rssi = radio.getRSSI();
    Serial.printf("random()=%u  RSSI=%.1f dBm\n", (unsigned)radio.random(1000), rssi);
  } else {
    Serial.println("\n*** SX1262 DID NOT INITIALISE ***");
    Serial.println("Both TCXO and XTAL configurations failed.");
  }

  Serial.println("\nProbe finished; heartbeat follows.");
}

void loop() {
  static uint32_t n = 0;
  Serial.printf("heartbeat %lu\n", (unsigned long)n++);
  delay(2000);
}
