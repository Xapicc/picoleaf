#!/usr/bin/env python3
"""Host side of the Canvas bus probe (pico/ firmware).

Talks to the Pico over USB serial, saves every capture under captures/ as JSON
plus a VCD for PulseView, and decodes the recorded edges as 8N1 UART.
"""

from __future__ import annotations

import argparse
import bisect
import datetime
import glob
import json
import pathlib
import statistics
import sys
import time
from dataclasses import dataclass

from layout import parse_layout, render

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CAPTURE_DIR = REPO_ROOT / "captures"

STANDARD_BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 500000, 921600, 1000000, 2000000, 3000000]

# Shapes uses FE for panel firmware update; never send it by accident (docs/scope.md).
DANGEROUS_FIRST_BYTES = {0xFE: "panel firmware update on Shapes"}


@dataclass
class Capture:
    header: dict[str, str]
    durations: list[int]

    @property
    def tick_hz(self) -> int:
        return int(self.header["tick_hz"])

    @property
    def start_level(self) -> int:
        return int(self.header["start_level"])

    @property
    def baud(self) -> int:
        return int(self.header["baud"])

    def ticks_to_us(self, ticks: float) -> float:
        return ticks * 1e6 / self.tick_hz

    def us_to_ticks(self, us: float) -> float:
        return us * self.tick_hz / 1e6

    def edge_times(self) -> list[int]:
        """Tick time of each level change, measured from capture start."""
        times, now = [], 0
        for duration in self.durations:
            now += duration
            times.append(now)
        return times

    def level_at(self, edge_times: list[int], tick: float) -> int:
        changes = bisect.bisect_right(edge_times, tick)
        return self.start_level ^ (changes & 1)

    @property
    def tx_window_us(self) -> tuple[float, float] | None:
        tx_bytes = int(self.header.get("tx_bytes", 0))
        if tx_bytes == 0:
            return None
        start = int(self.header["tx_us"]) - int(self.header["start_us"])
        # Matches TX_BITS_PER_BYTE_BOUND in the firmware.
        return start, start + tx_bytes * 12 * 1e6 / self.baud


@dataclass
class UartByte:
    tick: int
    value: int
    stop_ok: bool


def parse_capture(lines: list[str]) -> Capture:
    if not lines or not lines[0].startswith("CAP "):
        raise ValueError(f"expected a CAP header line, got {lines[:1]!r}")
    header = dict(field.split("=", 1) for field in lines[0].split()[1:])
    durations: list[int] = []
    for line in lines[1:]:
        if line == "END":
            break
        if not line.startswith("D "):
            raise ValueError(f"unexpected line inside capture: {line!r}")
        durations.extend(int(value) for value in line[2:].split())
    expected = int(header["edges"])
    if len(durations) != expected:
        raise ValueError(f"capture header says {expected} edges but {len(durations)} values arrived")
    return Capture(header, durations)


