#!/usr/bin/env python3
"""Run the initial deterministic Probe bench loop over two USB serial ports.

The operator must enable System > Low Profile on the Cardputer before this
script starts. It intentionally requires explicit ports: automatically picking
an arbitrary second USB CDC device is unsafe in a hardware lab.
"""

import argparse
from datetime import datetime, timezone
import pathlib
import sys
import time

import serial


CARD_MARKER = "@LTRX/1"
TX_MARKER = "@LTTX/1"
CARD_HOME_LONGFAST_KHZ = "906875"
TARGET_CANDIDATE_INDEX = "1"  # LongModerate when Cardputer home is LongFast.


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ 0x1021) & 0xFFFF if value & 0x8000 else (value << 1) & 0xFFFF
    return value


def frame(marker: str, sequence: int, command: str, argument: str) -> bytes:
    body = f"{marker} {sequence} {command} {argument}".encode("ascii")
    return body + f" {crc16(body):04X}\n".encode("ascii")


def parse_frame(line: str, marker: str):
    parts = line.strip().split(" ")
    if len(parts) != 5 or parts[0] != marker:
        return None
    body = " ".join(parts[:-1]).encode("ascii", errors="strict")
    try:
        sequence = int(parts[1])
        supplied_crc = int(parts[4], 16)
    except ValueError:
        return None
    if not 0 <= sequence <= 65535 or crc16(body) != supplied_crc:
        return None
    return sequence, parts[2], parts[3]


def parse_fields(argument: str):
    fields = {}
    for item in argument.split(";"):
        key, sep, value = item.partition("=")
        if not sep or not key:
            raise ValueError(f"malformed status field: {argument!r}")
        fields[key] = value
    return fields


class Endpoint:
    def __init__(self, label: str, port: str, marker: str, log):
        self.label = label
        self.marker = marker
        self.log = log
        self.sequence = 0
        self.port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
        self.port.port = port
        self.port.dtr = False
        self.port.rts = False
        self.port.open()
        self.port.dtr = False
        self.port.rts = False

    def close(self):
        self.port.close()

    def record(self, text: str):
        stamped = f"{datetime.now(timezone.utc).isoformat(timespec='milliseconds')} {self.label} {text}\n"
        self.log.write(stamped)
        self.log.flush()
        print(stamped, end="")

    def request(self, command: str, argument: str, timeout: float = 3.0):
        self.sequence = (self.sequence + 1) & 0xFFFF
        outgoing = frame(self.marker, self.sequence, command, argument)
        self.record("> " + outgoing.decode("ascii").rstrip())
        self.port.write(outgoing)
        self.port.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = self.port.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            self.record("< " + text)
            parsed = parse_frame(text, self.marker)
            if parsed and parsed[0] == self.sequence:
                return parsed[1], parsed[2]
        raise TimeoutError(f"{self.label} did not answer {command} sequence {self.sequence}")


def require_ack(endpoint: Endpoint, command: str, argument: str):
    opcode, payload = endpoint.request(command, argument)
    if opcode != "ACK":
        raise RuntimeError(f"{endpoint.label} {command} failed: {opcode} {payload}")
    return payload


def card_status(card: Endpoint):
    opcode, payload = card.request("STATUS", "-")
    if opcode != "STATUS":
        raise RuntimeError(f"Cardputer STATUS failed: {opcode} {payload}")
    return parse_fields(payload)


def wait_for(card: Endpoint, predicate, timeout: float, description: str):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = card_status(card)
        if predicate(last):
            return last
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {description}; last status={last}")


def run_cycle(card: Endpoint, transmitter: Endpoint, arm_delay_ms: int, cycle: int):
    before = card_status(card)
    if before.get("F") != CARD_HOME_LONGFAST_KHZ:
        raise RuntimeError(
            "this initial matrix requires the built-in LongFast home tuple "
            f"(expected F={CARD_HOME_LONGFAST_KHZ}, got {before.get('F')})"
        )
    if before.get("B") == "RUNNING":
        raise RuntimeError("Cardputer Probe is already running")

    require_ack(card, "PROBE_START", "-")
    wait_for(
        card,
        lambda status: status.get("B") == "RUNNING" and status.get("I") == TARGET_CANDIDATE_INDEX,
        4.0,
        "LongModerate candidate",
    )
    require_ack(transmitter, "ARM", str(arm_delay_ms))
    terminal = wait_for(
        card,
        lambda status: status.get("B") in {"COMPLETE", "CANCELLED", "FAILED"},
        30.0,
        "Probe terminal state",
    )
    if int(terminal.get("R", "0")) <= int(before.get("R", "0")):
        raise RuntimeError(f"cycle {cycle}: home recovery did not advance: {terminal}")
    if terminal.get("F") != CARD_HOME_LONGFAST_KHZ:
        raise RuntimeError(f"cycle {cycle}: Watch did not return to LongFast: {terminal}")
    return terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cardputer-port", required=True)
    parser.add_argument("--heltec-port", required=True)
    parser.add_argument("--log", required=True, help="append-only raw-control capture")
    parser.add_argument("--cycles", type=int, default=1, help="Probe cycles to run (start with 1)")
    parser.add_argument("--arm-delay-ms", type=int, default=100, help="delay from candidate observation to TX")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error("--cycles must be 1..1000")
    if not 10 <= args.arm_delay_ms <= 5000:
        parser.error("--arm-delay-ms must be 10..5000")

    log_path = pathlib.Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log:
        card = Endpoint("cardputer", args.cardputer_port, CARD_MARKER, log)
        transmitter = Endpoint("heltec", args.heltec_port, TX_MARKER, log)
        try:
            require_ack(card, "HELLO", "-")
            require_ack(transmitter, "HELLO", "-")
            require_ack(transmitter, "CONFIG", "LONG_MODERATE")
            for cycle in range(1, args.cycles + 1):
                terminal = run_cycle(card, transmitter, args.arm_delay_ms, cycle)
                print(f"cycle {cycle}/{args.cycles}: {terminal}")
            require_ack(transmitter, "QUIET", "-")
        finally:
            transmitter.close()
            card.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, serial.SerialException, RuntimeError, TimeoutError, ValueError) as error:
        print(f"phase8 bench: {error}", file=sys.stderr)
        sys.exit(2)
