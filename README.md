# Linked Lamps

Two lamps that always show the same colour. Touch one, both change — over 868 MHz
LoRa, with no wifi required for the core link, no hub, no server. Control either
lamp directly from a browser over Bluetooth: colour, sunrise alarm, touch
sensitivity, Wi-Fi, and the mesh it's part of.

```
   ┌──────────────┐                                    ┌──────────────┐
   │  touch pad   │──┐                              ┌──│  touch pad   │
   │   XIAO S3    │  ▼                              ▼  │   XIAO S3    │
   │              │ SX1262 ))))  868 MHz, 19 B  (((( SX1262           │
   │ 39 × SK6812  │                                    │ 39 × SK6812  │
   └──────────────┘                                    └──────────────┘
          tap ──────────────► 211 ms ──────────────► same colour
```

---

## Status

| | |
|---|---|
| Radio link | working — 211 ms, measured (fast build); ~10 s (Meshtastic build) |
| Colour sync | working, self-healing, fingerprint-verified |
| LED strip | working — 39 SK6812 RGBW |
| Capacitive touch | working — TTP223 module, tunable threshold, persisted on-device |
| Sunrise alarm | working — per-lamp, hue + brightness ramp, NTP time |
| Colour groups | working — 2–3 independently-coloured bands, set by hand |
| Bluetooth web app | working — the primary control surface, see below |
| Browser control (serial) | working — Web Serial, standalone/fast build only |
| Meshtastic build | working — mesh range + phone app + web app, default |
| State persistence | working on both builds — survives a power cut |

Both lamps currently run the **Meshtastic** build (EU_868, LongFast, default
channel), see each other, and hold the fast build in the second flash slot.

---

## Control it: the Bluetooth web app

**Just want to use the lamps?** [**web/README.md**](web/README.md) is the
full step-by-step guide — opening the link, installing Bluefy on iPhone,
what every control does, and troubleshooting touch/Bluetooth. The rest of
this section is a quick reference.

**<https://fionnf.github.io/xiao-linked-lights/>** — talks directly to
Meshtastic's own BLE GATT service (the same one the official phone app uses),
so nothing extra needs installing. Chrome or Edge, desktop or Android.

**iOS has no Web Bluetooth in any browser** (Safari's engine has never
implemented it, and Apple requires every iOS browser to use that engine) —
install [Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055)
from the App Store and open the link there instead.

First connection: the OS will show a native Bluetooth pairing prompt — enter
**123456**.

What it controls, live over BLE:

- **Tap** a random scene, **Power** on/off (a smooth dim, not a flash), an
  RGBW colour picker (sliders, hex input, or the firmware's own tint palette
  as one-tap swatches)
- **Colour groups** — drag 2 or 3 numbered handles on a hue bar to set each
  band's colour by hand, instead of a generated scene
- **Touch threshold** — the TTP223 module has no analog signal to threshold
  in firmware, so this tunes a smoothed 0–1 "how touched" level instead; set
  per-lamp, saved to NVS, survives a reboot
- **Sunrise alarm** — a local wake-up ramp (dim ember → warm white) at a set
  time and duration, per lamp; needs Wi-Fi below for the clock
- **Wi-Fi** — reads and writes the device's real network config through
  Meshtastic's admin protocol (not a custom hack), so it never disturbs
  settings the app has no UI for
- **Mesh** — every node in the lamp's on-radio database, not just the other
  lamp: name, hop count, SNR, last heard
- **Reboot**

Run it locally instead of the Pages link if you're changing it:

```bash
cd web && python3 -m http.server 8765
open http://localhost:8765/
```

---

## Hardware

| Part | Notes |
|---|---|
| Seeed XIAO ESP32-S3 ×2 | 8 MB flash, 8 MB PSRAM |
| Wio-SX1262 LoRa module ×2 | 868 MHz, wired to the header (see below) |
| SK6812 RGBW strip | 39 LEDs per lamp, GRBW byte order |
| 330 Ω resistor | series on DIN, prevents ringing |
| TTP223 touch module ×2 | digital touch output, no external resistor needed |
| 5 V supply, ≥3 A | **not** the XIAO's 5 V pin — see [Power](#power) |
| 1000 µF capacitor | across the strip's supply at the DIN end |

