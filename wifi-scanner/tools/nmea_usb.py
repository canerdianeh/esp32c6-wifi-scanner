#!/usr/bin/env python3
"""
Send GPS position (NMEA) to the scanner over its USB port.

The scanner uses NMEA lines arriving on its USB serial port like a GPS module
(a real module still wins; USB wins over a phone). See main/sdlog.c.

    python tools/nmea_usb.py COM5 --gps COM7              # forward a GPS on this computer
    python tools/nmea_usb.py COM5 --gps COM7 --baud 9600
    python tools/nmea_usb.py COM5 --fixed 41.948437 -87.655334 --alt 181
    python tools/nmea_usb.py COM5 --file track.nmea        # replay a log, one fix per second

The scanner's log output is printed while it runs (use --quiet to hide it), so
this doubles as a serial monitor. Close any other monitor on the port first;
sdlog.py can't run at the same time. Needs pyserial (included in the ESP-IDF
Python environment).
"""
import argparse
import datetime
import sys
import time

import serial

BAUD = 115200   # ignored by USB-Serial-JTAG, but pyserial wants a value


def open_port(port, baud=BAUD):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0
    # Leave DTR/RTS released so opening the port doesn't reset the board.
    s.dtr = False
    s.rts = False
    s.open()
    return s


def sentence(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return f"${body}*{c:02X}\r\n"


def coord(deg, is_lat):
    a = abs(deg)
    d = int(a)
    m = (a - d) * 60
    hemi = ("N" if deg >= 0 else "S") if is_lat else ("E" if deg >= 0 else "W")
    return f"{d:02d}{m:08.5f},{hemi}" if is_lat else f"{d:03d}{m:08.5f},{hemi}"


def fixed_nmea(lat, lon, alt, hdop):
    t = datetime.datetime.now(datetime.timezone.utc)
    hms = t.strftime("%H%M%S.") + f"{t.microsecond // 10000:02d}"
    dmy = t.strftime("%d%m%y")
    la, lo = coord(lat, True), coord(lon, False)
    return (sentence(f"GPGGA,{hms},{la},{lo},1,08,{hdop:.1f},{alt:.1f},M,,M,,") +
            sentence(f"GPRMC,{hms},A,{la},{lo},0.00,,{dmy},,,A") +
            sentence(f"GPGSA,A,3,,,,,,,,,,,,,,{hdop:.1f},"))


def epochs(path):
    """Yield groups of NMEA lines, one group per fix (a new GGA/RMC time starts a group)."""
    group, last_time = [], None
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("$"):
                continue
            fields = line.split(",")
            if fields[0][3:6] in ("GGA", "RMC") and len(fields) > 1:
                if last_time is not None and fields[1] != last_time and group:
                    yield group
                    group = []
                last_time = fields[1]
            group.append(line + "\r\n")
    if group:
        yield group


def main():
    ap = argparse.ArgumentParser(description="Send NMEA to the scanner over USB.")
    ap.add_argument("port", help="scanner's USB port, e.g. COM5")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--gps", metavar="PORT", help="forward a GPS receiver on this port")
    src.add_argument("--fixed", nargs=2, type=float, metavar=("LAT", "LON"), help="send a fixed position")
    src.add_argument("--file", help="replay an NMEA log at one fix per second")
    ap.add_argument("--baud", type=int, default=9600, help="GPS receiver baud rate (default 9600)")
    ap.add_argument("--alt", type=float, default=0.0, help="altitude for --fixed, meters")
    ap.add_argument("--accuracy", type=float, default=5.0, help="accuracy for --fixed, meters (default 5)")
    ap.add_argument("--quiet", action="store_true", help="don't print the scanner's log output")
    args = ap.parse_args()

    dev = open_port(args.port)
    gps = open_port(args.gps, args.baud) if args.gps else None
    replay = epochs(args.file) if args.file else None
    print(f"Sending NMEA to {args.port}; Ctrl+C to stop.", file=sys.stderr)

    gps_buf = b""
    next_send = 0.0
    sent = 0
    try:
        while True:
            out = dev.read(4096)
            if out and not args.quiet:
                sys.stdout.write(out.decode("utf-8", "replace"))
                sys.stdout.flush()

            if gps:
                gps_buf += gps.read(4096)
                *lines, gps_buf = gps_buf.split(b"\n")
                for line in lines:
                    line = line.strip()
                    if line.startswith(b"$"):
                        dev.write(line + b"\r\n")
                        sent += 1
            elif time.monotonic() >= next_send:
                next_send = time.monotonic() + 1.0
                if args.fixed:
                    dev.write(fixed_nmea(args.fixed[0], args.fixed[1], args.alt, args.accuracy / 2.5).encode())
                else:
                    group = next(replay, None)
                    if group is None:
                        print("\nEnd of file.", file=sys.stderr)
                        return
                    dev.write("".join(group).encode())
                sent += 1
            time.sleep(0.02)
    except KeyboardInterrupt:
        print(f"\nStopped after {sent} {'sentences' if gps else 'fixes'}.", file=sys.stderr)


if __name__ == "__main__":
    main()
