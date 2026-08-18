#!/usr/bin/env python3
"""
Colour-sync prototype: listen to EVERY packet, and send raw RGBW bytes on a
custom portnum.

Run:  python3 proto/colour_sync_listen.py /dev/cu.usbmodemXXXX
      python3 proto/colour_sync_listen.py --ble "Lamp A"

Nothing here touches the port unless you actually run it.
"""
import argparse
import sys
import threading
import time

from pubsub import pub

import meshtastic
import meshtastic.serial_interface
import meshtastic.tcp_interface
from meshtastic import BROADCAST_ADDR, LOCAL_ADDR          # "^all" / "^local"
from meshtastic.protobuf import portnums_pb2, mesh_pb2

# ---- our application portnum -------------------------------------------------
# PRIVATE_APP == 256 is the only officially "yours to use" range entry point.
# portnums.proto: PRIVATE_APP = 256, and 256..511 is the private range.
COLOUR_PORT = portnums_pb2.PortNum.PRIVATE_APP          # 256
CHANNEL_INDEX = 0                                        # PRIMARY

connected = threading.Event()


# ---- receive path ------------------------------------------------------------
def onReceive(packet, interface):
    """Subscribed to 'meshtastic.receive' -> fires for EVERY packet type.

    NOTE: this runs on the library's 'publishing' daemon thread, not main.
    Exceptions here are swallowed+printed by DeferredExecution._run().
    """
    dec = packet.get("decoded")          # absent if the packet could not be decrypted
    print("---- packet ----")
    print(f"  from      : {packet.get('from')}   fromId={packet.get('fromId')}")
    print(f"  to        : {packet.get('to')}     toId={packet.get('toId')}")
    print(f"  id        : {packet.get('id')}")
    print(f"  channel   : {packet.get('channel', 0)}")
    print(f"  rxSnr     : {packet.get('rxSnr')}   rxRssi={packet.get('rxRssi')}")
    print(f"  hopLimit  : {packet.get('hopLimit')}  hopStart={packet.get('hopStart')}")
    print(f"  viaMqtt   : {packet.get('viaMqtt')}  pkiEncrypted={packet.get('pkiEncrypted')}")
    if dec is None:
        print("  decoded   : <none - encrypted for a channel we don't hold>")
        return
    portnum = dec.get("portnum")         # str name ("TEXT_MESSAGE_APP") or int (256)
    payload = dec.get("payload")         # ALWAYS raw bytes (library re-injects them)
    print(f"  portnum   : {portnum!r}")
    print(f"  payload   : {payload!r}  ({len(payload)} bytes)")
    if "text" in dec:
        print(f"  text      : {dec['text']!r}")

    # our own colour packets
    if portnum in (COLOUR_PORT, "PRIVATE_APP") and payload and len(payload) == 4:
        r, g, b, w = payload
        print(f"  >>> COLOUR r={r} g={g} b={b} w={w}")


def onConnection(interface, topic=pub.AUTO_TOPIC):
    print(f"[connection] {topic.getName()}  myNodeNum={interface.myInfo.my_node_num}")
    connected.set()


def onLost(interface):
    print("[connection] lost")


def onNodeUpdated(node, interface):
    print(f"[node] {node.get('num')} {node.get('user', {}).get('id')}")


# ---- send path ---------------------------------------------------------------
def send_colour(iface, rgbw, dest=BROADCAST_ADDR):
    """Send 4 raw bytes on PRIVATE_APP. Max payload is 233 bytes (DATA_PAYLOAD_LEN)."""
    return iface.sendData(
        bytes(rgbw),                      # data: bytes OR a protobuf message
        destinationId=dest,               # BROADCAST_ADDR / "!a1b2c3d4" / 0xa1b2c3d4 / LOCAL_ADDR
        portNum=COLOUR_PORT,
        wantAck=(dest != BROADCAST_ADDR), # ACK only makes sense unicast
        wantResponse=False,
        onResponse=None,                  # def cb(packet): ...
        onResponseAckPermitted=False,
        channelIndex=CHANNEL_INDEX,
        hopLimit=3,                       # None -> uses lora.hop_limit from the node
        priority=mesh_pb2.MeshPacket.Priority.RELIABLE,   # 70; default for sendData
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None, help="serial device path")
    ap.add_argument("--ble", default=None)
    ap.add_argument("--host", default=None)
    ap.add_argument("--send", default=None, help="R,G,B,W to send once connected")
    ap.add_argument("--dest", default=BROADCAST_ADDR)
    a = ap.parse_args()

    # Subscribe BEFORE constructing the interface: the connect+nodedb download
    # happens inside the constructor when connectNow=True (the default).
    pub.subscribe(onReceive, "meshtastic.receive")
    pub.subscribe(onConnection, "meshtastic.connection.established")
    pub.subscribe(onLost, "meshtastic.connection.lost")
    pub.subscribe(onNodeUpdated, "meshtastic.node.updated")

    if a.ble:
        from meshtastic.ble_interface import BLEInterface
        iface = BLEInterface(a.ble)                      # address OR name; None = "any"
    elif a.host:
        iface = meshtastic.tcp_interface.TCPInterface(a.host)
    else:
        # devPath=None auto-probes and will refuse if >1 port is present.
        iface = meshtastic.serial_interface.SerialInterface(devPath=a.port)

    try:
        connected.wait(timeout=30)
        if a.send:
            rgbw = [int(x) for x in a.send.split(",")]
            p = send_colour(iface, rgbw, a.dest)
            print(f"sent packet id={p.id} to={p.to:#010x}")
        print("listening (ctrl-c to stop)…")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        iface.close()


if __name__ == "__main__":
    sys.exit(main())
