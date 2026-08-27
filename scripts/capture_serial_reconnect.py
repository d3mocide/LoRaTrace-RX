#!/usr/bin/env python3
"""Capture an ESP32 USB-serial console across reset/re-enumeration.

The USB-JTAG serial device disappears briefly when the board resets.  This
reader deliberately avoids PlatformIO's interactive monitor and keeps the
log open while it waits for the device to return.
"""

import argparse
from datetime import datetime, timezone
import os
import pathlib
import sys
import time

import serial
from serial.tools import list_ports


ESPRESSIF_VID = 0x303A
USB_JTAG_PID = 0x1001


def find_port(requested):
    if requested:
        return requested if pathlib.Path(requested).exists() else None

    ports = list(list_ports.comports())
    matching = [
        info.device
        for info in ports
        if info.vid == ESPRESSIF_VID and info.pid == USB_JTAG_PID
    ]
    if matching:
        return sorted(matching)[0]

    acm = [info.device for info in ports if info.device.startswith("/dev/ttyACM")]
    return sorted(acm)[0] if acm else None


def marker(log, message):
    line = "[capture] %s %s\n" % (
        datetime.now(timezone.utc).isoformat(timespec="seconds"),
        message,
    )
    log.write(line.encode("utf-8"))
    log.flush()
    sys.stdout.write(line)
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, help="append-only capture path")
    parser.add_argument("--port", help="optional fixed device path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--echo",
        action="store_true",
        help="also mirror device bytes to stdout (can add host-side backpressure)",
    )
    args = parser.parse_args()

    # A blocking PTY echo can stop the reader long enough for the ESP32's
    # small USB-CDC TX ring to overflow.  Keep the display best-effort so the
    # append-only capture remains authoritative even when the terminal lags.
    echo_fd = None
    if args.echo:
        echo_fd = sys.stdout.fileno()
        os.set_blocking(echo_fd, False)

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    with log_path.open("ab") as log:
        marker(log, "capture started")
        while True:
            port = find_port(args.port)
            if port is None:
                time.sleep(0.25)
                continue

            reader = serial.Serial(port=None, baudrate=args.baud, timeout=0.25)
            reader.port = port
            # Keep DTR/RTS low before opening.  Toggling either line while
            # opening an ESP32 USB-serial port can itself request a reset.
            reader.dtr = False
            reader.rts = False
            try:
                reader.open()
                reader.dtr = False
                reader.rts = False
                marker(log, "connected %s" % port)
                while True:
                    data = reader.read(4096)
                    if not data:
                        if not pathlib.Path(port).exists():
                            raise OSError("serial device disappeared")
                        continue
                    log.write(data)
                    log.flush()
                    if echo_fd is not None:
                        try:
                            os.write(echo_fd, data)
                        except (BlockingIOError, OSError):
                            # Dropping display bytes is acceptable; the log
                            # above has already retained the complete read.
                            pass
            except (OSError, serial.SerialException) as exc:
                marker(log, "disconnected %s (%s)" % (port, exc))
                time.sleep(0.25)
            finally:
                reader.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
