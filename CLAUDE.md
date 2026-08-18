# xiao-linked-lights — working notes

Two lamps that always show the same colour. Each lamp = Seeed XIAO ESP32-S3 +
Wio-SX1262 LoRa module + addressable RGBW LED strip + capacitive touch sensor.
Touch one lamp, both change colour.

This file is the durable log. If a session is interrupted, start here.

---

## 1. THE HEADLINE FINDING

**The LoRa module is NOT wired the way Meshtastic expects. Stock Meshtastic
firmware can never drive this hardware.**

Measured pin map (proved by RadioLib `begin()` returning `ERR_NONE`, then by two
boards exchanging real 868 MHz packets):

| Signal | GPIO | XIAO pad |
|---|---|---|
| SPI SCK  | 7 | D8 |
| SPI MISO | 8 | D9 |
| SPI MOSI | 9 | D10 |
| NSS / CS | **5** | D4 |
| NRST     | **1** | D0 |
| BUSY     | **2** | D1 |
| DIO1     | **4** | D3 (unconfirmed — see §5) |
| TCXO     | on the SX1262's own DIO3 @ 1.8 V | — |
| button?  | **3** | D2 (reads high via pull-up; user confirms a button on the module) |

What Meshtastic's `seeed_xiao_s3` variant drives instead, and which is wrong for
this hardware: `cs=41 rst=42 busy=40 dio1=39 rxen=38`, plus LED 48 and button 21.
Those pins all read **floating** on our boards — nothing is connected to them.

`seeed_xiao_s3` is the ONLY XIAO variant Meshtastic ships (checked the full
`variants/esp32s3/` listing at tag `v2.7.26.54e0d8d`). So there is no stock
target that works. Options are custom Meshtastic firmware with a corrected
variant, or our own RadioLib firmware. Not yet decided — see §6.

---

## 2. HARDWARE

Two boards, both ESP32-S3 (QFN56) rev v0.2, 8 MB flash, 8 MB PSRAM:

| Board | MAC | Node id (see §5) |
|---|---|---|
| A | `cc:ba:97:16:d1:30` | `30D1` |
| B | `cc:ba:97:16:d1:44` | `44D1` |

Nothing else is wired to either board yet — no LED strip, no touch sensor, no
OLED (the I2C scan on GPIO5/6 finds zero devices).

Region: **EU_868** (user is at ETH Zurich). Confirmed with the user.

### USB port naming — this WILL confuse you

There is no USB-UART bridge; the ESP32-S3's native USB is used, so the device
node changes with what is running:

- Meshtastic firmware (TinyUSB CDC): `/dev/cu.usbmodem<MAC>1`, e.g.
  `/dev/cu.usbmodemCCBA9716D1301`, USB product name `seeed_xiao_s3`
- ROM download mode / our Arduino builds (hardware USB-Serial-JTAG):
  short names like `/dev/cu.usbmodem21301`, `/dev/cu.usbmodem101`,
  USB product name `USB JTAG_serial debug unit`

Always re-check `ls /dev/cu.usbmodem*` after any reset, and confirm which board
you are talking to with `esptool ... read-mac` before flashing.

---

## 3. HOW TO FLASH (this works reliably)

`esptool` cannot reset these boards over the app's TinyUSB CDC. The working
sequence, discovered the hard way:

```bash
# 1. force download mode (this command "fails" but does the job, and the port
#    disappears and reappears under a new name)
esptool --port <app-port> --before default-reset --after no-reset flash-id

# 2. find the new port, confirm which board it is
ls /dev/cu.usbmodem*
esptool --port <new-port> --chip esp32s3 --before no-reset --after no-reset read-mac

# 3. flash (merged.bin from arduino-cli goes at 0x0)
esptool --port <new-port> --chip esp32s3 --before no-reset --after no-reset \
        write-flash 0x0 <sketch>.ino.merged.bin

# 4. boot it — plain hard-reset does NOT leave download mode, watchdog-reset does
esptool --port <new-port> --chip esp32s3 --before no-reset --after watchdog-reset read-mac
```

