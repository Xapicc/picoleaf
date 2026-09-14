"""Cycle-level checks of the assembled PIO programs in build/bus.pio.h.

Uses a minimal emulator covering only the instructions bus.pio uses, so the
waveforms can be verified without a Pico on the bench. Build the firmware
first; the tests skip if the generated header is missing.
"""

from __future__ import annotations

import pathlib
import re
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
HEADER = HERE.parent / "build" / "bus.pio.h"
sys.path.insert(0, str(HERE.parent.parent / "tools"))

from canvasbus import Capture, decode_uart

MASK = 0xFFFFFFFF


def load_programs():
    text = HEADER.read_text()
    programs = {}
    for name in ("bus_tx_pp", "bus_tx_od", "edge_capture"):
        body = re.search(rf"{name}_program_instructions\[\] = \{{(.*?)\}};", text, re.DOTALL).group(1)
        programs[name] = {
            "code": [int(value, 16) for value in re.findall(r"0x([0-9a-f]{4})", body)],
            "wrap_target": int(re.search(rf"#define {name}_wrap_target (\d+)", text).group(1)),
            "wrap": int(re.search(rf"#define {name}_wrap (\d+)", text).group(1)),
        }
    return programs


class StateMachine:
    def __init__(
        self, program, sideset_bits=0, sideset_pindirs=False, status_tx_lessthan=1, pin_level=lambda cycle: 1, pins=0
    ):
        self.code = program["code"]
        self.wrap_target = program["wrap_target"]
        self.wrap = program["wrap"]
        self.sideset_bits = sideset_bits  # including the enable bit (all our side-sets are opt)
        self.sideset_pindirs = sideset_pindirs
        self.status_tx_lessthan = status_tx_lessthan
        self.pin_level = pin_level
        self.pc = self.x = self.y = self.isr = self.osr = 0
        self.pins = pins
        self.pindir = 0
        self.delay = 0
        self.tx_fifo = []
        self.rx_fifo = []
        self.cycle = 0

    def step(self):
        cycle = self.cycle
        self.cycle += 1
        if self.delay:
            self.delay -= 1
            return
        instruction = self.code[self.pc]
        field = (instruction >> 8) & 0x1F
        delay_bits = 5 - self.sideset_bits
        delay = field & ((1 << delay_bits) - 1)
        if self.sideset_bits:
            sideset = field >> delay_bits
            if sideset >> (self.sideset_bits - 1):
                value = sideset & 1
                if self.sideset_pindirs:
                    self.pindir = value
                else:
                    self.pins = value

        opcode = instruction >> 13
        argument = instruction & 0xFF
        next_pc = None
        if opcode == 0b000:  # JMP
            condition, target = argument >> 5, argument & 0x1F
            if condition == 0:
                taken = True
            elif condition == 2:
                taken = self.x != 0
                self.x = (self.x - 1) & MASK
            elif condition == 3:
                taken = self.y == 0
            elif condition == 6:
                taken = self.pin_level(cycle) == 1
            else:
                raise NotImplementedError(f"jmp condition {condition}")
            if taken:
                next_pc = target
        elif opcode == 0b011:  # OUT, shifting right
            destination, count = argument >> 5, (argument & 0x1F) or 32
            data = self.osr & ((1 << count) - 1)
            self.osr >>= count
            if destination == 0:
                self.pins = data & 1
            elif destination == 4:
                self.pindir = data & 1
            else:
                raise NotImplementedError(f"out destination {destination}")
        elif opcode == 0b100:
            if argument & 0x80:  # PULL
                if not self.tx_fifo:
                    return  # blocking pull stalls; side-set already applied
                self.osr = self.tx_fifo.pop(0)
            else:  # PUSH
                self.rx_fifo.append(self.isr)
                self.isr = 0
        elif opcode == 0b101:  # MOV
            destination, operation, source = argument >> 5, (argument >> 3) & 3, argument & 7
            value = {1: self.x, 2: self.y, 3: 0, 6: self.isr, 7: self.osr}.get(source)
            if source == 5:
                value = MASK if len(self.tx_fifo) < self.status_tx_lessthan else 0
            if value is None:
                raise NotImplementedError(f"mov source {source}")
            if operation == 1:
                value = ~value & MASK
            if destination == 1:
                self.x = value
            elif destination == 2:
                self.y = value
            elif destination == 6:
                self.isr = value
            else:
                raise NotImplementedError(f"mov destination {destination}")
        elif opcode == 0b111:  # SET
            destination, data = argument >> 5, argument & 0x1F
            if destination == 0:
                self.pins = data & 1
            elif destination == 1:
                self.x = data
            elif destination == 2:
                self.y = data
            elif destination == 4:
                self.pindir = data & 1
            else:
                raise NotImplementedError(f"set destination {destination}")
        else:
            raise NotImplementedError(f"opcode {opcode:03b}")

        if next_pc is None:
            next_pc = self.wrap_target if self.pc == self.wrap else self.pc + 1
        self.pc = next_pc
        self.delay = delay


def levels_to_capture(levels, tick_hz, baud):
    durations, run = [], 1
    for previous, current in zip(levels, levels[1:]):
        if current == previous:
            run += 1
        else:
            durations.append(run)
            run = 1
    header = {
        "tick_hz": str(tick_hz),
        "start_level": str(levels[0]),
        "baud": str(baud),
        "edges": str(len(durations)),
        "start_us": "0",
        "stop_us": "0",
        "tx_us": "0",
        "tx_bytes": "0",
    }
    return Capture(header, durations)


