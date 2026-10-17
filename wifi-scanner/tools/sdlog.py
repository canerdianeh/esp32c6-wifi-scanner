#!/usr/bin/env python3
"""
List, download and delete the scanner's microSD log files over USB.

The ESP32-C6 can't present the card as a USB drive, so the firmware answers
simple commands on its USB serial port instead (see main/sdlog.c).

    python tools/sdlog.py COM5 list
    python tools/sdlog.py COM5 get SCAN0003.CSV [output_dir]
    python tools/sdlog.py COM5 get all [output_dir]      # every file not already downloaded
    python tools/sdlog.py COM5 del SCAN0001.CSV

Close any serial monitor on the port first. Needs pyserial (included in the
ESP-IDF Python environment).
"""
import os
import sys
import time
import zlib

import serial

BAUD = 115200   # ignored by USB-Serial-JTAG, but pyserial wants a value


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = 0.2
    # Leave DTR/RTS released so opening the port doesn't reset the board.
    s.dtr = False
    s.rts = False
    s.open()
    return s


def read_line(s, deadline):
    """Read one line (bytes, without line ending), or None on timeout."""
    buf = bytearray()
    while time.monotonic() < deadline:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return bytes(buf).rstrip(b"\r")
        buf += c
    return None


def command(s, cmd, expect, timeout=8.0, retries=3):
    """Send a command and return the first reply line starting with `expect` or '#ERR'.
    Log lines from the scanner are skipped. Retries cover a board that is still booting."""
    for _ in range(retries):
        s.reset_input_buffer()
        s.write(b"\n" + cmd.encode() + b"\n")
        deadline = time.monotonic() + timeout
        while True:
            line = read_line(s, deadline)
            if line is None:
                break
            if line.startswith(expect.encode()) or line.startswith(b"#ERR"):
                return line.decode(errors="replace")
    sys.exit(f"No reply to {cmd!r}. Is the scanner on this port, with no monitor open?")


def check_error(line):
    if line.startswith("#ERR"):
        sys.exit(line[5:])


def list_files(s):
    check_error(command(s, "#LS", "#BEGIN LS"))
    files = []
    deadline = time.monotonic() + 10
    while True:
        line = read_line(s, deadline)
        if line is None:
            sys.exit("Listing timed out")
        line = line.decode(errors="replace")
        if line == "#END LS":
            return files
        if line.startswith("#F "):
            parts = line.split()
            files.append((parts[1], int(parts[2]), len(parts) > 3 and parts[3] == "current"))


def get_file(s, name, out_dir):
    line = command(s, f"#GET {name}", "#DATA")
    check_error(line)
    _, _, size = line.split()
    size = int(size)

    data = bytearray()
    last = time.monotonic()
    while len(data) < size:
        chunk = s.read(size - len(data))
        if chunk:
            data += chunk
            last = time.monotonic()
        elif time.monotonic() - last > 5:
            sys.exit(f"{name}: transfer stalled at {len(data)} of {size} bytes")

    end = read_line(s, time.monotonic() + 5)
    if not end or not end.startswith(b"#END "):
        sys.exit(f"{name}: missing end marker")
    expected = int(end.split()[1], 16)
    actual = zlib.crc32(data) & 0xFFFFFFFF
    if actual != expected:
        sys.exit(f"{name}: checksum mismatch (got {actual:08x}, board sent {expected:08x})")

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name)
    with open(path, "wb") as f:
        f.write(data)
    print(f"{name}: {size} bytes -> {path}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port, action = sys.argv[1], sys.argv[2]
    s = open_port(port)
    time.sleep(0.3)

    if action == "list":
        files = list_files(s)
        if not files:
            print("No files on the card.")
        for name, size, current in files:
            print(f"{name:16} {size:>10} bytes{'   (being written)' if current else ''}")

    elif action == "get" and len(sys.argv) >= 4:
        target = sys.argv[3]
        out_dir = sys.argv[4] if len(sys.argv) > 4 else "logs"
        if target == "all":
            for name, size, current in list_files(s):
                path = os.path.join(out_dir, name)
                # Skip finished files already downloaded; always refresh the one being written.
                if not current and os.path.exists(path) and os.path.getsize(path) == size:
                    print(f"{name}: already downloaded")
                    continue
                get_file(s, name, out_dir)
        else:
            get_file(s, target, out_dir)

    elif action == "del" and len(sys.argv) >= 4:
        line = command(s, f"#DEL {sys.argv[3]}", "#OK DEL")
        check_error(line)
        print(f"Deleted {sys.argv[3]}")

    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