def estimate_bit_us(capture: Capture) -> float | None:
    """Shortest pulse cluster, ignoring the open first segment."""
    pulses = sorted(capture.durations[1:])
    if not pulses:
        return None
    shortest = pulses[: max(1, len(pulses) // 10)]
    return capture.ticks_to_us(statistics.median(shortest))


def nearest_standard_baud(bit_us: float) -> int:
    return min(STANDARD_BAUDS, key=lambda baud: abs(1e6 / baud - bit_us))


def decode_uart(capture: Capture, baud: int, inverted: bool = False) -> list[UartByte]:
    """8N1, LSB first. Idle is high unless inverted."""
    edge_times = capture.edge_times()
    bit = capture.tick_hz / baud
    idle = 0 if inverted else 1

    def level(tick: float) -> int:
        return capture.level_at(edge_times, tick)

    decoded: list[UartByte] = []
    earliest_start = 0.0
    for edge in edge_times:
        if edge < earliest_start or level(edge) == idle:
            continue
        if level(edge + 0.5 * bit) == idle:
            continue  # glitch shorter than half a bit
        value = 0
        for index in range(8):
            if level(edge + (1.5 + index) * bit) != idle:
                continue
            value |= 1 << index
        stop_ok = level(edge + 9.5 * bit) == idle
        decoded.append(UartByte(edge, value, stop_ok))
        earliest_start = edge + 9.5 * bit
    return decoded


def group_bursts(capture: Capture, decoded: list[UartByte], baud: int, gap_bits: float = 30) -> list[list[UartByte]]:
    bit = capture.tick_hz / baud
    bursts: list[list[UartByte]] = []
    for byte in decoded:
        if bursts and byte.tick - bursts[-1][-1].tick <= (10 + gap_bits) * bit:
            bursts[-1].append(byte)
        else:
            bursts.append([byte])
    return bursts


def write_vcd(capture: Capture, path: pathlib.Path) -> None:
    lines = [
        "$timescale 1ns $end",
        "$scope module canvas $end",
        "$var wire 1 ! bus $end",
        "$upscope $end",
        "$enddefinitions $end",
        "#0",
        f"{capture.start_level}!",
    ]
    level = capture.start_level
    for tick in capture.edge_times():
        level ^= 1
        lines.append(f"#{round(tick * 1e9 / capture.tick_hz)}")
        lines.append(f"{level}!")
    total_us = int(capture.header["stop_us"]) - int(capture.header["start_us"])
    lines.append(f"#{total_us * 1000}")
    path.write_text("\n".join(lines) + "\n")


def describe(capture: Capture, baud: int | None, inverted: bool) -> str:
    total_ms = (int(capture.header["stop_us"]) - int(capture.header["start_us"])) / 1000
    out = [
        (
            f"edges={len(capture.durations)}  duration={total_ms:.1f} ms  "
            f"start_level={'high' if capture.start_level else 'low'}  "
            f"truncated={'YES' if capture.header['truncated'] == '1' else 'no'}  "
            f"mode={capture.header['mode']} pull={capture.header['pull']}"
        )
    ]
    if not capture.durations:
        out.append("line never changed level")
        return "\n".join(out)

    bit_us = estimate_bit_us(capture)
    if bit_us:
        out.append(f"shortest pulses ≈ {bit_us:.3f} µs → nearest standard baud {nearest_standard_baud(bit_us)}")

    if len(capture.durations) <= 60:
        level = capture.start_level
        parts = []
        for duration in capture.durations:
            parts.append(f"{'H' if level else 'L'}{capture.ticks_to_us(duration):.1f}")
            level ^= 1
        out.append("levels (µs): " + " ".join(parts))

    decode_baud = baud or capture.baud
    decoded = decode_uart(capture, decode_baud, inverted)
    out.append(f"UART 8N1 @ {decode_baud}{' inverted' if inverted else ''}: {len(decoded)} bytes")
    # A panel can answer within one byte time of our stop bit, so a time window
    # can't separate directions. The first tx_bytes decoded bytes are ours.
    tx_bytes = int(capture.header.get("tx_bytes", 0))
    ours = decoded[:tx_bytes] if baud in (None, capture.baud) else []
    theirs = decoded[len(ours) :]
    for direction, group in (("tx", ours), ("rx", theirs)):
        for burst in group_bursts(capture, group, decode_baud):
            start_us = capture.ticks_to_us(burst[0].tick)
            text = " ".join(f"{b.value:02X}{'' if b.stop_ok else '!'}" for b in burst)
            out.append(f"  {start_us:12.1f} µs  {direction}  {text}")
    if any(not b.stop_ok for b in decoded):
        out.append("  (! = stop bit not idle: wrong baud, polarity, or not UART)")
    return "\n".join(out)


def save_capture(capture: Capture, command: str, label: str | None) -> pathlib.Path:
    CAPTURE_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    name = f"{stamp}-{label}" if label else stamp
    path = CAPTURE_DIR / f"{name}.json"
    path.write_text(
        json.dumps(
            {
                "command": command,
                "label": label,
                "created": stamp,
                "header": capture.header,
                "durations": capture.durations,
            },
            indent=1,
        )
    )
    write_vcd(capture, path.with_suffix(".vcd"))
    return path


def load_capture(path: pathlib.Path) -> Capture:
    data = json.loads(path.read_text())
    return Capture(data["header"], data["durations"])


class Probe:
    def __init__(self, port: str | None):
        import serial  # imported here so decode/tests work without pyserial

        self.serial = serial.Serial(port or self.find_port(), timeout=40)

    @staticmethod
    def find_port() -> str:
        ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
        if len(ports) != 1:
            sys.exit(f"found {len(ports)} candidate serial ports {ports}; pass --port")
        return ports[0]

    def command(self, line: str) -> tuple[list[str], list[Capture]]:
        self.serial.reset_input_buffer()
        self.serial.write((line + "\n").encode())
        output: list[str] = []
        captures: list[Capture] = []
        block: list[str] | None = None
        while True:
            raw = self.serial.readline()
            if not raw:
                raise TimeoutError(f"no response to {line!r}")
            text = raw.decode(errors="replace").strip()
            if block is not None:
                block.append(text)
                if text == "END":
                    captures.append(parse_capture(block))
                    block = None
            elif text.startswith("CAP "):
                block = [text]
            elif text == "OK":
                return output, captures
            elif text.startswith("ERR"):
                raise RuntimeError(f"{line!r}: {text}")
            elif text:
                output.append(text)


def parse_hex_bytes(tokens: list[str]) -> list[int]:
    # Tokens may themselves contain spaces (a quoted "E0 03 05"), so re-split.
    words = " ".join(tokens).split()
    joined = "".join(word[2:] if word.lower().startswith("0x") else word for word in words)
    if len(joined) % 2 != 0:
        raise ValueError(f"odd number of hex digits in {' '.join(tokens)!r}")
    return list(bytes.fromhex(joined))


def tx_line(listen_ms: int, payload: list[int], allow_dangerous: bool) -> str:
    reason = DANGEROUS_FIRST_BYTES.get(payload[0])
    if reason and not allow_dangerous:
        sys.exit(f"refusing to send first byte {payload[0]:02X} ({reason}); pass --allow-dangerous")
    return f"tx {listen_ms} " + " ".join(f"{b:02X}" for b in payload)


def split_directions(capture: Capture) -> tuple[list[UartByte], list[UartByte]]:
    """The first tx_bytes decoded bytes are ours, everything after is the panel's reply."""
    decoded = decode_uart(capture, capture.baud)
    tx_bytes = int(capture.header["tx_bytes"])
    return decoded[:tx_bytes], decoded[tx_bytes:]


def run_sequence(probe: Probe, steps: list[tuple[str, object]], label: str | None) -> None:
    """Runs frames and waits back to back and prints one line per frame: what went out, what came back."""
    started = time.monotonic()
    for index, (kind, value) in enumerate(steps):
        if kind == "wait":
            time.sleep(value / 1000)
            continue
        sent_at_ms = (time.monotonic() - started) * 1000
        _, captures = probe.command(value)
        sent, reply = split_directions(captures[0])
        save_capture(captures[0], value, f"{label}-{index:02d}" if label else None)
        sent_text = " ".join(f"{b.value:02X}" for b in sent)
        reply_text = " ".join(f"{b.value:02X}{'' if b.stop_ok else '!'}" for b in reply)
        print(f"{sent_at_ms:9.0f} ms  tx {sent_text:<24}  rx {reply_text or '-'}")


def read_layout(probe: Probe, rotate_degrees: int) -> None:
    """Opens a session, reads and draws the layout, and cross-checks the square count against a poll."""

    def reply_to(payload: list[int], listen_ms: int) -> list[int]:
        _, captures = probe.command(tx_line(listen_ms, payload, allow_dangerous=False))
        save_capture(captures[0], f"layout {payload[0]:02X}", "layout-read")
        return [byte.value for byte in split_directions(captures[0])[1]]

    reply_to([0x00], 20)
    # Relayed replies take tens of µs per byte; 150 ms covers large layouts and keeps the
    # following poll inside the ~480 ms session timeout.
    raw = reply_to([0x80], 150)
    poll = reply_to([0xC0], 50)
    print("layout reply:", " ".join(f"{value:02X}" for value in raw) or "nothing")
    print("poll reply:  ", " ".join(f"{value:02X}" for value in poll) or "nothing")
    parsed = parse_layout(raw)
    psu_count = len(parsed.nodes) - len(parsed.squares)
    print(f"{len(parsed.squares)} squares, {psu_count} PSUs, drawing rotated {rotate_degrees}° clockwise\n")
    print(render(parsed, rotate_degrees // 90))
    if len(poll) != 2 * len(parsed.squares):
        print(f"\nwarning: poll returned {len(poll)} bytes, expected {2 * len(parsed.squares)} for this many squares")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port (default: the only /dev/cu.usbmodem*)")
    commands = parser.add_subparsers(dest="action", required=True)

    commands.add_parser("info", help="firmware settings and current line level")
    send = commands.add_parser("send", help="raw firmware command, e.g. 'baud 115200', 'mode od', 'pull up'")
    send.add_argument("line", nargs="+")

    for name, helptext in (("cap", "record the line passively"), ("tx", "transmit bytes, then record")):
        sub = commands.add_parser(name, help=helptext)
        sub.add_argument("ms", type=int, help="how long to record (after transmitting, for tx)")
        if name == "tx":
            sub.add_argument("hex", nargs="+", help="bytes, e.g. 80 or E0 03 05")
            sub.add_argument("--allow-dangerous", action="store_true")
        sub.add_argument("--label", help="suffix for the saved capture file")
        sub.add_argument("--baud", type=int, help="decode at this baud instead of the TX baud")
        sub.add_argument("--inverted", action="store_true", help="decode with idle-low polarity")

    layout_parser = commands.add_parser("layout", help="read the panel layout and draw it")
    layout_parser.add_argument(
        "--rotate", type=int, choices=(0, 90, 180, 270), default=0, help="turn the drawing clockwise to match the wall"
    )

    seq = commands.add_parser("seq", help="several frames over one connection, e.g. '00' '80' wait=500 'C0'")
    seq.add_argument("steps", nargs="+", help="hex frame (quote multi-byte frames) or wait=<ms>")
    seq.add_argument("--listen", type=int, default=20, help="ms to record after each frame")
    seq.add_argument("--label", help="prefix for the saved capture files")
    seq.add_argument("--allow-dangerous", action="store_true")

    decode = commands.add_parser("decode", help="decode a saved capture")
    decode.add_argument("file", type=pathlib.Path)
    decode.add_argument("--baud", type=int)
    decode.add_argument("--inverted", action="store_true")

    args = parser.parse_args()

    if args.action == "decode":
        print(describe(load_capture(args.file), args.baud, args.inverted))
        return

    line = f"cap {getattr(args, 'ms', 0)}"
    if args.action == "tx":
        line = tx_line(args.ms, parse_hex_bytes(args.hex), args.allow_dangerous)

    if args.action == "layout":
        read_layout(Probe(args.port), args.rotate)
        return

    if args.action == "seq":
        steps = []
        for step in args.steps:
            if step.startswith("wait="):
                steps.append(("wait", int(step[5:])))
            else:
                steps.append(("tx", tx_line(args.listen, parse_hex_bytes([step]), args.allow_dangerous)))
        run_sequence(Probe(args.port), steps, args.label)
        return

    probe = Probe(args.port)
    if args.action == "info":
        print("\n".join(probe.command("info")[0]))
    elif args.action == "send":
        output, _ = probe.command(" ".join(args.line))
        print("\n".join(output) or "OK")
    else:
        _, captures = probe.command(line)
        for capture in captures:
            path = save_capture(capture, line, args.label)
            print(describe(capture, args.baud, args.inverted))
            print(f"saved {path.relative_to(REPO_ROOT)} (+ .vcd)")


if __name__ == "__main__":
    main()