### Wiring

```
  5 V PSU ────────┬────────────────► VCC   SK6812 strip
                  │
              1000 µF
                  │
          ────────┴────────────────► GND ──┐
                                           │ ← must be common
  XIAO GND ────────────────────────────────┘
  XIAO D6 (GPIO43) ──[330 Ω]──────────────► DIN
  XIAO D7 (GPIO44) ◄───────────────────────  OUT   TTP223 module
  XIAO 3V3 ─────────────────────────────────► VCC  TTP223 module
  XIAO GND ─────────────────────────────────► GND  TTP223 module
```

### Pin budget

The radio consumes almost the whole header. What remains:

| Pad | GPIO | Used by |
|---|---|---|
| D1 / D2 / D3 / D4 | 2 / 3 / 4 / 5 | LoRa DIO1 / RST / BUSY / NSS |
| D8 / D9 / D10 | 7 / 8 / 9 | LoRa SPI |
| **D6** | **43** | **LED data** |
| **D7** | **44** | **touch** |
| D0, D5 | 1, 6 | leave alone — one is `RF_SW1` |

---

## The part that cost a day

**Stock Meshtastic cannot drive this hardware, and fails silently.**

Meshtastic's `seeed_xiao_s3` variant expects the SX1262 on GPIO 41/42/40/39 —
reachable only through the Wio board's 30-pin board-to-board connector. This build
is wired through the module's 1×7 breakout headers instead, so those pins float.
The firmware boots, finds no radio, never advertises Bluetooth, never answers the
serial API, and prints no error anywhere.

Worse was **RST**. It is on **GPIO3**, and until that was found the chip had never
once been reset. An SX126x that has not been reset will happily answer SPI reads,
accept register writes and execute `SetPacketType` — while refusing to start its
oscillator, so it can never transmit. `GetDeviceErrors` returned a clean `0x0000`
throughout, because that bit is only latched during a reset cycle. The moment a
real reset was applied, the same register returned `0x0020` — `XOSC_START_ERR` —
and everything worked.

### Measured pin map

```
SCK  = GPIO7 (D8)    MISO = GPIO8 (D9)    MOSI = GPIO9 (D10)
NSS  = GPIO5 (D4)    RST  = GPIO3 (D2)    BUSY = GPIO4 (D3)    DIO1 = GPIO2 (D1)
TCXO = 1.8 V via the chip's own DIO3
```

Every one of those was proved by experiment, not copied:

- **NSS** — held high, the chip stops answering entirely
- **RST** — a marker written to register `0x0740` reverts to the documented default `0x1424`
- **BUSY** — held high for the whole `Calibrate(0x7F)` window
- **DIO1** — tracked the TX_DONE IRQ: `0→1` on latch, `→0` on clear

The chip reports `"SX1261"` in register `0x0320`. RadioLib's `SX1262` class drives
it correctly regardless.

Each sketch under [`diagnostics/`](diagnostics/) is kept deliberately — together
they document the hardware, and each records what it ruled out.

---

## Two firmwares

Both live in flash at once. They share an identical 8 MB partition table, so
Meshtastic sits in `app0`, the direct-radio build in `app1`, and switching just
repoints the bootloader — nothing is erased, nothing is downloaded.

| | **fast** (`firmware/lamp/`) | **mesh** (`meshtastic-variant/`) |
|---|---|---|
| Tap → other lamp | **0.21 s** | ~10 s (1.5–17.5 s) |
| Range | lamps must hear each other | any Meshtastic node relays |
| Phone app | no | yes |
| Bluetooth web app | no | **yes** |
| Web Serial control | yes | no |
| Sunrise alarm / colour groups / tunable touch / Wi-Fi admin | no | yes |

All figures measured on this hardware. Meshtastic's delay is its transmit
scheduling, not airtime — raising packets to `Priority_HIGH` changed nothing. The
mesh build is the default: it has every control surface above, and the range/
relay/phone-app benefits are worth more than the latency in normal use. Use
**fast** only if you specifically want the sub-second link and don't need the
rest.

**Switching:** type `mesh` on the fast build's serial console; send a Meshtastic
text message saying exactly `lamp fast` to come back.

