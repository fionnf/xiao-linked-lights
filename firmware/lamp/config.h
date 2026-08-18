#pragma once
// ============================================================
//  config.h - per-lamp tunables
// ============================================================
// Ported from config.py of the Pico/MQTT version, retuned for 39 LEDs and
// for a XIAO ESP32-S3 whose pins are mostly consumed by the LoRa module.

// ---- LED strip -------------------------------------------------------------
// SK6812 RGBW, GRBW byte order, data in at the DIN end.
// D6 and D7 are the only header pads the radio does not use. D0/D5 are left
// alone deliberately: one of them is RF_SW1 on the LoRa breakout header, and
// driving an 800 kHz data signal onto the antenna-switch line would be a poor
// way to find out which.
static const int PIN_LED_DATA = 43;   // D6, via 330R to DIN
static const int NUM_LEDS = 39;
static const float LED_BRIGHTNESS = 0.6f;   // global cap; also keeps current sane
static const bool REVERSE_LEDS = false;     // true if this lamp joins at the DOUT end

// ---- Capacitive touch ------------------------------------------------------
// Charge-time method, exactly as on the Pico: drive high, release, count the
// discharge through 1M to GND. The ESP32-S3's hardware touch peripheral only
// covers GPIO1-14, all of which the radio has taken, so the software method is
// not a compromise here - it is the only option, and it is the one already
// proven on this strip.
static const int PIN_TOUCH = 44;      // D7, pad + 1M to GND
static const float TOUCH_THRESHOLD = 1.6f;  // multiple of baseline that counts as a touch
static const uint32_t HOLD_TIME_MS = 3000;  // longer than this is a hold, not a tap

// ---- Colour ----------------------------------------------------------------
// Warm white base; the W channel carries the warmth and RGB adds the tint.
static const uint8_t BASE_WARM_WHITE[4] = {0, 0, 0, 200};

// Hue tints blended on top of the warm white. Adjacent entries interpolate, so
// a continuous 0..1 "position" sweeps smoothly through all of them.
static const uint8_t TINT_PALETTE[][3] = {
    {255, 200,  80}, {255, 160,   0}, {255, 120,   0}, {255,  60,   0},
    {255,   0,   0}, {255,   0,  60}, {255,   0, 140}, {200,   0, 200},
    {140,   0, 255}, { 80,   0, 255}, {  0,   0, 255}, {  0,  60, 255},
    {  0, 140, 255}, {  0, 200, 255}, {  0, 255, 220}, {  0, 255, 160},
    {  0, 255,  80}, {  0, 220,   0}, { 80, 255,   0}, {160, 255,   0},
    {220, 255,   0}, {255, 240,   0}, {255, 180,  40}, {255, 100,  80},
    {255,  80, 160}, {180,  40, 255}, { 40, 100, 255}, {  0, 180, 180},
    { 20, 255, 120}, {255, 220, 120},
};
static const int TINT_COUNT = sizeof(TINT_PALETTE) / sizeof(TINT_PALETTE[0]);

// Independent colour groups along the strip. Scaled up from the 10-LED build:
// 3 groups of up to 8 would cover a fifth of a 39-LED strip and read as one
// blob, so both the count and the size range grow with the strip.
static const int NUM_GROUPS = 6;
static const int GROUP_MIN_LEDS = 2;
static const int GROUP_MAX_LEDS = 12;

// ---- Fade / breathing ------------------------------------------------------
static const int FADE_STEPS = 60;            // ~1 s at 60 fps
static const float BREATHE_SPEED = 0.0008f;
static const float BREATHE_DEPTH = 0.10f;
static const uint32_t IDLE_DRIFT_INTERVAL_S = 65;

// ---- Radio -----------------------------------------------------------------
// Measured pin map for the header-wired Wio-SX1262. See CLAUDE.md section 9;
// RST on GPIO3 is the discovery that made the radio work at all.
static const int PIN_LORA_SCK = 7, PIN_LORA_MISO = 8, PIN_LORA_MOSI = 9;
static const int PIN_LORA_NSS = 5, PIN_LORA_RST = 3;
static const int PIN_LORA_BUSY = 4, PIN_LORA_DIO1 = 2;

static const float LORA_FREQ_MHZ = 868.0f;
static const float LORA_BW_KHZ = 250.0f;
static const int LORA_SF = 9;
static const int LORA_CR = 5;
static const int LORA_TX_DBM = 14;     // EU 868 ERP limit
static const int LORA_PREAMBLE = 8;
static const float LORA_TCXO_V = 1.8f;
