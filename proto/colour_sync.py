#!/usr/bin/env python3
"""
Colour sync between two lamps, over Meshtastic.

Each lamp broadcasts its colour when touched; every lamp that hears it adopts
that colour. This module is the protocol, testable from a laptop before any LED
or touch hardware exists - the embedded firmware will mirror it exactly.

Run one of these per board, in separate terminals:

    python3 proto/colour_sync.py /dev/cu.usbmodemCCBA9716D1301
    python3 proto/colour_sync.py /dev/cu.usbmodemCCBA9716D1441 --send 255,0,64,0

Design decisions worth knowing:

* **Broadcast, not direct message.** Meshtastic 2.5+ encrypts DMs with per-node
  public keys, and two fresh nodes fail with PKI_UNKNOWN_PUBKEY until they have
  exchanged them. Broadcast needs no PKI, and every lamp should hear every colour
  change anyway - so broadcast is both simpler and more correct here.

* **PRIVATE_APP (portnum 256).** The range reserved for third-party use, so lamp
  traffic never collides with Meshtastic's own apps and other nodes ignore it.

* **Last-write-wins on a Lamport counter.** Two lamps touched at nearly the same
  moment must converge on ONE colour, not oscillate. Each lamp keeps a counter,
  sets it to max(seen)+1 when touched, and adopts a remote colour only if that
  colour carries a higher counter - ties broken by the higher node number. Wall
  clocks are not used: these nodes have no RTC and may have no GPS fix.
"""

import argparse
import struct
import sys
import threading
import time

from pubsub import pub

import meshtastic.serial_interface
from meshtastic import BROADCAST_ADDR
from meshtastic.protobuf import portnums_pb2

# Third-party range. portnums.proto: PRIVATE_APP = 256.
COLOUR_PORT = portnums_pb2.PortNum.PRIVATE_APP
CHANNEL_INDEX = 0

MAGIC = 0xC1          # "colour v1" - lets us change the format later safely
FORMAT = "<BBBBBI"    # magic, r, g, b, w, counter  -> 9 bytes
PAYLOAD_LEN = struct.calcsize(FORMAT)


def encode(rgbw, counter):
    r, g, b, w = rgbw
    return struct.pack(FORMAT, MAGIC, r, g, b, w, counter)


def decode(raw):
    """Return (rgbw, counter) or None if this is not one of our packets."""
    if len(raw) != PAYLOAD_LEN:
        return None
    magic, r, g, b, w, counter = struct.unpack(FORMAT, raw)
    if magic != MAGIC:
        return None
    return (r, g, b, w), counter


class Lamp:
    """The colour state machine. No I/O, so it can be reasoned about and tested."""

    def __init__(self, node_num):
        self.node_num = node_num
        self.colour = (0, 0, 0, 0)
        self.counter = 0
        self.owner = node_num      # which node last set the colour

    def touch(self, rgbw):
        """Local touch: claim the colour with a counter nobody has used yet."""
        self.counter += 1
        self.colour = rgbw
        self.owner = self.node_num
        return encode(rgbw, self.counter)

    def on_remote(self, rgbw, counter, from_node):
        """A colour arrived from another lamp. Adopt it only if it genuinely wins.

        The comparison must happen BEFORE our own counter is advanced, or we would
        be comparing the remote counter against a copy of itself and every message
        would look like a tie.

        Ties are broken by node number so that two lamps touched in the same
        instant pick the SAME winner rather than each preferring its own colour and
        ping-ponging forever.
        """
        wins = counter > self.counter or (counter == self.counter and from_node > self.owner)
        self.counter = max(self.counter, counter)
        if wins:
            self.colour = rgbw
            self.owner = from_node
            return True
        return False


def main():
    ap = argparse.ArgumentParser(description="lamp colour sync over Meshtastic")
    ap.add_argument("port", help="serial port of this lamp's node")
    ap.add_argument("--send", metavar="R,G,B,W",
                    help="simulate a touch: broadcast this colour once connected")
    ap.add_argument("--repeat", type=int, default=0,
                    help="keep sending every N seconds (0 = send once)")
    ap.add_argument("--listen-secs", type=float, default=60.0)
    args = ap.parse_args()

    iface = meshtastic.serial_interface.SerialInterface(args.port)
    my_num = iface.myInfo.my_node_num
    lamp = Lamp(my_num)
    print(f"lamp !{my_num:08x} ready on {args.port}")

    def on_receive(packet, interface):
        dec = packet.get("decoded") or {}
        if dec.get("portnum") != "PRIVATE_APP":
            return
        parsed = decode(dec.get("payload", b""))
        if not parsed:
            return
        rgbw, counter = parsed
        src = packet.get("from")
        changed = lamp.on_remote(rgbw, counter, src)
        print(f"  <- colour {rgbw} counter={counter} from=!{src:08x} "
              f"rssi={packet.get('rxRssi')} snr={packet.get('rxSnr')} "
              f"{'ADOPTED' if changed else 'ignored (older)'}")

    pub.subscribe(on_receive, "meshtastic.receive")

    def broadcast(rgbw):
        raw = lamp.touch(rgbw)
        iface.sendData(raw, destinationId=BROADCAST_ADDR, portNum=COLOUR_PORT,
                       wantAck=False, channelIndex=CHANNEL_INDEX)
        print(f"  -> colour {rgbw} counter={lamp.counter} broadcast ({len(raw)} bytes)")

    if args.send:
        rgbw = tuple(int(x) for x in args.send.split(","))
        assert len(rgbw) == 4, "--send needs R,G,B,W"
        time.sleep(2)          # let the interface settle before first transmit
        broadcast(rgbw)
        if args.repeat:
            def loop():
                while True:
                    time.sleep(args.repeat)
                    r, g, b, w = lamp.colour
                    broadcast(((r + 40) % 256, (g + 90) % 256, (b + 150) % 256, w))
            threading.Thread(target=loop, daemon=True).start()

    time.sleep(args.listen_secs)
    iface.close()
    print(f"final colour {lamp.colour} counter={lamp.counter} owner=!{lamp.owner:08x}")


if __name__ == "__main__":
    main()