### Use LongFast if you want the actual network

Meshtastic's public network runs on the **LongFast** preset. A faster preset like
`SHORT_FAST` looks tempting for latency, but it changes the spreading factor and
bandwidth — the lamps then cannot hear anyone else's node and nobody can hear
them. They become a private two-node mesh that merely looks like Meshtastic. If
the point is relaying, stay on LongFast.

---

## How the sync works

### Send the seed, not the scene

The colour engine randomises band count, band sizes, hues and white levels on every
tap. Rather than transmit all that, both lamps run the *same* deterministic
generator from the *same* 32-bit seed and independently produce byte-identical
scenes — randomness intact, four bytes on air.

This is why the generator is an explicit xorshift32 and never `rand()`: `rand()`'s
sequence is not portable, so the lamps would drift apart.

### Wire format — 19 bytes

```
 0     magic 0xC1
 1     type: 1 SCENE · 2 POWER · 3 STATE · 4 COLOUR
       Meshtastic build only, unicast to one lamp, not broadcast:
       5 THRESHOLD (touch sensitivity) · 6 ALARM (sunrise) · 7 GROUPS (per-band hue)
 2..5  counter        (Lamport)
 6..9  payload        (seed, packed RGBW, or a type-specific bit-packed value)
10..13 node id        (stable tiebreak)
14..17 visual code    (fingerprint of exactly what is displayed)
18     flags
```

Types 1–4 are the original shared-state protocol both firmwares speak and
broadcast, so both lamps converge. Types 5–7, added for the Bluetooth web app,
are Meshtastic-build only: 5 and 6 are deliberately **unicast** and never
synced — touch sensitivity and a wake alarm are per-device hardware settings,
not shared strip state. 7 (colour groups) broadcasts and syncs like 1–4.

### Why they cannot drift apart

**Lamport counter, node-id tiebreak.** Higher counter wins; equal counters break on
node id. Tap both lamps at once and they converge on one winner rather than
ping-ponging.

**Visual fingerprint.** An FNV-1a hash over the complete displayed state — mode,
colour or seed, band count, every band length and hue. Counters alone cannot catch
two lamps that agree on a counter while showing different things; one number can.
Matching codes prove matching strips.

**Periodic announcements.** A tap is sent once, with no ack and no retry, and both
LoRa and mesh relaying can drop packets. Each lamp re-announces roughly every
minute and answers immediately when it hears a stale neighbour.

---

## Getting started

```bash
# build and flash the fast firmware
./tools/flash.sh fast

# or the Meshtastic build
./tools/flash.sh mesh
```

The board has no USB-UART bridge, so it changes device node between modes:
`/dev/cu.usbmodem<MAC>1` running Meshtastic, a short name like
`/dev/cu.usbmodem101` in download mode or on the fast build.

A plain hard reset leaves this chip **in** download mode — boot it with
`--after watchdog-reset`.

### Serial console (fast build, 115200)

```
tap              new random scene, broadcast to the other lamp
seed 42          a specific scene — same number on both lamps, same look
colour R G B W   an explicitly chosen colour, synced
power            toggle both lamps
pos 0.5          whole strip at one palette position (0 warm white → 1 saturated)
chain            replay the startup animation
test             walk one pixel down the strip
sync             announce state so the other lamp can catch up
status           everything, including the visual code
mesh             reboot into the Meshtastic build

groups 2         force 2 bands (3, or 0 = random 2–3 per scene)
bright 0.4       brightness
fade 120         crossfade frames (60 ≈ 1 s)
breathe 0        idle shimmer depth
drift 65         autonomous scene changes, 0 = off (default)

touchmon         live touch readings — raw pin state + smoothed level + threshold
cal              reset the smoothed touch level
```

