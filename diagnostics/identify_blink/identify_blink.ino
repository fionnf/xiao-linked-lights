// identify_blink - tell the two physically identical boards apart.
//
// Board B (MAC cc:ba:97:16:d1:44) is healthy and flashable; board A
// (cc:ba:97:16:d1:30) has a corrupted image and needs manual BOOT recovery.
// They look the same on the desk, so make the healthy one announce itself:
// a fast, obvious blink. The board that is NOT blinking is board A.

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
}

void loop() {
  // XIAO's onboard LED is active-LOW. Double-blink then pause, so it reads as a
  // deliberate signal rather than a board glitching.
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_BUILTIN, LOW);  delay(120);
    digitalWrite(LED_BUILTIN, HIGH); delay(120);
  }
  delay(600);
  Serial.println("I am BOARD B (cc:ba:97:16:d1:44) - the healthy one");
}
