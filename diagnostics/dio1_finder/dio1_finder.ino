// dio1_finder - work out where DIO1 actually goes, using ONE board.
//
// Evidence so far says our DIO1 guess is wrong:
//   * interrupt-driven RX retriggered endlessly with empty payloads
//   * polled RX never saw RX_DONE
//   * RadioLib receive() now returns -705 (SPI_CMD_TIMEOUT, BUSY never released)
// while begin() succeeds every time - so SPI, CS, RESET and BUSY are right and
// only the IRQ line is in doubt.
//
// Trick: DIO1 does not need a second radio to assert. Mask the SX1262's IRQs so
// that ONLY TxDone maps to DIO1, transmit a packet, and then look at every
// candidate GPIO. Whichever pin is HIGH after TxDone and returns LOW after the
// IRQ is cleared IS DIO1. If no pin moves, DIO1 simply is not wired to the MCU,
// and everything must be polled over SPI - which is worth knowing for certain.

#include <RadioLib.h>

static const int PIN_SCK = 7, PIN_MISO = 8, PIN_MOSI = 9;
static const int PIN_CS = 5, PIN_RST = 1, PIN_BUSY = 2;
static const int PIN_DIO1_GUESS = 4;

// Every pin that could plausibly carry DIO1. GPIO2 (BUSY) and 5/7/8/9 (SPI) are
// already accounted for but are listed so their behaviour shows up for contrast.
static const int WATCH[] = {1, 2, 3, 4, 6, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                            21, 38, 39, 40, 41, 42, 43, 44, 47, 48};
static const int N_WATCH = sizeof(WATCH) / sizeof(WATCH[0]);

static SPIClass spi(FSPI);
static SX1262 radio = new Module(PIN_CS, PIN_DIO1_GUESS, PIN_RST, PIN_BUSY, spi);

// Kept at file scope so loop() can reprint the verdict forever. The USB CDC
// enumerates a moment after boot, so anything printed only once in setup() is
// easily missed by a host that is still opening the port.
static int before[N_WATCH], after[N_WATCH], cleared[N_WATCH];
static uint32_t txIrqFlags = 0;
static int beginStatus = 0;
static int txStartStatus = 0;
static int irqMaskStatus = 0;

static void snapshot(const char *label, int *dest) {
  Serial.printf("\n%s\n  ", label);
  for (int i = 0; i < N_WATCH; i++) {
    pinMode(WATCH[i], INPUT);
    dest[i] = digitalRead(WATCH[i]);
    Serial.printf("%d:%d ", WATCH[i], dest[i]);
  }
  Serial.println();
}

static void printVerdict();

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(1500);

  Serial.println("\n########################################");
  Serial.println("# DIO1 finder");
  Serial.println("########################################");

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  // Use the exact parameters pinmap_hunt succeeds with; a different
  // frequency/power combination left the radio wedged once before.
  beginStatus = radio.begin(868.0, 250.0, 11, 5, 0x2B, 22, 8, 1.8, false);
  int st = beginStatus;
  Serial.printf("begin -> %d\n", st);
  if (st != RADIOLIB_ERR_NONE) { while (true) { Serial.println("INIT FAILED"); delay(3000); } }
  radio.setDio2AsRfSwitch(true);

  // On the SX126x the IRQ MASK gates which events are recorded in the IRQ status
  // register at all - an event outside the mask never shows up, no matter how
  // often you poll. getIrqFlags() returning a flat 0x00000000 after a
  // transmission is exactly that symptom, and it would equally explain why the
  // polled receive loop never saw RX_DONE. Enable everything.
  irqMaskStatus = radio.setIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  Serial.printf("setIrqFlags(ALL) -> %d\n", irqMaskStatus);


  radio.standby();
  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  snapshot("[1] idle, all IRQs cleared:", before);

  // startTransmit() is the non-blocking form: it kicks off the transmission and
  // returns immediately, WITHOUT clearing the IRQ afterwards the way transmit()
  // does. That leaves the TxDone flag - and therefore the DIO1 line - asserted
  // for us to catch.
  const char *payload = "DIO1PROBE";
  txStartStatus = radio.startTransmit((uint8_t *)payload, strlen(payload));
  st = txStartStatus;
  Serial.printf("\nstartTransmit -> %d\n", st);

  uint32_t waitStart = millis();
  uint32_t flags = 0;
  while (millis() - waitStart < 5000) {
    flags = radio.getIrqFlags();
    if (flags & RADIOLIB_SX126X_IRQ_TX_DONE) break;
    delay(1);
  }
  txIrqFlags = flags;
  Serial.printf("irq flags after TX = 0x%08lX (TX_DONE bit = 0x%X) after %lu ms\n",
                (unsigned long)flags, RADIOLIB_SX126X_IRQ_TX_DONE,
                (unsigned long)(millis() - waitStart));

  snapshot("[2] immediately after TxDone (IRQ still set):", after);

  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  delay(5);
  snapshot("[3] after clearing IRQs:", cleared);

  printVerdict();
}

static void printVerdict() {
  Serial.println("\n=== ANALYSIS ===");
  Serial.printf("begin=%d irqMask=%d startTransmit=%d irqFlagsAfterTx=0x%08lX\n",
                beginStatus, irqMaskStatus, txStartStatus, (unsigned long)txIrqFlags);
  int found = 0;
  for (int i = 0; i < N_WATCH; i++) {
    if (after[i] == 1 && before[i] == 0 && cleared[i] == 0) {
      Serial.printf("  GPIO%d went HIGH on TxDone and LOW when cleared  <<< THIS IS DIO1\n",
                    WATCH[i]);
      found++;
    } else if (after[i] != before[i]) {
      Serial.printf("  GPIO%d changed (%d -> %d -> %d) - suspicious, not a clean match\n",
                    WATCH[i], before[i], after[i], cleared[i]);
    }
  }
  if (!found) {
    Serial.println("  No pin tracked the TxDone IRQ.");
    Serial.println("  DIO1 is NOT connected to the MCU on this board.");
    Serial.println("  => all radio events must be polled over SPI.");
  }
  Serial.println("=== DONE ===");
}

void loop() { delay(5000); printVerdict(); }
