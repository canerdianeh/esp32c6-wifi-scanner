#!/usr/bin/env python3
"""Sends a test pattern to the node over sACN or Art-Net, for bring-up without a console.

  python send_test.py sacn 1                 # universe 1, multicast
  python send_test.py artnet 0 --ip 192.168.4.1
  python send_test.py sacn 3 --chase         # one channel at a time (pixel strips)

The default pattern fades every channel up and down together.
"""
import argparse
import math
import socket
import struct
import time
import uuid


def sacn_packet(universe, data, seq, priority=100, name=b"send_test"):
    cid = uuid.uuid4().bytes if not hasattr(sacn_packet, "cid") else sacn_packet.cid
    sacn_packet.cid = cid
    slots = bytes([0]) + bytes(data)        # start code 0
    dmp_len = 10 + len(slots)
    framing_len = 77 + dmp_len
    root_len = 22 + framing_len
    return (struct.pack("!HH12s", 0x0010, 0, b"ASC-E1.17\0\0\0")
            + struct.pack("!HI16s", 0x7000 | root_len, 0x00000004, cid)
            + struct.pack("!HI64sBHBBH", 0x7000 | framing_len, 0x00000002, name, priority, 0, seq, 0, universe)
            + struct.pack("!HBBHHH", 0x7000 | dmp_len, 0x02, 0xA1, 0, 1, len(slots))
            + slots)


def artnet_packet(port_address, data, seq):
    data = bytes(data) + (b"\0" if len(data) % 2 else b"")  # length must be even
    return (b"Art-Net\0" + struct.pack("<H", 0x5000) + struct.pack("!H", 14)
            + struct.pack("BBBB", seq, 0, port_address & 0xFF, port_address >> 8)
            + struct.pack("!H", len(data)) + data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("protocol", choices=["sacn", "artnet"])
    ap.add_argument("universe", type=int)
    ap.add_argument("--ip", help="node address (default: sACN multicast / Art-Net broadcast)")
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--chase", action="store_true", help="light one channel at a time")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    if args.protocol == "sacn":
        dest = (args.ip or f"239.255.{args.universe >> 8}.{args.universe & 0xFF}", 5568)
    else:
        dest = (args.ip or "255.255.255.255", 6454)

    seq, t0 = 0, time.time()
    print(f"sending {args.protocol} universe {args.universe} to {dest[0]}, Ctrl-C to stop")
    while True:
        t = time.time() - t0
        if args.chase:
            data = [0] * 512
            data[int(t * 20) % 512] = 255
        else:
            data = [int(127.5 + 127.5 * math.sin(t * 2))] * 512
        seq = (seq + 1) & 0xFF
        pkt = sacn_packet(args.universe, data, seq) if args.protocol == "sacn" else artnet_packet(args.universe, data, seq)
        sock.sendto(pkt, dest)
        time.sleep(1 / args.fps)


if __name__ == "__main__":
    main()
