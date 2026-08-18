# Custom Meshtastic variant: `xiao-lamp`

Stock Meshtastic cannot drive this hardware. Its `seeed_xiao_s3` variant expects
the SX1262 on GPIO 41/42/40/39 — reachable only through the Wio board's 30-pin
board-to-board connector. This build is wired through the module's 1×7 breakout
headers, so those pins are floating: Meshtastic finds no radio, never advertises
BLE, and never answers the serial API, with no error anywhere.

This variant fixes the pin map. Everything in `variant.h` was measured on the
bench, not copied — see `../CLAUDE.md` §9 for how each pin was proved.

## Result

```
Connected to radio
Owner: Meshtastic d144 (d144)
My info: { "myNodeNum": 2534854980, "pioEnv": "xiao-lamp", ... }
Metadata: { "firmwareVersion": "2.7.26.54e0d8d", "hasBluetooth": true, ... }
lora.region: 3      (EU_868)
Primary channel URL: https://meshtastic.org/e/#CgcSAQE6AggNEhkIARj6ASALKAU4A0ADSAFQG2gBwAYB0AYC
```

Both nodes advertise over Bluetooth as `Meshtastic_d130` and `Meshtastic_d144`,
so the Meshtastic phone app can see them.

## Building

```bash
git clone --depth 1 --branch v2.7.26.54e0d8d --recurse-submodules \
    https://github.com/meshtastic/firmware.git meshtastic-firmware
cp -r xiao_lamp meshtastic-firmware/variants/esp32s3/
cd meshtastic-firmware && pio run -e xiao-lamp     # ~90 s
```

Artifacts land in `.pio/build/xiao-lamp/`:
`firmware-xiao-lamp-*.factory.bin` (flash at `0x0`) and
`littlefs-xiao-lamp-*.bin` (flash at `0x670000`).

## Flashing

```bash
esptool --port <port> --chip esp32s3 --before default-reset --after no-reset erase-flash
esptool --port <port> --chip esp32s3 --before no-reset --after no-reset write-flash \
    0x0 firmware-xiao-lamp-*.factory.bin 0x670000 littlefs-xiao-lamp-*.bin
esptool --port <port> --chip esp32s3 --before no-reset --after watchdog-reset read-mac
```

Use `watchdog-reset`; a plain hard reset leaves this chip in download mode.

## What changed from `seeed_xiao_s3`, and why

| Setting | Stock | Here | Reason |
|---|---|---|---|
| `LORA_CS` | 41 | **5** | header wiring, proved by chip-select gating |
| `LORA_RESET` | 42 | **3** | proved by register-default restore |
| `SX126X_BUSY` | 40 | **4** | proved by holding high through `Calibrate` |
| `LORA_DIO1` | 39 | **2** | proved by tracking the TX_DONE IRQ |
| `SX126X_RXEN` | 38 | *removed* | no external RXEN; the module switches TX/RX from the chip's own DIO2 |
| `I2C_SDA/SCL` | 5/6 | **43/44** | GPIO5 is the LoRa chip-select here — a direct collision |
| GPS (L76K) | enabled | *removed* | not fitted; its standby pin (GPIO1) collides with the LoRa header |
| `USCREEN_SSD1306` | enabled | *removed* | no screen fitted (the I2C scan finds nothing) |

`hw_model` stays 81, so the device reports as `PRIVATE_HW` — expected for an
out-of-tree variant and harmless.
