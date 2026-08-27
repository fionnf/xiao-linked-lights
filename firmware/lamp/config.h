#pragma once
// ============================================================
//  config.h - per-lamp tunables
// ============================================================
// Tuned for 39 LEDs and for a XIAO ESP32-S3 whose pins are mostly consumed
// by the LoRa module.

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
// TTP223 module: it does its own capacitance sensing on-board and just hands
// us a clean digital level on OUT, so no charge-time measurement or discharge
// resistor is needed here - see touch.h. VCC/GND to 3V3/GND, OUT to PIN_TOUCH.
static const int PIN_TOUCH = 44;      // D7, TTP223 OUT
static const bool TOUCH_ACTIVE_HIGH = true;  // flip if your board's jumper is set to active-low
static const uint32_t HOLD_TIME_MS = 3000;  // longer than this is a hold, not a tap
// The TTP223's OUT pin is already digital (0/1), so there is no raw analog
// level to threshold the way the old charge-time pad had one. What we build
// instead: an exponential moving average of recent raw samples, so a chattery
// / oversensitive line still produces a real number between 0 and 1 - mostly-0
// at rest, rising toward 1 the longer and more often it reads touched - and
// TOUCH_THRESHOLD becomes a genuine, tunable line in that number, the same
// role TOUCH_THRESHOLD played for the old sensor. See touch.h.
static const float TOUCH_SMOOTHING = 0.15f;   // EMA alpha: higher = faster to react, less smoothing
static const float TOUCH_THRESHOLD = 0.55f;   // smoothed level above this counts as touched
// Minimum gap between two accepted taps. Independent of the threshold above:
// even a smoothed signal that's genuinely oversensitive (a hand held near the
// module) can cross the threshold repeatedly, so this caps it at one tap per
// window. Raise this if one lamp still fires taps on its own; see CLAUDE.md
// for how board A's sensitivity was diagnosed.
static const uint32_t TAP_COOLDOWN_MS = 700;

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

// Independent colour groups along the strip.
//
// Six groups on 39 LEDs read as noise - too many colour changes in too short a
// run to see any of them as a block. Two or three large bands is the look, so
// the count is now RANDOM per tap between GROUPS_MIN and GROUPS_MAX, which also
// stops every scene having the same rhythm.
//
// MAX_GROUPS only sizes the arrays; the number actually drawn is chosen per
// scene and unused groups are given size 0.
static const int MAX_GROUPS = 4;
static const int GROUPS_MIN = 2;
static const int GROUPS_MAX = 3;
static const int GROUP_MIN_LEDS = 8;
static const int GROUP_MAX_LEDS = 28;

// ---- Fade / breathing ------------------------------------------------------
// Slower than a snap: a tap should breathe into the new scene, not flash to
// it. Fades run at one step per ~16 ms frame, so 180 steps is ~2.9 s.
static const int FADE_STEPS = 180;
static const float BREATHE_SPEED = 0.0008f;
static const float BREATHE_DEPTH = 0.10f;
// Autonomous scene changes are OFF by default. With two lamps each drifting,
// the pair changed roughly every 30 s unprompted, which reads as restless rather
// than alive. Re-enable at runtime with `drift 65` if you want it back.
static const uint32_t IDLE_DRIFT_INTERVAL_S = 0;

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
