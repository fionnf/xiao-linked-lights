# Browser control panel

Two pages, for the two firmware builds the lamps dual-boot between (see
CLAUDE.md section 13). Pick the one that matches whichever build is currently
running - `esptool ... read-mac` or the USB product name tells you which.

## `index.html` - for the Meshtastic build (the default)

Talks Bluetooth Low Energy directly to Meshtastic's own GATT service - the
exact one the official phone app uses - and injects a lamp packet on the
private app port, exactly as a tap on the lamp itself would. No firmware
change was needed for this to work: `LampModule`'s `handleReceived` already
accepts packets from anywhere on the mesh, phone-injected ones included.

    cd web && python3 -m http.server 8765
    open http://localhost:8765/

Chrome or Edge only - Safari and Firefox do not implement Web Bluetooth.
**First connection:** the OS will show a native Bluetooth pairing prompt -
enter **123456** (the lamps' fixed BLE PIN, chosen because no board here has a
screen to show a random one). If a write is rejected right after connecting,
that is normal: it is the OS finishing the pairing handshake underneath, and
the app retries once automatically.

Sync design: the app tracks its own Lamport counter, seeded from unix time in
seconds (~1.77 billion right now). Any real device counter is a small integer
by comparison, so a browser-sent command always outranks whatever the lamps
currently agree on, without the app needing to learn the live counter first.
It also updates from `node counter` values it decodes off the wire, so it
never regresses if the lamps have counted further while it was disconnected.

## `serial.html` - for the standalone/fast build

Single-page Web Serial app. It speaks the **same line-based command protocol**
the serial console uses (`tap`, `power`, `bright 0.4`, ...), so nothing on the
device had to change - the browser is just another terminal with buttons.

    cd web && python3 -m http.server 8765
    open http://localhost:8765/serial.html

Chrome, Edge or Arc only. Safari and Firefox do not implement Web Serial.

**Close any serial monitor first.** A port can only be held by one program, and
`arduino-cli monitor` will keep the browser from opening it.

Each lamp is a separate connection: press Connect on both panels and choose a
different port each time. The on-screen strip is an approximate preview rendered
from the same seed the firmware used - useful for seeing that both lamps agree,
and for watching the lamp that has no LEDs attached yet.