**On the Meshtastic build**, there is deliberately no equivalent serial console:
Meshtastic's USB serial is Meshtastic's own framed API (what `meshtastic --info`
and phone-over-USB use), and a module reading raw bytes off that same UART
starves it — this was tried, broke the CLI/phone-over-USB connection on both
boards, and was reverted. Touch tuning, status, and everything else on that
build goes through the [Bluetooth web app](#control-it-the-bluetooth-web-app)
instead.

### Phone app (mesh build)

Pair `Meshtastic_d130` / `Meshtastic_d144` with PIN **123456**. It is a fixed PIN
rather than a random one because these boards have no OLED for Meshtastic to
display one on.

---

## Power

39 SK6812 RGBW draw roughly:

| | per LED | 39 LEDs |
|---|---|---|
| all four channels full | ~80 mA | **~3.1 A** |
| this palette at `bright 0.6` | ~25 mA | **~1 A** |

**Do not run these from the XIAO's 5 V pin** — that is straight VBUS passthrough,
and an amp through the USB connector is not something to ask of your laptop. Use a
5 V supply of 3 A or more, share ground with the XIAO, and put 1000 µF across the
strip at the DIN end. The SX1262 also draws a ~120 mA spike on every transmit, so a
sagging rail shows up as random resets mid-message.

At this length expect voltage drop; if the far end goes dim and pink, run a second
pair of power wires to it. Data still enters at DIN only.

---

## Troubleshooting

**A board enumerates as "Generic CDC" with no serial number** — suspect the **USB
cable** before the board. A flaky cable corrupted one board mid-flash during
development and then could not carry a clean recovery.

**A board stops responding** — hold **B** (BOOT) *on the XIAO*, plug USB in, hold
two seconds, release. Nothing here can brick a board; recovery is always USB.

**A board shows as `USB JTAG_serial debug unit`** — that name covers *both* ROM
download mode and the fast build, so it does not tell you which is running. Probe
it: `esptool --port <p> --chip esp32s3 --before no-reset --after no-reset read-mac`.
If that answers, the board is in download mode; boot it with
`--after watchdog-reset`.

**`meshtastic --info` times out, but the lamp works fine over BLE and its own
log looks healthy** — this specific CLI handshake has been unreliable on this
hardware throughout development, independent of firmware changes. A BLE scan
(`Meshtastic_d1xx` advertising) or the web app connecting successfully is a
more reliable health check than that one command.

**Bluetooth shows "paired" in the OS but the web app's connection attempt
fails** — usually a transient link-layer timing issue right after bonding; the
app retries the connect step automatically. If it still won't connect, forget
the device in the OS's Bluetooth settings and reconnect from the app to re-pair
from scratch.

**No other nodes in the node list** — check `rxBad` in the debug log, or the
Mesh card in the web app. A non-zero `rxBad` means foreign traffic is arriving
but not decoding (wrong channel or PSK); zero means nothing else is
transmitting in range at all, which is common indoors at 868 MHz.

**Lamps show different colours** — compare `status` on the fast build, or
watch each lamp's log for its `code=` fingerprint on the Meshtastic build; if
they differ, either `sync` (fast build) or waiting out the periodic
re-announcement forces reconciliation.

**Verify radio work against the chip, not the library.** Early on, two boards
appeared to exchange packets at −40 dBm. They were not: every "packet" had an empty
payload and byte-identical RSSI/SNR, because RadioLib was waiting on a DIO1 pin
that sat permanently high and every call returned success instantly. Real reception
has a non-empty payload, an advancing sequence, a plausible time-on-air and slight
RSSI jitter. Check `GetStatus` (0xC0), `GetIrqStatus` (0x12) and `GetDeviceErrors`
(0x17) directly.

---

## Layout

```
firmware/lamp/         the fast, direct-radio firmware
meshtastic-variant/    custom Meshtastic variant + the lamp module (source of truth;
                       meshtastic-firmware/ is a gitignored upstream clone built from it)
diagnostics/           bring-up sketches; each documents what it ruled out
web/                   index.html: Bluetooth control app (deployed via GitHub Pages)
                       serial.html: Web Serial control app, standalone/fast build only
.github/workflows/     Pages deploy for web/
docs/                  Meshtastic reference, Seeed schematic, module datasheet
firmware/backups/      full-flash dumps taken before reflashing (gitignored, local only)
tools/flash.sh         switch a board between the two firmwares
CLAUDE.md              full engineering log — read before changing anything
```

---

## Licence

MIT — see [LICENSE](LICENSE).

Built on [Meshtastic](https://meshtastic.org) and
[RadioLib](https://github.com/jgromes/RadioLib).
