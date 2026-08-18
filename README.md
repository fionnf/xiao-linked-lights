# xiao-linked-lights

Two lamps that always show the same colour. Touch one, both change.

Each lamp is a **Seeed XIAO ESP32-S3** with a **Wio-SX1262** LoRa module, an
addressable RGBW LED strip, and a capacitive touch sensor. The two lamps talk to
each other directly over 868 MHz LoRa, so they work with no wifi, no hub and no
internet — just power.

## Status

**The radio link works.** Two boards exchange packets reliably:

```
[TX 15] "PING 30D1 seq=15"  TX_DONE after 91 ms
[RX 16] len=16 "PING 30D1 seq=15"  rssi=-17.0 dBm  snr=11.0 dB
```

16 of 16 packets, zero loss, RSSI −17 dBm, SNR +11 dB at desk distance.

Still to build: the colour-sync protocol, the LED driver, and the touch sensor.

## The thing that cost a day, so nobody repeats it

**Stock Meshtastic firmware cannot drive this hardware.** Meshtastic's
`seeed_xiao_s3` variant expects the SX1262 on GPIO 41/42/40/39, reached through the
Wio board's 30-pin board-to-board connector. This build is wired through the
module's **1×7 breakout headers** instead, so those pins read as floating and
Meshtastic sits there talking to nothing — no BLE advertisement, no response on the
serial API, no error message.

Worse, the pin that actually mattered was **RST**. It is on **GPIO3**, and until
that was found the chip had never once been reset. An SX126x that has not been
reset will happily answer SPI reads, accept register writes and execute
`SetPacketType` — while silently refusing to start its oscillator, so it can never
transmit. `GetDeviceErrors` reported a clean `0x0000` throughout, because the error
bit is only latched during a reset cycle. The moment a real reset was applied, the
same command returned `0x0020` — `XOSC_START_ERR` — and everything fell into place.

### Verified pin map

| Signal | GPIO | XIAO pad |
|---|---|---|
| SCK  | 7 | D8 |
| MISO | 8 | D9 |
| MOSI | 9 | D10 |
| NSS  | 5 | D4 |
| **RST** | **3** | **D2** |
| **BUSY** | **4** | **D3** |
| **DIO1** | **2** | **D1** |
| TCXO | 1.8 V via the chip's own DIO3 | — |

The module's button is on GPIO21 and its green LED on GPIO48. GPIO1 and GPIO6 are
unassigned; one of them is `RF_SW1`, and neither needs driving for the link to work.

The chip reports `"SX1261"` in register `0x0320`. RadioLib's `SX1262` class drives
it correctly anyway.

## Radio settings

868.0 MHz · BW 250 kHz · SF9 · CR 4/5 · preamble 8 · private sync word · +14 dBm
(the EU 868 ERP limit) · `setDio2AsRfSwitch(true)` · CRC on.

## Layout

```
diagnostics/     bring-up sketches, kept because they document the hardware
  link_test/     the working two-board link — start here
  busy_dio1/     identifies BUSY and DIO1 by experiment
  map_signals/   identifies RST by experiment (the key discovery)
  spi_dump/      raw SPI byte dump; found the chip's "SX126" version string
  ...            and the dead ends, each recording what it ruled out
docs/            Meshtastic reference, Seeed schematic, module datasheet
CLAUDE.md        full engineering log — read this before changing anything
```

## Getting started

```bash
# build and flash a sketch
arduino-cli compile -b esp32:esp32:XIAO_ESP32S3 diagnostics/link_test \
    --output-dir diagnostics/link_test/build

# the board has no USB-UART bridge, so it changes device node between modes.
# force download mode (this command "fails" but does the job):
esptool --port <app-port> --before default-reset --after no-reset flash-id
ls /dev/cu.usbmodem*                       # find the new node
esptool --port <new-port> --chip esp32s3 --before no-reset --after no-reset \
        write-flash 0x0 <sketch>.ino.merged.bin
# NOTE: plain hard-reset leaves the chip in download mode. Use watchdog-reset:
esptool --port <new-port> --chip esp32s3 --before no-reset --after watchdog-reset read-mac
```

If a board stops responding, hold **B** (BOOT) *on the XIAO*, plug USB in, release.
If it enumerates as "Generic CDC" with no serial number, suspect the **USB cable**
before the board — a flaky cable corrupted one board mid-flash during development.

## A note on verifying radio work

Early in this project two boards appeared to be exchanging packets at RSSI −40 dBm.
They were not. Every "packet" had an empty payload and byte-identical RSSI/SNR:
RadioLib was waiting on a DIO1 pin that sat permanently high, so every call
returned success instantly. Real reception has a non-empty payload, an advancing
sequence number, a plausible time-on-air, and slight RSSI/SNR jitter. Check the
chip directly — `GetStatus` (0xC0), `GetIrqStatus` (0x12), `GetDeviceErrors` (0x17)
— rather than trusting a library return code.
