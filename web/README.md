# Browser control panel

Single-page Web Serial app for the lamps. It speaks the **same line-based command
protocol** the serial console uses (`tap`, `power`, `bright 0.4`, ...), so nothing
on the device had to change - the browser is just another terminal with buttons.

    cd web && python3 -m http.server 8765
    open http://localhost:8765/

Chrome, Edge or Arc only. Safari and Firefox do not implement Web Serial.

**Close any serial monitor first.** A port can only be held by one program, and
`arduino-cli monitor` will keep the browser from opening it.

Each lamp is a separate connection: press Connect on both panels and choose a
different port each time. The on-screen strip is an approximate preview rendered
from the same seed the firmware used - useful for seeing that both lamps agree,
and for watching the lamp that has no LEDs attached yet.
