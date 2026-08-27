# Using your Linked Lamps

This is the guide for actually **using** the lamps day to day — pairing your
phone or laptop, changing colours, setting an alarm, and fixing the handful of
things that can go wrong. If you're looking to build or flash the firmware
itself, see the [main README](../README.md) instead.

> **In a hurry?** Open <https://fionnf.github.io/xiao-linked-lights/> in
> Chrome (iPhone: install Bluefy first, below), press **Connect**, pick a
> lamp from the list, enter **123456** if asked. That's the whole thing —
> the rest of this page is detail and troubleshooting for when something
> doesn't go that smoothly.

---

## 1. Open the app

**<https://fionnf.github.io/xiao-linked-lights/>**

There's nothing to install on a computer or Android phone — it's a normal
web page. The only exception is iPhone/iPad, covered below.

| Your device | What to use |
|---|---|
| Mac | **Chrome** or **Edge**. Not Safari. |
| Windows | **Chrome** or **Edge**. |
| Linux | **Chrome** or **Edge**. |
| Android phone/tablet | **Chrome**. |
| **iPhone / iPad** | Install **Bluefy** first — see below. Not Safari, not Chrome, not any other ordinary browser. |

**Why not Safari or Firefox?** They don't implement the specific web feature
("Web Bluetooth") this app needs to talk to the lamp. This is a permanent
limitation of those browsers, not a bug in this page — if you open the link
in Safari, expect the **Connect** button to do nothing or show an error.

### iPhone and iPad: install Bluefy first (one-time, ~1 minute)

Apple's Safari engine has never added Web Bluetooth, and Apple requires
*every* browser on iOS — including anything that looks like Chrome — to use
Safari's engine underneath. So on an iPhone, literally no ordinary browser
can do this. The fix is a free app that adds the missing feature:

