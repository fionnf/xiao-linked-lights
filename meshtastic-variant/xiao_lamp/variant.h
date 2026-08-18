/*
 * xiao_lamp - Seeed XIAO ESP32-S3 + Wio-SX1262, wired through the module's
 * 1x7 breakout headers rather than its 30-pin board-to-board connector.
 *
 * This exists because the stock `seeed_xiao_s3` variant cannot drive this
 * hardware: it expects the radio on GPIO 41/42/40/39, which reach the module
 * only through the B2B connector. On a header-wired build those pins are
 * floating, so Meshtastic finds no radio, never advertises BLE, and never
 * answers the serial API - with no error message anywhere.
 *
 * Every pin below was measured, not copied:
 *   - NSS  proven by gating: held HIGH the chip stops answering entirely
 *   - RST  proven by writing a marker to register 0x0740 and pulsing each
 *          candidate; only GPIO3 restored the documented default 0x1424
 *   - BUSY proven by holding HIGH for the whole Calibrate(0x7F) window
 *   - DIO1 proven by tracking the TX_DONE IRQ (0 -> 1 on latch, -> 0 on clear)
 *
 * Verified working: 16/16 packets between two boards, RSSI -17 dBm, SNR +11 dB,
 * TX_DONE at 91 ms (correct time-on-air for SF9/BW250).
 */

#define LED_POWER 48    // green LED on the Wio-SX1262 carrier
#define LED_STATE_ON 1  // active high

#define BUTTON_PIN 21   // K1 (TS-1185E) on the carrier, 10K pull-up
#define BUTTON_NEED_PULLUP

#define BATTERY_PIN -1
#define ADC_CHANNEL ADC1_GPIO1_CHANNEL
#define BATTERY_SENSE_RESOLUTION_BITS 12

/*
 * I2C is moved off GPIO5/6. The stock variant puts SDA on GPIO5, which is the
 * LoRa chip-select here - leaving it there makes the radio and the I2C scan
 * fight over the same pin. D6/D7 are free on the header and carry nothing.
 */
#define I2C_SDA 43  // D6
#define I2C_SCL 44  // D7

/*
 * No GPS and no screen on this build. The stock variant enables an L76K GPS on
 * GPIO43/44 and an SSD1306; neither is fitted, and the GPS standby pin (GPIO1)
 * would collide with a line on the LoRa header.
 */

#define USE_SX1262

#define LORA_SCK 7   // D8
#define LORA_MISO 8  // D9
#define LORA_MOSI 9  // D10
#define LORA_CS 5    // D4
#define LORA_RESET 3 // D2  <-- the pin that made the radio work at all
#define LORA_DIO1 2  // D1

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY 4 // D3
#define SX126X_RESET LORA_RESET

/*
 * The module switches TX/RX from the SX1262's own DIO2 line (per the Wio-SX1262
 * datasheet), so there is no external RXEN/TXEN to drive - unlike the stock
 * variant, which sets SX126X_RXEN to GPIO38 on the B2B connector.
 */
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
