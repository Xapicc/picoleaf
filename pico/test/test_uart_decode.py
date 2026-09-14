"""Runs the firmware's C UART decoder (src/uart_decode.c) on the host through ctypes."""

from __future__ import annotations

import ctypes

import pytest

TICK_HZ = 62_500_000  # 125 MHz system clock, 2 PIO cycles per count
MASK = 0xFFFFFFFF


@pytest.fixture(scope="module")
def decoder(build_host_library):
    decode = build_host_library("uart_decode.c").uart_decode
    decode.restype = ctypes.c_size_t
    decode.argtypes = [
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_bool),
        ctypes.c_size_t,
    ]

    def run(words: list[int], baud: int = 1_000_000, max_bytes: int = 64) -> list[tuple[int, bool]]:
        word_array = (ctypes.c_uint32 * len(words))(*words)
        values = (ctypes.c_uint8 * max_bytes)()
        stop_ok = (ctypes.c_bool * max_bytes)()
        count = decode(word_array, len(words), TICK_HZ, baud, values, stop_ok, max_bytes)
        return [(values[i], stop_ok[i]) for i in range(count)]

    return run


def recorder_words(frames: list[tuple[float, list[int]]], baud: int = 1_000_000, stop_bits: float = 1.0) -> list[int]:
    """Words as the PIO edge recorder pushes them: start level, then a countdown timestamp per edge.

    Frames are (start time in µs, bytes); a byte is start bit, 8 data bits LSB first, stop bit(s).
    """
    bit_ticks = TICK_HZ / baud
    edges = []
    level = 1
    for start_us, payload in frames:
        tick = start_us * TICK_HZ / 1e6
        for value in payload:
            for bit in [0] + [(value >> i) & 1 for i in range(8)]:
                if bit != level:
                    edges.append(round(tick))
                    level = bit
                tick += bit_ticks
            if level != 1:
                edges.append(round(tick))
                level = 1
            tick += bit_ticks * stop_bits
    return [1] + [MASK - edge for edge in edges]


def test_decodes_session_exchange(decoder):
    # Our 80 at 10 µs, then the square's layout reply with the gaps measured on the bench.
    frames = [(10.0, [0x80]), (44.5, [0xC3]), (108.5, [0x00]), (137.5, [0x93]), (176.5, [0x00]), (226.5, [0x40])]
    assert decoder(recorder_words(frames)) == [(value, True) for _, (value,) in frames]


@pytest.mark.parametrize("payload", [list(range(256)), [0xE0, 0x01, 0x05, 0x00, 0xFF, 0x00, 0x00, 0x00]])
def test_round_trips_back_to_back_bytes(decoder, payload):
    result = decoder(recorder_words([(10.0, payload)], stop_bits=1.5), max_bytes=300)
    assert [value for value, _ in result] == payload
    assert all(stop_ok for _, stop_ok in result)


def test_flags_wrong_baud(decoder):
    words = recorder_words([(10.0, [0x00] * 20)])
    assert any(not stop_ok for _, stop_ok in decoder(words, baud=115_200))


def test_respects_output_limit(decoder):
    assert len(decoder(recorder_words([(10.0, [0x11] * 10)]), max_bytes=4)) == 4


def test_no_edges_decodes_nothing(decoder):
    assert decoder([1]) == []