1. Open the **App Store**.
2. Search for **Bluefy** and install **[Bluefy – Web BLE Browser](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055)**
   (it's free, and its icon is a blue circle with a small Bluetooth logo).
3. Open **Bluefy** itself — not Safari — from your home screen.
4. Inside Bluefy, tap its address bar (it works like a mini web browser) and
   type: `fionnf.github.io/xiao-linked-lights`
5. Press Go. The lamp control page should load, looking exactly like it
   would in Chrome on a computer.

From here on, every instruction below applies the same way inside Bluefy as
it does in Chrome — just remember to always open the link from *inside*
Bluefy, not by tapping the link from Messages/Notes/Safari, which would open
it in Safari instead and it won't work there.

---

## 2. Connect to a lamp

1. Press the **Connect** button near the top of the page.
2. Your browser will pop up its own device-picker window (this is your
   browser/OS's own UI, not part of the page) listing nearby Bluetooth
   devices. Look for **Meshtastic_d130** or **Meshtastic_d144** — these are
   the two lamps' fixed names. Click the one you want, then click
   **Pair** / **Connect** in that same picker window.
   - **Don't see either name?** Close the picker, wait 5 seconds, and press
     **Connect** again — Bluetooth advertising is intermittent by design.
     If it still doesn't appear after a few tries, see
     [Troubleshooting the connection](#5-troubleshooting-the-connection)
     below.
3. The very first time you connect to a given lamp, your operating system
   will separately pop up a **pairing request** with a place to enter a
   6-digit code. Type **123456** and confirm. (It's a fixed code, not a
   random one, because these lamps have no screen to display a random code
   on.) You should only see this pairing prompt once per lamp, ever — after
   that, your device remembers it.
4. Within a couple of seconds, the page itself should update: the dot next
   to the status line turns **green**, the text says **"connected: ..."**,
   and every button/slider on the page changes from greyed-out to fully
   usable. That's your confirmation it worked — if the dot is still grey,
   it hasn't connected yet.

**Only connect to one lamp at a time.** Changing colour, tapping, or
anything else still updates *both* lamps automatically over their own
long-range radio to each other — you never need to open two connections or
connect to both lamps to keep them in sync.

**Reconnecting later:** every time you come back to the page (or reload it),
you'll need to press **Connect** and pick the lamp again — the browser
doesn't remember an open connection across page loads, only the *pairing*
(so you won't be asked for 123456 again, just to pick it from the list).

---

## 3. What everything does

- **Tap** — picks a new random scene, on both lamps.
- **Power** — turns the lamp off or on, with a slow dim rather than a snap.
- **Colour** — pick an exact colour: drag the R/G/B sliders, type a hex code
  (e.g. `#FF9900`), use your device's own colour picker (the small square
  swatch), or click one of the round swatches (pulled straight from the
  lamp's built-in palette). **Warmth** is a fourth, independent slider — the
  strip's white LEDs, which add cosiness underneath whatever colour you've
  picked; it doesn't change the colour itself, only how "warm" it looks.
  Press **Send** to actually apply whatever you've dialed in — nothing
  changes on the lamp until you press it.
- **Colour groups** — instead of one colour for the whole strip, split it
  into 2 or 3 bands and give each one its own colour. Click **2 groups** or
  **3 groups** to choose how many, drag the numbered circles left/right
  along the rainbow bar to set each one's hue, then press **Send groups**.
- **Mesh** — a list of every device this lamp can currently hear over its
  own long-range radio, not just the other lamp. Mostly useful for
  confirming the two lamps are actually in contact with each other. This
  list refreshes automatically roughly every 30 seconds while you're
  connected — you don't need to do anything to keep it current.
- **Device** — shows this lamp's ID, a **Touch threshold** slider (see
  [section 4](#4-troubleshooting-touch) below), a **Reboot** button, and
  **Firmware update** (see [section 3b](#3b-updating-the-lamps-firmware)
  below — it works differently from everything else on this page).
- **Wi-Fi** — enter a network name and password so the lamp can get online.
  This is only needed for two things: the sunrise alarm knowing what time it
  is, and firmware updates. **The lamps do not need Wi-Fi to stay in sync
  with each other** — that always happens over their own radio link,
  Wi-Fi or not. Pressing **Save** reboots the lamp (~15 seconds) and will
  drop this page's connection to it on purpose — just reconnect once it's
  back, the Wi-Fi setting itself is already saved.
- **Sunrise alarm** — set a time of day and how many minutes the ramp should
  take. At that time, *this specific lamp* (not the other one — each lamp
  has its own independent alarm) gradually brightens from a dim ember colour
  up to warm white over that many minutes. Requires Wi-Fi connected above,
  since the lamp needs to know the actual current time.

---

## 3b. Updating the lamp's firmware

This is the one feature that does **not** go through the Bluetooth
connection above — everything else on this page talks to the lamp over
Bluetooth; this one talks to it over your home Wi-Fi network instead. Two
things have to be true first:

- The lamp needs to already be on your Wi-Fi (set that up in the **Wi-Fi**
  card first, and wait for it to reboot and reconnect).
- **This page needs to be open over plain `http://`, not the secure
  `https://` GitHub Pages link you use for everything else.** Browsers
  actively block a secure page from talking to a plain address on your home
  network — this isn't a bug to work around, it's a deliberate browser
  security rule, so the *only* fix is to load the page differently:

  ```bash
  cd web && python3 -m http.server 8765
  ```

  then open **http://localhost:8765/** (note: `http`, not `https`) in your
  browser. Everything else on the page works exactly the same from here —
  only the address bar looks different.

Then, on the **Device** card, under **Firmware update**:

1. In the **Lamp IP** field, type the lamp's address on your network. The
   easiest option: type **`lamp-d130.local`** or **`lamp-d144.local`**
   (matching whichever lamp you're updating — same last four characters as
   its Bluetooth name) instead of hunting for a numeric IP address. This
   works automatically on Mac and iPhone/iPad. On Windows, `.local` names
   sometimes need [Bonjour](https://support.apple.com/kb/dl999) installed
   (it comes bundled with iTunes, or can be installed standalone) — if it
   doesn't resolve, fall back to finding the numeric IP address in your
   router's connected-devices list instead, and use that.
2. Click **Choose File** and select the firmware file you were given — it
   should be named like `firmware-xiao-lamp-<version>.bin`. **Do not** pick
   a file ending in `.factory.bin` — that one contains extra data the
   update process doesn't expect and will fail to verify (safely — see
   below — but it won't work).
3. Press **Upload & update**. A progress bar will fill up as the file
   uploads. When it reaches 100%, wait a few more seconds for a status
   message ("OK — verified, rebooting…" or an error). If it succeeds, the
   lamp reboots into the new firmware by itself within a few seconds — you
   don't need to do anything further.

**If anything goes wrong during this — a dropped Wi-Fi connection, an
interrupted upload, the wrong file — the lamp is completely unaffected.** It
keeps running exactly the firmware it was running before, and you can just
try again. The new firmware is written to a separate, unused area of the
chip's memory first, and the lamp only ever switches over to it after
confirming the upload arrived complete and undamaged.

---

## 4. Troubleshooting touch

The lamps use a small touch module, and how sensitive it is can vary a
little between the two — this is what the **Touch threshold** slider under
**Device** is for. It's a number between roughly 0.05 (extremely sensitive)
and 0.95 (barely sensitive); each lamp ships around 0.55.

**Symptom: the lamp changes colour by itself, or one touch sometimes counts
as two.** It's too sensitive. Fix:
1. Connect to *that specific lamp* (the sensitivity setting only affects
   whichever lamp you're connected to).
2. Open the **Device** card.
3. Drag the **Touch threshold** slider a little to the *right* (a bigger
   number = less sensitive). Try a small move first — e.g. from 0.55 to
   0.60 — rather than a big jump.
4. Press **Set & save threshold**.
5. Try touching the lamp again. If it's still too sensitive, repeat with
   another small increase.

**Symptom: touching the lamp doesn't do anything, or you have to press hard
or repeatedly.** The opposite problem — lower the threshold a little instead
(e.g. 0.55 down to 0.50), following the same steps above.

**This setting is saved on the lamp itself**, in memory that survives a
power cut, so you only need to do this once per lamp, not every time you
plug it in.

**Each lamp's sensitivity is completely independent of the other's.**
Adjusting one lamp never changes the other — if both feel off, connect to
each one in turn and adjust them separately.

**If several small adjustments in a row don't help,** something in the
lamp's surroundings may have changed — a hand or object resting near the
touch pad, a nearby light fixture, humidity — rather than the setting itself
being wrong. Try moving anything touching or very close to the lamp's touch
pad away from it first, then reassess.

---

## 5. Troubleshooting the connection

**The device picker shows the lamp as paired, but connecting still says
something like "Connection attempt failed."** This almost always clears
itself — the app automatically retries the connection a couple of times on
its own, so first just wait a couple of seconds and try pressing **Connect**
again without changing anything. If it keeps failing after several tries:

1. Open your device's Bluetooth settings directly (not through this app) —
   on a Mac: **System Settings → Bluetooth**; on iPhone: **Settings →
   Bluetooth**; on Android: **Settings → Connected devices → Bluetooth**.
2. Find the lamp in that list (**Meshtastic_d130** or **Meshtastic_d144**),
   tap/click it (or its ⓘ "info" icon if there is one), and choose
   **Forget This Device** — it may also be labelled "Unpair" or "Remove
   Device" depending on your OS.
3. Go back to the lamp app and press **Connect** again. It will act as if
   it's never seen this lamp before — expect the whole pairing sequence
   again, 6-digit code included.

This is a normal quirk of Bluetooth Low Energy pairing in general — it
happens when your device's memory of a pairing and the lamp's own memory of
it fall out of step with each other, which is common right after the lamp's
firmware has changed. Forgetting and re-pairing fixes it every time; there's
no deeper problem to chase.

**Pressing Connect shows an empty or immediately-closing device list, with
neither lamp name in it.** Check, in order:
- Is Bluetooth actually turned on for your phone/computer? (Not just "the
  app allowed to use it" — the physical/OS toggle.)
- Are you within a few metres of the lamp?
- **On iPhone specifically:** are you definitely inside the **Bluefy** app,
  not Safari or another browser? Safari will never show any devices here,
  by design, with no error message to explain why.
- Is the lamp actually powered on? (Unplugging it stops it advertising
  entirely, same as any Bluetooth device.)

If all of those check out and it still won't appear, wait about 10 seconds
and try **Connect** once more before assuming something is actually wrong —
Bluetooth advertising is inherently intermittent.

**I pressed Save on the Wi-Fi card (or Reboot), and now the app looks
disconnected/frozen.** That's expected, not an error — both of those
actions make the lamp reboot to apply the change, which takes about 15
seconds and necessarily drops whatever was connected to it over Bluetooth.
Just wait, then press **Connect** again.

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

Also advertises itself over **mDNS** as `lamp-<last4hex>.local` (e.g.
`lamp-d130.local`), matching the last four hex digits of the node id already
used in its `Meshtastic_d1xx` Bluetooth name - removes the single biggest
point of friction in the update flow (hunting a router's DHCP client list
for a numeric IP) on every platform where mDNS resolution just works out of
the box (macOS, iOS; Windows needs Bonjour installed separately). Falls back
silently to "use the IP address" if `MDNS.begin()` fails for any reason -
never blocks the update server itself from starting.
