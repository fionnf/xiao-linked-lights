// pinmap_hunt - find the real SX1262 pin map by brute force, then prove it.
//
// The pin scan found a genuine SX126x sync word (0x1424) with CS on GPIO5, and
// showed GPIO1-4 being actively driven while the pins Meshtastic expects
// (38,39,40,41,42,48,21) all float. That is the fingerprint of a module wired to
// the XIAO's header pads (D0-D4 + the SPI trio) rather than through the
// board-to-board connector.
//
// Rather than guess which of GPIO1-4 is RESET/BUSY/DIO1, hand every plausible
// combination to RadioLib and let it decide: begin() only returns ERR_NONE when
// the chip really answers, so a success here is a proven pin map, not a theory.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;

// RESET and RF_SW are module inputs (idle high via pull-up); BUSY and DIO1 are
// module outputs (idle low). The survey saw 1 and 3 high, 2 and 4 low, so try
// reset from {1,3,5} and busy/dio1 from {2,4}. CS candidates lead with 5.
static const int CS_CAND[]    = {5, 41, 1, 3};
static const int RST_CAND[]   = {1, 3, 42, 2, 4};
static const int BUSY_CAND[]  = {2, 4, 40, 1, 3};
static const int DIO1_CAND[]  = {4, 2, 39, 3, 1};

static SPIClass spi(FSPI);

static bool tryMap(int cs, int rst, int busy, int dio1, bool tcxo) {
  if (cs == rst || cs == busy || cs == dio1) return false;
  if (rst == busy || rst == dio1 || busy == dio1) return false;

  SX1262 radio = new Module(cs, dio1, rst, busy, spi);
  int st = radio.begin(868.0, 250.0, 11, 5, 0x2B, 22, 8, tcxo ? 1.8 : 0.0, false);
  if (st == RADIOLIB_ERR_NONE) {
    Serial.printf("\n  *** SUCCESS  cs=%d rst=%d busy=%d dio1=%d tcxo=%s ***\n",
                  cs, rst, busy, dio1, tcxo ? "1.8V" : "none");
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(2000);

  Serial.println("\n########################################");
  Serial.println("# SX1262 pin map hunt");
  Serial.println("########################################");
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  int tried = 0, found = 0;
  for (int t = 0; t < 2 && !found; t++) {          // TCXO first, then plain XTAL
    bool tcxo = (t == 0);
    Serial.printf("\n--- sweeping with tcxo=%s ---\n", tcxo ? "1.8V" : "none");
    for (unsigned a = 0; a < sizeof(CS_CAND)/sizeof(int) && !found; a++)
    for (unsigned b = 0; b < sizeof(RST_CAND)/sizeof(int) && !found; b++)
    for (unsigned c = 0; c < sizeof(BUSY_CAND)/sizeof(int) && !found; c++)
    for (unsigned d = 0; d < sizeof(DIO1_CAND)/sizeof(int) && !found; d++) {
      tried++;
      if (tryMap(CS_CAND[a], RST_CAND[b], BUSY_CAND[c], DIO1_CAND[d], tcxo))
        found = 1;
    }
  }

  Serial.printf("\ncombinations tried: %d\n", tried);
  if (!found)
    Serial.println("No working pin map found. The chip answers SPI but no "
                   "reset/busy combination completes init.");
  Serial.println("=== HUNT COMPLETE ===");
}

void loop() { Serial.println("heartbeat"); delay(3000); }
