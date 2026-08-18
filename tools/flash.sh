#!/usr/bin/env bash
# Flash a lamp with either firmware.
#
#   ./tools/flash.sh fast          low latency, direct radio  (~211 ms)
#   ./tools/flash.sh mesh          Meshtastic + lamp module   (mesh range, seconds)
#   ./tools/flash.sh fast <port>   pick a board explicitly
#
# The two builds are a deliberate trade, not duplicates:
#   fast  - talks to the SX1262 directly. Both lamps must hear each other.
#   mesh  - the lamp runs inside Meshtastic, so any other Meshtastic node can
#           relay for it, and the phone app works. Slower, because Meshtastic
#           schedules transmissions rather than sending immediately.
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-}"
PORT="${2:-}"

if [[ "$MODE" != "fast" && "$MODE" != "mesh" ]]; then
  echo "usage: $0 {fast|mesh} [port]"; exit 1
fi

if [[ -z "$PORT" ]]; then
  mapfile -t PORTS < <(ls /dev/cu.usbmodem* 2>/dev/null | grep -v SN2345 || true)
  if [[ ${#PORTS[@]} -eq 0 ]]; then echo "no board found"; exit 1; fi
  if [[ ${#PORTS[@]} -gt 1 ]]; then
    echo "several boards connected; pass one explicitly:"; printf '  %s\n' "${PORTS[@]}"; exit 1
  fi
  PORT="${PORTS[0]}"
fi

echo "==> $MODE  ->  $PORT"

# esptool cannot reset this board over the app's USB CDC, so force download mode
# first. The command "fails" and the port reappears under a new name - expected.
esptool --port "$PORT" --before default-reset --after no-reset flash-id >/dev/null 2>&1 || true
sleep 4
PORT="$(ls -t /dev/cu.usbmodem* | grep -v SN2345 | head -1)"
echo "    download mode on $PORT"

if [[ "$MODE" == "fast" ]]; then
  arduino-cli compile -b esp32:esp32:XIAO_ESP32S3 firmware/lamp \
      --output-dir firmware/lamp/build >/dev/null
  esptool --port "$PORT" --chip esp32s3 --before no-reset --after no-reset \
      write-flash 0x0 firmware/lamp/build/lamp.ino.merged.bin
else
  ( cd meshtastic-firmware && ../.pio-venv/bin/pio run -e xiao-lamp >/dev/null )
  B=meshtastic-firmware/.pio/build/xiao-lamp
  esptool --port "$PORT" --chip esp32s3 --before no-reset --after no-reset erase-flash
  esptool --port "$PORT" --chip esp32s3 --before no-reset --after no-reset write-flash \
      0x0 "$B"/firmware-xiao-lamp-*.factory.bin \
      0x670000 "$B"/littlefs-xiao-lamp-*.bin
fi

# A plain hard reset leaves this chip in download mode. watchdog-reset boots it.
esptool --port "$PORT" --chip esp32s3 --before no-reset --after watchdog-reset read-mac >/dev/null
echo "==> done, board booting"