def uart_levels(payload, cycles_per_bit, idle_before, idle_after):
    levels = [1] * idle_before
    for value in payload:
        for bit in [0] + [(value >> i) & 1 for i in range(8)] + [1]:
            levels.extend([bit] * cycles_per_bit)
    return levels + [1] * idle_after


needs_header = pytest.mark.skipif(not HEADER.exists(), reason="build the firmware first (pico/build/bus.pio.h)")

TX_CYCLES_PER_BIT = 8
TX_BAUD = 1_000_000
TX_PAYLOAD = [0xE0, 0x03, 0x05, 0x05, 0xFF, 0x00, 0x00, 0x00]
CAPTURE_CYCLES_PER_BIT = 125  # 1 Mbaud at 125 MHz with clock divider 1


def decoded_values(capture, baud=TX_BAUD):
    return [byte.value for byte in decode_uart(capture, baud)]


@needs_header
def test_push_pull_drives_whole_frame_then_releases():
    sm = StateMachine(load_programs()["bus_tx_pp"], sideset_bits=2, sideset_pindirs=True, pins=1)
    sm.tx_fifo = list(TX_PAYLOAD)
    line, driven = [], []
    for _ in range(len(TX_PAYLOAD) * 12 * TX_CYCLES_PER_BIT + 200):
        sm.step()
        driven.append(sm.pindir)
        line.append(sm.pins if sm.pindir else 1)  # released line idles high

    first = driven.index(1)
    last = len(driven) - 1 - driven[::-1].index(1)
    assert all(driven[first : last + 1]), "driver released mid-frame"
    assert not any(driven[last + 1 :]), "driver not released after frame"

    capture = levels_to_capture(line, TX_CYCLES_PER_BIT * TX_BAUD, TX_BAUD)
    assert decoded_values(capture) == TX_PAYLOAD
    assert all(byte.stop_ok for byte in decode_uart(capture, TX_BAUD))


@needs_header
def test_push_pull_stays_released_until_data_arrives():
    sm = StateMachine(load_programs()["bus_tx_pp"], sideset_bits=2, sideset_pindirs=True, pins=1)
    for _ in range(100):
        sm.step()
        assert sm.pindir == 0
    sm.tx_fifo = [0x80]
    line = []
    for _ in range(200):
        sm.step()
        line.append(sm.pins if sm.pindir else 1)
    capture = levels_to_capture([1, *line], TX_CYCLES_PER_BIT * TX_BAUD, TX_BAUD)
    assert decoded_values(capture) == [0x80]
    assert sm.pindir == 0


@needs_header
def test_open_drain_frame():
    sm = StateMachine(load_programs()["bus_tx_od"], pins=0)
    sm.tx_fifo = [~value & 0xFF for value in TX_PAYLOAD]  # firmware inverts
    line = []
    for _ in range(len(TX_PAYLOAD) * 11 * TX_CYCLES_PER_BIT + 200):
        sm.step()
        assert sm.pins == 0, "open-drain latch must stay low"
        line.append(0 if sm.pindir else 1)
    capture = levels_to_capture(line, TX_CYCLES_PER_BIT * TX_BAUD, TX_BAUD)
    assert decoded_values(capture) == TX_PAYLOAD
    assert sm.pindir == 0


def record_edges(levels):
    sm = StateMachine(load_programs()["edge_capture"], pin_level=lambda cycle: levels[min(cycle, len(levels) - 1)])
    for _ in range(len(levels)):
        sm.step()
    return sm.rx_fifo


def true_runs(levels):
    runs, run = [], 1
    for previous, current in zip(levels, levels[1:]):
        if current == previous:
            run += 1
        else:
            runs.append(run)
            run = 1
    return runs


def durations_like_firmware(words):
    """Mirrors print_capture() in main.c."""
    previous, durations = MASK, []
    for word in words[1:]:
        durations.append((previous - word) & MASK)
        previous = word
    return durations


@needs_header
def test_edge_times_do_not_drift():
    # 100+ edges: any per-edge counting loss would accumulate past the tolerance.
    payload = [0x80, 0x55, 0x00, 0xA5, 0x3C] + [0x55] * 20
    levels = uart_levels(payload, CAPTURE_CYCLES_PER_BIT, idle_before=1_000, idle_after=500)
    words = record_edges(levels)
    assert words[0] == 1, "line starts high"

    runs = true_runs(levels)
    durations = durations_like_firmware(words)
    assert len(durations) == len(runs)
    assert len(runs) > 100

    true_edge, measured_edge, offset = 0, 0, None
    for run, ticks in zip(runs, durations):
        true_edge += run
        measured_edge += 2 * ticks
        if offset is None:
            offset = true_edge - measured_edge  # counter starts a few cycles late
        assert abs(true_edge - measured_edge - offset) <= 2, (true_edge, measured_edge)


@needs_header
def test_capture_decodes_as_uart():
    payload = [0xE0, 0x03, 0x05, 0x05, 0xFF, 0x00, 0x00, 0x00] * 4
    levels = uart_levels(payload, CAPTURE_CYCLES_PER_BIT, idle_before=1_000, idle_after=500)
    words = record_edges(levels)
    header = {
        "tick_hz": "62500000",
        "start_level": str(words[0]),
        "baud": "1000000",
        "edges": "0",
        "start_us": "0",
        "stop_us": "0",
        "tx_us": "0",
        "tx_bytes": "0",
    }
    capture = Capture(header, durations_like_firmware(words))
    assert decoded_values(capture) == payload


@needs_header
def test_starts_low_marker():
    words = record_edges([0] * 300 + [1] * 300 + [0] * 300)
    assert words[0] == 0
    assert len(words) == 3
