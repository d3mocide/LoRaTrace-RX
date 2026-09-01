"""Shared transport, framing, and device helpers for hardware bench scenarios.

Scenario modules should contain only test intent.  This module owns the
bounded serial transport, CRC validation, retry policy, and capture format so
new Cardputer/bench-node harnesses cannot grow subtly different copies.
"""

from datetime import datetime, timezone
import json
import pathlib
import re
import time

import serial


CARD_MARKER = "@LTRX/1"
TX_MARKER = "@LTTX/1"


class ResultWriter:
    """Append one JSON object per scenario event for machine-readable review."""

    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = self.path.open("a", encoding="utf-8")

    def write(self, event):
        self.stream.write(json.dumps(event, sort_keys=True, separators=(",", ":")) + "\n")
        self.stream.flush()

    def close(self):
        self.stream.close()


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


def parse_frames(line: str, marker: str):
    """Recover valid frames when native USB joins multiple lines together."""
    token = re.compile(
        rf"{re.escape(marker)}\s+\d+\s+\S+\s+\S+\s+[0-9A-Fa-f]{{4}}"
    )
    frames = []
    for match in token.finditer(line):
        parsed = parse_frame(match.group(0), marker)
        if parsed:
            frames.append(parsed)
    return frames


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
        # Native USB Serial/JTAG can pause between endpoint chunks. Keep a
        # short read poll and assemble complete newline-delimited frames in
        # software so a partial chunk never blocks command/retry scheduling.
        # serial_for_url (not plain Serial) so `port` can be a local device
        # path (/dev/ttyACM0) *or* a network URL (socket://host:port,
        # rfc2217://host:port) for a bridge box sitting next to a
        # physically-separated transmitter -- same frame/CRC protocol either
        # way, this is transport only.
        self.port = serial.serial_for_url(port, baudrate=115200, timeout=0.05, do_not_open=True)
        self.port.dtr = False
        self.port.rts = False
        self.port.open()
        self.port.dtr = False
        self.port.rts = False
        self.opened_at = time.monotonic()
        self.observed = []
        self.rx_buffer = bytearray()

    def _poll_lines(self):
        waiting = self.port.in_waiting
        chunk = self.port.read(waiting or 1)
        if chunk:
            self.rx_buffer.extend(chunk)
        lines = []
        while b"\n" in self.rx_buffer:
            raw, _, remainder = self.rx_buffer.partition(b"\n")
            self.rx_buffer = bytearray(remainder)
            lines.append(raw + b"\n")
        return lines

    def close(self):
        self.port.close()

    def record(self, text: str):
        stamped = f"{datetime.now(timezone.utc).isoformat(timespec='milliseconds')} {self.label} {text}\n"
        self.log.write(stamped)
        self.log.flush()
        print(stamped, end="")

    def send(self, command: str, argument: str):
        """Send one command without waiting for its asynchronous response."""
        self.sequence = (self.sequence + 1) & 0xFFFF
        outgoing = frame(self.marker, self.sequence, command, argument)
        self.record("> " + outgoing.decode("ascii").rstrip())
        self.port.write(outgoing)
        self.port.flush()
        return self.sequence

    def request(self, command: str, argument: str, timeout: float = 3.0):
        self.sequence = (self.sequence + 1) & 0xFFFF
        outgoing = frame(self.marker, self.sequence, command, argument)
        deadline = time.monotonic() + timeout
        # Opening native USB-CDC resets an ESP32-S3. The first bytes written
        # during that boot window can be discarded before the application
        # starts polling Serial, so only idempotent commands are retried.
        retry_hello = command == "HELLO"
        retry_safe = self.label == "cardputer" or command in {"HELLO", "STATUS", "CONFIG", "QUIET"}
        next_send = 0.0
        if retry_hello and self.label == "cardputer":
            # Let normal boot diagnostics drain before the first control
            # bytes; bytes sent while setup() initializes can be discarded.
            while time.monotonic() - self.opened_at < 8.5 and time.monotonic() < deadline:
                for raw in self._poll_lines():
                    text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    self.record("< " + text)
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                self.record("> " + outgoing.decode("ascii").rstrip())
                self.port.write(outgoing)
                self.port.flush()
                next_send = now + (1.0 if retry_safe else timeout)
            lines = self._poll_lines()
            if not lines:
                time.sleep(0.005)
                continue
            for raw in lines:
                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                self.record("< " + text)
                parsed_frames = parse_frames(text, self.marker)
                self.observed.extend(parsed_frames)
                parsed = next((item for item in parsed_frames if item[0] == self.sequence), None)
                if parsed:
                    if retry_safe:
                        # Drain duplicate responses left in the USB queue so
                        # the next command cannot mistake one for its reply.
                        settle_deadline = time.monotonic() + 0.25
                        while time.monotonic() < settle_deadline:
                            for stale in self._poll_lines():
                                stale_text = stale.decode("utf-8", errors="replace").rstrip("\r\n")
                                self.record("< " + stale_text)
                            time.sleep(0.005)
                    return parsed[1], parsed[2]
        raise TimeoutError(f"{self.label} did not answer {command} sequence {self.sequence}")


def require_ack(endpoint: Endpoint, command: str, argument: str, timeout: float = 3.0):
    opcode, payload = endpoint.request(command, argument, timeout=timeout)
    if opcode != "ACK":
        raise RuntimeError(f"{endpoint.label} {command} failed: {opcode} {payload}")
    return payload


def card_status(card: Endpoint):
    # STATUS is idempotent. Multiple attempts give the native USB transport a
    # bounded settling window without repeating an action.
    last_error = None
    for _ in range(6):
        try:
            opcode, payload = card.request("STATUS", "-")
            if opcode != "STATUS":
                raise RuntimeError(f"Cardputer STATUS failed: {opcode} {payload}")
            return parse_fields(payload)
        except (TimeoutError, RuntimeError, ValueError) as exc:
            last_error = exc
            time.sleep(0.2)
    raise last_error


def wait_for(card: Endpoint, predicate, timeout: float, description: str):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = card_status(card)
        if predicate(last):
            return last
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {description}; last status={last}")