`--after hard-reset` leaves the chip stuck in download mode. **Use
`watchdog-reset`.** This cost a lot of time.

Manual fallback if a board ever stops responding: hold BOOT, tap RESET, release
BOOT. Nothing we do here can brick a board — recovery is always USB.

**Do NOT use NRF-OTA** (per Seeed's wiki). Irrelevant to us anyway: we only ever
flash over USB serial, and every write is hash-verified.

Build: `arduino-cli compile -b esp32:esp32:XIAO_ESP32S3 <sketch-dir> --output-dir <dir>/build`
Board defaults are already `USBMode=hwcdc` — keep that, it is far more reliable
than TinyUSB CDC here.

---

## 4. THE MESHTASTIC DEAD END (so nobody re-treads it)

Symptom: `meshtastic --info` times out on BOTH boards, device sends zero bytes
back. No `Meshtastic_*` BLE advert appears (BLE scanning otherwise works fine —
15 other devices seen). Boot log prints, then stops mid-hardware-init forever.

Ruled out, with evidence:
- **Bad flash** — full erase + factory.bin @ 0x0 + littlefs @ 0x670000, both
  hash-verified. No change.
- **Wrong board/variant metadata** — `hwModel 81`, `seeed-xiao-s3`, 8 MB scheme
  all match the manifest.
- **Another process holding the port** — `lsof` clean. (Caveat: the Arc browser
  DID transiently hold board B's port via WebSerial at one point and invalidated
  one test. Re-run cleanly gave the same result. Watch for this.)
- **Boot loop** — enumeration stable, 30/30 seconds present.
- **Client timing out too early** — patched `_waitConnected` to 180 s. Still
  nothing. So it is not the known `ARDUINO_USB_MODE=0` slow-poll bug.
- **Host/DTR/macOS problems** — our own Arduino firmware talks over the same USB
  perfectly, so the host side is fine.

Actual cause: Meshtastic blocks bringing up an SX1262 on GPIO 41/42/40/39, where
there is no radio. See §1.

Firmware images are cached in `firmware/` (Meshtastic 2.7.26.54e0d8d, esp32s3
bundle) if we ever want to go back.

---

## 5. CURRENT STATE

Both boards run `diagnostics/lora_pingpong` — identical firmware, each deriving
its node id from its own MAC.

**Radio link is PROVEN.** An earlier interrupt-driven build had both boards
receiving each other at RSSI −40 / −44.5 dBm. Real packets, real RF.

Known-good so far:
- SPI, CS, RESET, BUSY pins — confirmed by `begin()` and by actual RF exchange
- 868 MHz TX and RX both work
- Node ids now distinct: `30D1`, `44D1`

Open bugs in the current build:
- `rx=0` since switching to polled receive. Almost certainly because both boards
  were reset simultaneously, so their 5 s beacons are synchronised and each
  transmits exactly while the other transmits. **Fix: stagger/jitter the beacon
  per node.** Not yet done.
- `DIO1 = GPIO4` is unconfirmed. `begin()` proves SPI/BUSY but not DIO1. The
  interrupt build retriggered hundreds of times per beacon with empty payloads,
  which suggests DIO1 is wrong or stuck. Current build polls over SPI to dodge
  this. Needs resolving before any sleep/low-power work.
- GPIO3 (D2) reads high, never driven. Believed to be the module's button.
  Must NOT be driven as an output — pressing it would short to ground.

### Mistakes made — don't repeat
- Claimed "the firmware hangs" before running the cheap tests that would have
  challenged it. The user pushed back and was right to.
- Guessed GPIO3 was an RF switch and drove it as an output. It is probably a
  button.
- Took the wrong 16 bits of `ESP.getEfuseMac()` for the node id — it returns the
  MAC bytes REVERSED, so the low bits are the shared vendor prefix and both
  boards called themselves `BACC`. The distinguishing bytes are bits 32..47.
- `cd` inside a Bash call can reset the shell cwd; a `mkdir` then landed a
  sketch in the parent directory. Use absolute paths.

---

## 6. PLAN

1. ~~Identify hardware, get a board talking~~ — done
2. ~~Find the real pin map~~ — done, §1
3. ~~Prove the two radios can talk~~ — done (RSSI −40 dBm)
4. **Fix the beacon collision, get a clean sustained two-way link** ← HERE
5. Decide the architecture (see below)
6. LED strip + capacitive touch on one board
7. Colour sync protocol, both lamps converge
8. Enclosures / mains power

### Architecture decision, still open
- **Custom Meshtastic firmware** with a corrected variant: keeps meshing,
  encryption, the phone app. Costs a PlatformIO build and ongoing rebasing.
- **Our own RadioLib firmware**: far simpler, much lower latency, one MCU does
  radio + LEDs + touch. Loses meshing and the app — but for two lamps in one
  building, meshing buys little.

Leaning RadioLib, because stock Meshtastic does not work on this hardware
anyway, so the "it just works" argument for Meshtastic is already gone. Discuss
with the user before committing.

### Still needed from the user
- LED strip details: SK6812 RGBW vs WS2812, LED count, power supply
- Capacitive touch: the ESP32-S3 has native touch on GPIO1-14, but most of those
  are taken here. Free pads look like GPIO6 (D5) and possibly GPIO3 (D2, if that
  is not the button). May need an external TTP223.
- Conflict rule when both lamps are touched at once (suggest last-touch-wins
  with a sequence number)

---

## 7. REPO LAYOUT

```
diagnostics/        bring-up sketches, kept because they document the hardware
  sx1262_probe/     hand-rolled SPI probe + I2C bus check
  radiolib_probe/   second opinion via RadioLib
  pin_scan/         floating-pin survey + SPI CS/MISO sweep — found the real pins
  pinmap_hunt/      brute-forces reset/busy/dio1 until begin() succeeds
  lora_pingpong/    two-board 868 MHz link test
docs/               Meshtastic reference compiled from the official docs
firmware/           cached Meshtastic 2.7.26 images (gitignored)
.venv/              python env: meshtastic CLI + pyserial + bleak (gitignored)
```

Useful commands:
```bash
# who is plugged in
ls /dev/cu.usbmodem*; ioreg -p IOUSB -l -w 0 | grep -E '"USB Product Name"|"USB Serial Number"'
# watch a board
arduino-cli monitor -p <port> -c baudrate=115200
```


---

## 8. RADIO DEEP-DIVE (session 1, later) — READ THIS BEFORE TOUCHING THE RADIO

### Confirmed hard facts

- **Chip identity: the version register 0x0320 reads ASCII `"SX1261"`**, not SX1262.
  Read it yourself: `ReadRegister(0x0320)`. This changes PA configuration and caps
  power at +15 dBm rather than +22.
- **NSS/CS is GPIO5 (D4).** Proved by gating: held HIGH the chip returns 0x0000,
  toggled or held LOW it answers. It genuinely selects the chip.
- **SPI works perfectly.** MODE0 (MODE3 identical). Deterministic: 40/40 identical
  reads at every clock from 100 kHz to 8 MHz. An earlier "flaky link" conclusion
  was WRONG — retracted.
- **Framing:** `ReadRegister` = `0x1D, addrHi, addrLo, <1 dummy byte>, data...`.
  `GetStatus` = `0xC0, <status>`. Verified by raw byte dump in
  `diagnostics/spi_dump`.
- **Writes and commands DO execute.** `WriteRegister(0x0740, AB CD)` reads back
  `AB CD`. `SetPacketType(LORA)` then `GetPacketType` returns 0x01. So the bus and
  command path are not the problem.
- **The chip never leaves `STBY_RC`.** `SetStandby(STBY_XOSC)`, `Calibrate(0x7F)`
  and `SetTx` all change nothing, and `GetDeviceErrors` stays `0x0000` throughout
  — no `XOSC_START_ERR`, which is what makes this strange.
- Tried and did NOT help: every TCXO voltage (0/1.6/1.7/1.8/2.2/2.4/2.7/3.0/3.3 V);
  both SX1261 and SX1262 PA configs at +10/+14/+22 dBm; with and without
  TCXO configuration; generous 64 ms TCXO timeouts and 150 ms settling.
- Register 0x0740 reads `24 B4`, not the documented default `1424`. Unexplained.

### Retracted claims — do not repeat them

- ~~"The two boards exchanged packets at RSSI −40 dBm"~~ — FALSE. Those RX events
  had empty payloads and byte-identical RSSI/SNR: stale registers. **No packet has
  ever been genuinely received.**
- ~~"The SPI link is intermittent / slipping bytes"~~ — FALSE, it is deterministic.
- ~~"Meshtastic firmware hangs in setup()"~~ — the observable facts (no BLE, no API
  response) are real, but the cause is the wrong radio pin map, not a hang per se.

### Where the radio investigation stands

TX has never actually happened. The chip accepts and executes everything except
entering FS/TX/RX. Remaining candidates, in order:
1. RadioLib's `SX1261` class (not `SX1262`) with its correct init ORDER —
   calibration after TCXO setup, `CalibrateImage` after frequency. My hand-rolled
   sequence may have the order wrong. **Try this next.**
2. A module enable/power pin we have not identified (GPIO3/D2 is still unexplained
   — it reads driven-high; the user reports a button on the radio module).
3. Genuine oscillator hardware fault — but the total absence of `XOSC_START_ERR`
   argues against it.

Get the real Seeed schematic for "Wio-SX1262 for XIAO" before assuming hardware
failure. The wiki links a schematic PDF; the datasheet in the repo root is for the
bare 12-pin module only and does NOT give XIAO pin mapping.

### Board A recovery — solved

Board A's failure to flash was **a bad USB cable**, not the board. It dropped
mid-write, corrupted the image, then could not carry a clean recovery connection.
A new cable fixed it immediately. If a board enumerates as "Generic CDC" with no
serial number, suspect the cable first.

Manual download mode (needed when firmware is corrupt): hold **B** (BOOT) on the
**XIAO** — not the button on the radio module — then plug USB in, hold ~2 s, release.

### Reading serial reliably

The native-USB CDC enumerates AFTER `setup()` runs, so anything printed once is
usually lost. **Every diagnostic sketch buffers its report into a `String` and
reprints it from `loop()`.** Keep doing that; it is the only reliable pattern here.

### Diagnostics written (all in diagnostics/, all keep working)

| sketch | what it establishes |
|---|---|
| `sx1262_probe` | I2C bus healthy, no I2C devices; first hand-rolled SPI probe |
| `radiolib_probe` | second opinion via RadioLib |
| `pin_scan` | floating-pin survey + CS sweep — found CS on GPIO5 |
| `pinmap_hunt` | brute-forces rst/busy/dio1 until `begin()` succeeds |
| `dio1_finder` | DIO1 does not track TxDone on any pin |
| `raw_irq` | chip stays in STBY_RC, never enters TX |
| `tcxo_sweep` | no TCXO voltage helps; no device errors |
| `osc_test` | raw state-machine walk, hand-rolled commands |
| `cs_verify` | GPIO5 genuinely gates the chip |
| `spi_stability` | link is deterministic at all clocks |
| `spi_dump` | raw byte dump — found "SX126" string, fixed framing |
| `cmd_test` | writes and commands DO execute; version = SX1261 |
| `pa_config` | neither SX1261 nor SX1262 PA config reaches TX |
| `identify_blink` | blinks board B's LED to tell the two boards apart |
| `lora_pingpong`, `lora_roles` | two-board link attempts (no RX yet) |


---

## 9. *** SOLVED: THE RADIO WORKS *** (read this first)

### The confirmed, working pin map

```
SCK  = GPIO7  (D8)
MISO = GPIO8  (D9)
MOSI = GPIO9  (D10)
NSS  = GPIO5  (D4)
RST  = GPIO3  (D2)   <-- THIS was the bug. Not GPIO1.
BUSY = GPIO4  (D3)
DIO1 = GPIO2  (D1)
TCXO = 1.8 V on the SX1262's own DIO3
chip = reports "SX1261" in register 0x0320; the SX1262 RadioLib class works fine
```

Proof of life:
```
tcxo 1.8V  errReset=0x0020  errInit=0x0000  XOSC=YES  TX=YES  DONE=YES  23ms
```

### The root cause, and why it hid for so long

**RST is GPIO3, not GPIO1.** Everything else followed from that one wrong assumption:

- Because RST was never actually asserted, **the chip was never reset**. An SX126x
  that has not been reset sits in an undefined state: it happily answers SPI reads,
  accepts and executes register writes and `SetPacketType`, and reports no errors -
  but it will not start its oscillator, so it can never reach FS/TX/RX.
- `GetDeviceErrors` returned a clean `0x0000` throughout, which made a clock fault
  look impossible. The moment a real reset was applied, the very same command
  returned `0x0020` = **`XOSC_START_ERR`** - the error had simply never been latched
  because the chip had never gone through a reset cycle.
- How RST was finally identified: write a marker into register 0x0740, pulse each
  candidate pin low, read it back. Only the true reset line restores the documented
  default `0x1424`. GPIO3 did; nothing else did. That test is
  `diagnostics/map_signals`.

Two further traps that wasted time and produced false conclusions:

- **RadioLib's blocking calls were meaningless** while DIO1 was wrong. `transmit()`
  waits for DIO1 to go HIGH; GPIO4 sits permanently HIGH, so `transmit()` returned
  `0` in 1 ms without sending anything, and the interrupt-driven receiver fired
  hundreds of times per second with empty payloads and identical RSSI/SNR. That is
  the source of the bogus "RSSI -40 dBm, they're talking!" claim. **Never trust a
  RadioLib return code until DIO1 is verified.** Verify against the chip: raw
  `GetStatus` (0xC0) for chip mode and `GetIrqStatus` (0x12) for TX_DONE.
- GPIO3 was briefly suspected of being the module's button and therefore avoided as
  an output. It is RST. **The button is `K1` (TS-1185E) on GPIO21**, with a 10K
  pull-up, per the Seeed schematic. The green LED is on GPIO48.

### The hardware, correctly understood

From `docs/Schematic_Diagram_Wio-SX1262_for_XIAO.pdf`: the Wio-SX1262 board carries
the module plus a 30-pin B2B connector (J3) **and two 1x7 2.54 mm breakout headers
(J1/J2)** exposing `DIO1, BUSY, RST, NSS, RF_SW1, SCK, MOSI, MISO, 3V3, VIN, GND`.
This build is wired through those headers, which is exactly why the pin map does not
match Meshtastic's `seeed_xiao_s3` variant (`cs=41 rst=42 busy=40 dio1=39`) - those
pins are on the B2B connector and read as floating here.

`RF_SW1` exists on the header but did NOT need driving for transmission to work.

### Consequence for Meshtastic

Custom firmware is still required - stock `seeed_xiao_s3` drives the wrong pins -
but now the exact variant edit is known. Change in a copied variant:

```c
#define LORA_SCK   7
#define LORA_MISO  8
#define LORA_MOSI  9
#define LORA_CS    5
#define LORA_RESET 3
#define SX126X_BUSY 4
#define LORA_DIO1  2
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
// drop SX126X_RXEN / DIO2_AS_RF_SWITCH pin 38 - not wired here
// button is GPIO21, LED is GPIO48
```

PlatformIO is installed at `.pio-venv/bin/pio`; the 2.7.26 source is cloned in
`meshtastic-firmware/`.


### Final pin map (all four control lines now measured, not guessed)

```
SCK  = GPIO7 (D8)     MISO = GPIO8 (D9)    MOSI = GPIO9 (D10)
NSS  = GPIO5 (D4)     RST  = GPIO3 (D2)
BUSY = GPIO4 (D3)     DIO1 = GPIO2 (D1)
TCXO = 1.8 V via the chip's own DIO3
GPIO1 and GPIO6 remain unassigned; one is RF_SW1. Neither needed driving.
```

How BUSY and DIO1 were finally pinned down (`diagnostics/busy_dio1`), and why the
earlier attempt was worthless: the first BUSY hunt ran before RST was known, so the
chip had never been reset, `Calibrate` never actually executed, and no pin ever
pulsed. Redone after a real reset:

- **BUSY = GPIO4** — held HIGH for the entire `Calibrate(0x7F)` window, LOW after.
- **DIO1 = GPIO2** — `0 -> 1` exactly when TX_DONE latched, `-> 0` when the IRQ was
  cleared. Nothing else moved.
- TX_DONE latched in **194 ms**, a credible SF9/BW250 time-on-air. Earlier runs
  reporting 1-2 ms were RadioLib returning instantly because it was waiting on a
  DIO1 that sat permanently high.

Working intermediate values before this fix were `BUSY=2, DIO1=6` — **both wrong**.
With BUSY misassigned, every RadioLib wait is meaningless: the receiver silently
failed to enter RX mode and chip-mode reads lagged seconds behind reality.

**Lesson worth keeping: on this board, verify against the chip, not the library.**
Raw `GetStatus` (0xC0) for mode, `GetIrqStatus` (0x12) for IRQs, `GetDeviceErrors`
(0x17) for faults. Framing is `opcode, <1 dummy byte>, data...`.


---

## 10. *** TWO-WAY LINK VERIFIED *** (2026-08-18)

Both boards exchange real LoRa packets. Evidence, not optimism:

```
[TX 15] "PING 30D1 seq=15"  TX_DONE after 91 ms   mode=STBY_RC err=0x0000
[RX 16] len=16 "PING 30D1 seq=15"  rssi=-17.0 dBm  snr=11.0 dB  <<< REAL PACKET
```

- 16 of 16 packets received, zero loss, over 30 s
- sequence numbers advance monotonically; payload matches byte for byte
- TX_DONE at 91 ms - the correct time-on-air for SF9/BW250/CR4-5 at this length
- RSSI -17 dBm, SNR +11 dB at desk distance

Contrast with the false positive early in the session (identical RSSI/SNR on every
"packet", empty payloads, hundreds per second). The difference between real and
phantom reception is: non-empty payload, advancing sequence, plausible time-on-air,
and slight RSSI/SNR jitter. Demand all four.

Working radio settings: 868.0 MHz, BW 250 kHz, SF9, CR 4/5, preamble 8,
sync word `RADIOLIB_SX126X_SYNC_WORD_PRIVATE`, +14 dBm (EU 868 ERP limit),
`setDio2AsRfSwitch(true)`, CRC on. RadioLib's `SX1262` class works despite the chip
reporting "SX1261". `GPIO1`/`GPIO6` were driven HIGH during this test but are NOT
required - the link works without them.

Reference implementation: `diagnostics/link_test/link_test.ino`.

### Next steps
1. Two-way (both nodes TX and RX) rather than fixed roles
2. Then either the colour-sync protocol on RadioLib, or the custom Meshtastic
   variant using the pin map above - user chose custom Meshtastic, and the exact
   variant edit is written out in section 9
3. LEDs and capacitive touch. Free pins now: GPIO1, GPIO6, GPIO21 is the module
   button, GPIO48 the module LED. Note GPIO43/44 (D6/D7) are also free.


---

## 11. *** MESHTASTIC WORKING END-TO-END *** (2026-08-18)

Custom variant `xiao-lamp` built from meshtastic/firmware v2.7.26.54e0d8d and
flashed to both boards. Source of truth for the variant is `meshtastic-variant/`
in this repo (the `meshtastic-firmware/` clone is gitignored).

```
Connected to radio
Owner: Meshtastic d144 (d144)     pioEnv: "xiao-lamp"     hasBluetooth: true
lora.region: 3 (EU_868)
Primary channel URL: https://meshtastic.org/e/#CgcSAQE6AggNEhkIARj6ASALKAU4A0ADSAFQG2gBwAYB0AYC
```

Mesh formed, both nodes present, direct contact:
```
1  Meshtastic d144  !9716d144
2  Meshtastic d130  !9716d130   SNR 6 dB   Hops 0
```

Broadcast delivered between them:
```
*** RECEIVED on d130: 'LAMP BROADCAST TEST'  from=!9716d144  rssi=-31  snr=6.0
```

Both advertise over BLE as `Meshtastic_d130` / `Meshtastic_d144`; the phone app
pairs with the fixed PIN **123456** (fixed rather than random because no screen is
attached).

### Gotchas found here

- **Direct messages fail with `PKI_UNKNOWN_PUBKEY`.** Meshtastic 2.5+ encrypts DMs
  with per-node public keys, and these two have not exchanged keys. **Broadcast on
  the shared channel works fine and is what the lamps want anyway** - both lamps
  should hear every colour change. Do not burn time on DMs.
- **Board A intermittently enumerates without a USB serial node.** It stays fully
  alive on BLE and LoRa; only `/dev/cu.*` is missing. A replug restores it. Do not
  assume the board has failed - check the BLE scan first.
- `hw_model` stays 81 so both report as `PRIVATE_HW`. Expected for an out-of-tree
  variant, harmless.
- Setting `lora.region` reboots the node and the USB port re-enumerates; a read
  issued immediately after will fail with `[Errno 6] Device not configured`. Wait
  ~15 s.

### Still open
- Both lamps sit on the **default public LongFast channel**. For a private pair,
  create a channel with a random PSK and share the channel URL between them.
- Colour payload protocol not yet built.


---

## 12. LAMP FIRMWARE (raw LoRa) - working

`firmware/lamp/` is a port of the Pico/MQTT project
(github.com/fionnf/linked_friend_lights) to XIAO ESP32-S3 + SX1262.

**Transport decision: raw LoRa via RadioLib, NOT Meshtastic.** Measured on this
hardware, same payload, same two boards:

| transport | latency | delivery |
|---|---|---|
| Meshtastic (LongFast) | 7-20 s, mean 15 s | 5/5 |
| Meshtastic (SHORT_FAST, hop 1) | 4.7-19.8 s, mean 10.8 s | 5/6 |
| **raw LoRa (this firmware)** | **203-218 ms, mean 211 ms** | **5/5** |

Meshtastic's delay is its transmit scheduling, not airtime - the radio itself
completes a transmission in 91 ms. The Meshtastic build still exists
(`meshtastic-variant/`) for the phone app and mesh range; it is a separate
firmware for the same hardware.

### Sync design: broadcast the SEED, not the scene

The colour engine randomises group sizes, hues and white levels on every tap. The
MQTT build sent the resulting scene; over LoRa that is expensive. Instead both
lamps run the same deterministic generator from the same 32-bit seed, so four
bytes reproduce an identical scene on both ends - randomness included.

This means the PRNG must be explicit and identical everywhere: `colour.h` uses
xorshift32, never `rand()`, whose sequence differs between implementations.

Wire format, 11 bytes: magic `0xC1`, type (1=SCENE, 2=POWER), counter u32, seed
u32, flags. Conflict resolution is last-write-wins on a Lamport counter with the
higher node id breaking ties, so two lamps tapped simultaneously converge instead
of ping-ponging.

### Pins (39 SK6812 RGBW)

```
D6 (GPIO43) --[330R]--> DIN      D7 (GPIO44) --+--> pad
                                                +--[1M]--> GND
```
D0/D5 are deliberately unused: one of them is RF_SW1 on the LoRa header.
The Meshtastic variant had I2C on GPIO43/44 and its boot scan would have driven
the LED data line - removed, so **the boards need the current variant build**.

### Serial control
`tap`, `seed <n>`, `power`, `pos <0..1>`, `status`, `cal` at 115200.

### Gotchas
- **Do not name a global `link`** - POSIX `link(2)` from `unistd.h` shadows it and
  the errors point at the wrong thing entirely.
- Touch reads the 20001 safety cap when no pad/1M resistor is attached. That is
  the "nothing connected" signature, not a fault.
- After flashing, `--before default-reset` puts the chip back INTO download mode.
  Boot with `--before no-reset --after watchdog-reset`.
