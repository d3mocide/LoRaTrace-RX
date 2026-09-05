#!/usr/bin/env python3
"""Configure the bench transmitter's WiFi control bridge, over USB.

Run this yourself: it prompts for the network password locally and sends it
straight to the board, so the credential never passes through a transcript,
a shell history entry, or a log file. The raw framed exchange is deliberately
not echoed for the WIFI_PASS command.

The bridge exists so the transmitter can sit far enough from the receiver to
give real path loss while a host still drives it. Once it reports an IP,
point any bench harness at it with:

    --heltec-port socket://<ip>:4227

The harness needs no change for that: bench_harness.py opens ports through
pyserial's serial_for_url(), which accepts socket:// URLs.

SECURITY: the bridge has no authentication. Anything that can reach the port
can key the transmitter (bounded, and capped at the firmware's -9 dBm, but
still a transmitter). Enable it on a trusted network only, and run
`--disable` when the fixture is not in use.
"""

import argparse
import getpass
import io
import pathlib
import sys

import serial

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from bench_harness import TX_MARKER, Endpoint, parse_fields, require_ack


def show_status(tx):
    opcode, payload = tx.request("WIFI_STATUS", "-", timeout=6.0)
    if opcode != "STATUS":
        raise RuntimeError(f"WIFI_STATUS failed: {opcode} {payload}")
    fields = parse_fields(payload)
    enabled = fields.get("EN") == "1"
    connected = fields.get("CONN") == "1"
    print(f"  enabled   : {'yes' if enabled else 'no'}")
    print(f"  connected : {'yes' if connected else 'no'}")
    print(f"  ssid      : {fields.get('SSID')}")
    if connected:
        print(f"  reachable : socket://{fields.get('IP')}:{fields.get('PORT')}")
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True,
                        help="the transmitter's USB path (use /dev/serial/by-id/...)")
    parser.add_argument("--ssid", help="prompted for if omitted")
    parser.add_argument("--disable", action="store_true",
                        help="turn the bridge off and forget nothing else")
    parser.add_argument("--status", action="store_true", help="report and exit")
    args = parser.parse_args()

    # Frames are space-delimited, so a credential containing a space cannot be
    # carried. Better to say so up front than to have the board reject it.
    log = io.StringIO()
    tx = Endpoint("heltec", args.port, TX_MARKER, log)
    try:
        identity = require_ack(tx, "HELLO", "-", timeout=25.0)
        print(f"transmitter: {identity}\n")

        if args.status:
            show_status(tx)
            return 0

        if args.disable:
            print(require_ack(tx, "WIFI_OFF", "-", timeout=6.0))
            print("bridge disabled.\n")
            show_status(tx)
            return 0

        ssid = args.ssid or input("SSID: ").strip()
        if not ssid or " " in ssid:
            raise SystemExit("SSID must be non-empty and contain no spaces "
                             "(the frame grammar splits on spaces)")
        password = getpass.getpass("Password (blank for an open network): ")
        if " " in password:
            raise SystemExit("password cannot contain a space "
                             "(the frame grammar splits on spaces)")

        require_ack(tx, "WIFI_SSID", ssid, timeout=6.0)
        require_ack(tx, "WIFI_PASS", password if password else "-", timeout=6.0)
        require_ack(tx, "WIFI_ON", "-", timeout=6.0)
        print("\nenabled; waiting for the join to complete...")

        import time
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            fields = parse_fields(tx.request("WIFI_STATUS", "-", timeout=6.0)[1])
            if fields.get("CONN") == "1":
                print()
                show_status(tx)
                print("\nUse that socket:// URL as --heltec-port. The transmitter can now "
                      "be moved away from the receiver.")
                return 0
            time.sleep(2.0)
        print("\ndid not join within 30s:")
        show_status(tx)
        print("\nCheck the SSID and password, and that the network is 2.4 GHz — "
              "the ESP32-S3 has no 5 GHz radio.")
        return 1
    finally:
        tx.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"heltec wifi setup: {error}", file=sys.stderr)
        sys.exit(2)
