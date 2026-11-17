#!/usr/bin/env python3
"""Capture the board's serial log to a file for a fixed time (non-interactive).

idf.py monitor is interactive and never exits, so it is unusable from an agent.
Run this from a shell where getmatter/getidf is loaded (pyserial comes with ESP-IDF).

    python firmware/tools/serial_capture.py --seconds 60 --reset --out /tmp/boot.log
    python firmware/tools/serial_capture.py --seconds 120 --out pair.log   # no reset, e.g. during pairing

--reset pulses RTS the same way esptool's hard reset does, so the log starts at boot.
Close any open idf.py monitor first: only one program can hold the port.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="/dev/cu.usbmodem1101")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--reset", action="store_true", help="reset the chip before capturing")
    p.add_argument("--out", default="serial_capture.log")
    p.add_argument("--quiet", action="store_true", help="don't echo to stdout")
    a = p.parse_args()

    ser = serial.Serial(a.port, a.baud, timeout=0.2)
    if a.reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False

    end = time.monotonic() + a.seconds
    with open(a.out, "wb") as f:
        while time.monotonic() < end:
            chunk = ser.read(4096)
            if chunk:
                f.write(chunk)
                f.flush()
                if not a.quiet:
                    sys.stdout.write(chunk.decode("utf-8", "replace"))
                    sys.stdout.flush()
    ser.close()
    print(f"\n[serial_capture] wrote {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
