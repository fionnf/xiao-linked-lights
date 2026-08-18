#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

#define USB_VID 0x2886
#define USB_PID 0x0059

// I2C is moved to D6/D7; GPIO5/6 are needed by the LoRa header wiring.
static const uint8_t SDA = 43;
static const uint8_t SCL = 44;

// Default SPI is the radio. SS is GPIO5 here, not GPIO41 as on a B2B-wired board.
static const uint8_t MISO = 8;
static const uint8_t SCK = 7;
static const uint8_t MOSI = 9;
static const uint8_t SS = 5;

#endif /* Pins_Arduino_h */
