# Linked Lamps

Two lamps that always show the same colour. Touch one, both change — over 868 MHz
LoRa, with no wifi, no hub, no server and no internet. Just power.

A port of [linked_friend_lights](https://github.com/fionnf/linked_friend_lights)
(Pico + wifi + MQTT) onto a radio link, so the lamps work anywhere the two can
hear each other.

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
| Radio link | working — 211 ms, measured |
| Colour sync | working, self-healing, fingerprint-verified |
| LED strip | working — 39 SK6812 RGBW |
| Startup animation | working |
| Browser control | working — Web Serial, colour picker |
| Meshtastic build | working — mesh range + phone app, ~10 s |
| State persistence | fast build only — survives a power cut |
| Capacitive touch | TTP223 module, wiring in progress on lamp B |

Both lamps currently run the **Meshtastic** build (EU_868, LongFast, default
channel), see each other, and hold the fast build in the second flash slot.

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
| Browser control | yes | no |

All figures measured on this hardware. Meshtastic's delay is its transmit
scheduling, not airtime — raising packets to `Priority_HIGH` changed nothing. Use
**fast** day to day; use **mesh** when the lamps are genuinely apart.

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
 2..5  counter        (Lamport)
 6..9  payload        (seed, or packed RGBW for a picked colour)
10..13 node id        (stable tiebreak)
14..17 visual code    (fingerprint of exactly what is displayed)
18     flags
```

### Why they cannot drift apart

**Lamport counter, node-id tiebreak.** Higher counter wins; equal counters break on
node id. Tap both lamps at once and they converge on one winner rather than
ping-ponging.

**Visual fingerprint.** An FNV-1a hash over the complete displayed state — mode,
colour or seed, band count, every band length. Counters alone cannot catch two
lamps that agree on a counter while showing different things; one number can.
Matching codes prove matching strips.

**Periodic announcements.** A tap is sent once, with no ack and no retry, and LoRa
drops packets. Each lamp re-announces every 15 s and answers immediately when it
hears a stale neighbour. Verified by rebooting a lamp mid-session: it reconverged
in 6 s with no tap.

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

touchmon         live touch readings — use while wiring the pad
cal              re-baseline touch
```

### Browser control

```bash
cd web && python3 -m http.server 8765
open http://localhost:8765/
```

Web Serial: Chrome, Edge or Arc only. Colour picker, white-channel slider,
presets, live tuning and a per-lamp log. Close any serial monitor first — a port
only supports one program.

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

**No other nodes in the node list** — check `rxBad` in the debug log. A non-zero
value means foreign traffic is arriving but not decoding (wrong channel or PSK);
zero means nothing else is transmitting in range at all, which is common indoors
at 868 MHz. Try a window sill.

**Lamps show different colours** — compare `status` on both. The `code=` line is
the fingerprint; if they differ, `sync` on either forces reconciliation.

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
firmware/lamp/        the fast, direct-radio firmware
meshtastic-variant/   custom Meshtastic variant + the lamp module
diagnostics/          bring-up sketches; each documents what it ruled out
web/                  browser control panel (Web Serial)
proto/                colour-sync protocol prototype in Python
docs/                 Meshtastic reference, Seeed schematic, module datasheet
tools/flash.sh        switch a board between the two firmwares
CLAUDE.md             full engineering log — read before changing anything
```

---

## Licence

MIT — see [LICENSE](LICENSE).

Built on [Meshtastic](https://meshtastic.org) and
[RadioLib](https://github.com/jgromes/RadioLib).
