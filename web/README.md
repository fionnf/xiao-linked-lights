# Using your Linked Lamps

This is the guide for actually **using** the lamps day to day — pairing your
phone or laptop, changing colours, setting an alarm, and fixing the handful of
things that can go wrong. If you're looking to build or flash the firmware
itself, see the [main README](../README.md) instead.

---

## 1. Open the app

**<https://fionnf.github.io/xiao-linked-lights/>**

That's it — there's nothing to install on a computer or Android phone. It's a
normal web page that talks to the lamp over Bluetooth.

| Device | What to use |
|---|---|
| Mac / Windows / Linux | **Chrome** or **Edge**. (Safari and Firefox don't support the Bluetooth feature this needs.) |
| Android | **Chrome**. |
| **iPhone / iPad** | See the next section — Safari can't do this on iOS, whichever browser icon it's wearing. |

### iPhone and iPad: install Bluefy first

Apple's Safari engine has never added the web standard this app needs
(“Web Bluetooth”), and Apple requires *every* browser on iOS — including
things that look like Chrome — to use Safari's engine underneath. So on an
iPhone, no ordinary browser can do this, full stop. The fix is a free app that
adds it:

1. Open the **App Store** and install **[Bluefy – Web BLE Browser](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055)**.
2. Open **Bluefy**, and inside it, go to `fionnf.github.io/xiao-linked-lights`
   (type the address in Bluefy's own address bar — it's a self-contained
   browser, separate from Safari).
3. Everything below works the same from here on.

---

## 2. Connect to a lamp

1. Tap **Connect**.
2. Pick **Meshtastic_d130** or **Meshtastic_d144** from the list your
   phone/computer shows you — these are the two lamps' Bluetooth names.
3. Your device will ask you to **pair**, and show a 6-digit code field.
   Enter **123456**. (It's a fixed code because these lamps have no screen to
   display a random one on.)
4. The app should now say "connected" with a green dot, and every control
   lights up.

Only one lamp at a time — connect to whichever one is nearer you. Changing
its colour (or tapping it) still updates the other lamp automatically over
their own radio link; you don't need to connect to both.

---

## 3. What everything does

- **Tap** — picks a new random scene, on both lamps.
- **Power** — turns the lamp off or on, with a slow dim rather than a snap.
- **Colour** — pick an exact colour: drag the R/G/B sliders, type a hex code,
  use your device's own colour picker, or tap one of the swatches (pulled
  straight from the lamp's built-in palette). **Warmth** is a fourth,
  independent channel — the strip's white LEDs, which add cosiness underneath
  whatever colour you've picked. Press **Send** to apply it.
- **Colour groups** — instead of one colour for the whole strip, split it into
  2 or 3 bands and give each one its own colour. Drag the numbered circles
  along the rainbow bar, then **Send groups**.
- **Mesh** — a list of every device this lamp can currently hear over its own
  long-range radio, not just the other lamp. Mostly useful for checking the
  lamps are actually in contact with each other (they'll be `hop: 0` if
  they're hearing each other directly).
- **Device** — shows this lamp's ID, a **Touch threshold** slider (see the
  touch section below), a **Reboot** button, and **Firmware update** — see
  its own section below, since it works differently from everything else
  here (Wi-Fi, not this Bluetooth connection).
- **Wi-Fi** — enter a network name and password so the lamp can get on the
  internet (used for the time, for the alarm below, and for firmware
  updates — it doesn't need Wi-Fi for the lamps to sync with each other).
  Saving it reboots the lamp, which takes about 15 seconds and will
  disconnect this app — reconnect once it's back.
- **Sunrise alarm** — set a time and how long the ramp should take. At that
  time, *this* lamp (not the other one) gradually brightens from a dim ember
  colour up to warm white over the chosen duration. Needs Wi-Fi connected
  above so the lamp knows what time it is.

---

## 3b. Updating the lamp's firmware

This is the one control that doesn't go through the Bluetooth connection
above — it needs the lamp on Wi-Fi (set that up in the **Wi-Fi** card first),
and it needs **this page loaded over plain `http://`, not the `https://`
GitHub Pages link.** Browsers block a secure page from talking to a plain
address on your home network, which is exactly what uploading to the lamp
needs to do. Run it locally instead:

```bash
cd web && python3 -m http.server 8765
open http://localhost:8765/
```

Then, under **Device → Firmware update**:

1. Find the lamp's IP address on your network (check your router's device
   list, or however you'd normally find a device's local IP) and enter it.
2. Choose the firmware file — the plain `firmware-xiao-lamp-*.bin`, **not**
   `*.factory.bin` (that one bundles the bootloader and partition table too,
   and isn't what an update expects).
3. Press **Upload & update**. A progress bar tracks the upload; the lamp
   reboots into the new firmware automatically once it's verified.

**If the upload fails, the lamp is unaffected** — it keeps running exactly
what it was running before. The update is written to a separate area of
flash first and only switches over once it's confirmed intact, the same
safety mechanism any standard ESP32/Arduino OTA update relies on.

---

## 4. Troubleshooting touch

The lamps use a small touch module, and how sensitive it is can vary a
little between the two — this is what the **Touch threshold** slider under
**Device** is for.

**If a lamp is changing colour on its own, or fires two taps from one
touch:** it's too sensitive. Connect to that specific lamp, open **Device**,
and drag the **Touch threshold** slider a bit *higher*, then **Set & save
threshold**. Try a small step first (e.g. 0.55 → 0.60) — the setting is saved
on the lamp itself, so it survives a power cut, and you won't need to redo it.

**If a lamp doesn't seem to register a real touch:** the opposite — lower the
threshold a little (e.g. 0.55 → 0.50) the same way.

**Each lamp is independent.** Turning down one lamp's sensitivity never
affects the other — you have to connect to each lamp separately to tune its
own threshold. If both lamps feel off, do them one at a time.

**If it's still wrong after a few small adjustments,** the module itself
might have a hardware sensitivity trimmer, or the ambient conditions changed
(a nearby light, humidity, a hand resting near it) — small threshold nudges
fix the vast majority of cases.

---

## 5. Troubleshooting the connection

**"Connection attempt failed" right after your phone/computer shows it as
paired.** This usually clears itself — the app automatically retries a
couple of times, so just try **Connect** again. If it keeps failing:

1. Open your Bluetooth settings (System Settings → Bluetooth on a Mac,
   Settings → Bluetooth on iPhone/Android).
2. Find the lamp (`Meshtastic_d130` / `Meshtastic_d144`), and choose
   **Forget This Device** (or the equivalent — "unpair"/"remove").
3. Come back to the app and press **Connect** again. It'll pair from
   scratch, PIN and all.

This is a known quirk of Bluetooth Low Energy in general, not something
specific to these lamps — it happens when your device's memory of the pairing
and the lamp's own memory of it fall out of sync (most often after the lamp's
firmware has been updated). Forgetting and re-pairing always fixes it.

**Nothing shows up when you tap Connect.** Make sure you're within a few
metres of the lamp, and that Bluetooth is actually on for your device. On
iPhone, double check you're inside **Bluefy**, not Safari — Safari will never
find it.

**The Wi-Fi card or Reboot button doesn't seem to do anything.** Both of
these make the lamp reboot to apply the change (~15 seconds), which drops
your Bluetooth connection on purpose. Wait, then reconnect.

---

## For developers

Two pages live here, one per firmware build the lamps can run (see the main
README's "Two firmwares" section):

- **`index.html`** — the Bluetooth app described above, for the **Meshtastic**
  build (the default). Talks directly to Meshtastic's own BLE GATT service —
  the same one the official phone app uses — so no firmware changes were
  needed for the lamp protocol itself to ride along on it.
- **`serial.html`** — a Web Serial app for the **standalone/fast** build.
  Speaks the same line-based command protocol as that build's serial console
  (`tap`, `power`, `bright 0.4`, ...). Chrome, Edge, or Arc only; close any
  serial monitor first, since a port only supports one program.

Run either locally instead of the deployed Pages link:

```bash
cd web && python3 -m http.server 8765
open http://localhost:8765/          # index.html
open http://localhost:8765/serial.html
```

Pushing to `master` redeploys `index.html`/`serial.html` to the Pages link
above automatically via `.github/workflows/pages.yml`.

### Implementation notes (`index.html`)

- Hand-rolled minimal protobuf encode/decode against Meshtastic's own wire
  messages (`MeshPacket`, `Data`, `AdminMessage`, `NodeInfo`) — no external
  library, just varint/fixed32/length-delimited helpers. Field numbers are
  cross-checked against the `.proto` sources in `meshtastic-firmware/protobufs/`.
- The lamp-specific messages (colour, threshold, alarm, groups) ride on
  Meshtastic's `PRIVATE_APP` portnum; Wi-Fi and reboot go through
  Meshtastic's real `AdminMessage` protocol (session-passkey handshake and
  all), not a shortcut.
- Sync design: the app tracks its own Lamport counter, seeded from unix time
  in seconds (~1.77 billion right now). Any real device counter is a small
  integer by comparison, so a browser-sent command always outranks whatever
  the lamps currently agree on, without the app needing to learn the live
  counter first. It also updates from counter values it decodes off the
  wire, so it never regresses if the lamps have counted further while it was
  disconnected.
- Per-lamp settings (touch threshold, alarm) are unicast to whichever node is
  currently connected and persisted in that lamp's own NVS — the app's
  `localStorage` cache of them is a convenience for the slider's starting
  position, not the source of truth.
- The Mesh card's node list re-requests every 30 s while connected
  (`meshRefreshInterval`), not just once at connect - `FromRadio.node_info`
  only flows in response to a fresh `want_config_id`, so a one-shot request
  would miss nodes that join the mesh later in the session.

### Firmware update endpoint

`LampModule` runs its own tiny `WebServer` on **port 8080** (not 80 -
Meshtastic's own web server already owns that) once Wi-Fi connects,
`POST /update` writes straight through Arduino's `Update` library to
`esp_ota_get_next_update_partition()` - always the app slot that is NOT
currently running, never the one executing the update itself. That's a hard
requirement on ESP32, not a nicety: it executes code directly out of
memory-mapped flash, so self-overwriting the running partition reliably
crashes mid-write rather than merely risking it. `Update.end(true)` verifies
the image before ever moving the boot pointer, so a bad/partial upload
leaves the device running whatever it was running before, untouched.

This is also why the lamps no longer dual-boot two different firmwares (see
the main README's "Two firmwares" section) - that used the same two
partition slots to hold two genuinely different builds, which is
incompatible with an OTA mechanism that assumes both slots hold the same
firmware, one version behind the other.
