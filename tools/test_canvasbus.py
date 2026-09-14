from __future__ import annotations

import pytest
from canvasbus import (
    Capture,
    decode_uart,
    estimate_bit_us,
    group_bursts,
    nearest_standard_baud,
    parse_capture,
    parse_hex_bytes,
)

TICK_HZ = 62_500_000


def synth_capture(
    frames: list[tuple[int, list[int]]],
    baud: int = 1_000_000,
    idle_ticks: int = 5_000,
    inverted: bool = False,
    stop_bits: int = 1,
) -> Capture:
    """Build a capture the way the firmware reports it: durations between level changes."""
    bit = TICK_HZ // baud
    idle = 0 if inverted else 1
    levels = []  # one entry per bit period
    for gap, payload in frames:
        levels.extend([idle] * (gap // bit))
        for value in payload:
            bits = [0] + [(value >> i) & 1 for i in range(8)] + [1] * stop_bits
            levels.extend(level ^ inverted for level in bits)
    levels.extend([idle] * (idle_ticks // bit))

    durations, run = [], bit
    for previous, current in zip(levels, levels[1:]):
        if current == previous:
            run += bit
        else:
            durations.append(run)
            run = bit
    total_us = len(levels) * bit * 1_000_000 // TICK_HZ
    header = {
        "tick_hz": str(TICK_HZ),
        "start_level": str(levels[0]),
        "baud": str(baud),
        "edges": str(len(durations)),
        "start_us": "0",
        "stop_us": str(total_us),
        "tx_us": "0",
        "tx_bytes": "0",
        "truncated": "0",
        "mode": "pp",
        "pull": "none",
    }
    return Capture(header, durations)


def values(decoded) -> list[int]:
    return [byte.value for byte in decoded]


@pytest.mark.parametrize(
    "payload",
    [
        [0xE0, 0x03, 0x05, 0x05, 0xFF, 0x00, 0x00, 0x00],
        [0xC3, 0x00, 0x93, 0x00, 0x40],
        list(range(256)),
    ],
)
def test_decodes_payload_at_1_mbaud(payload):
    decoded = decode_uart(synth_capture([(5_000, payload)]), 1_000_000)
    assert values(decoded) == payload
    assert all(byte.stop_ok for byte in decoded)


def test_decodes_inverted_polarity():
    payload = [0x80, 0x40, 0xC0]
    capture = synth_capture([(5_000, payload)], inverted=True)
    assert values(decode_uart(capture, 1_000_000, inverted=True)) == payload


def test_wrong_baud_flags_stop_bits():
    # 1 Mbaud zeros read at 115200: the stop-bit sample lands inside a later low byte.
    capture = synth_capture([(5_000, [0x00] * 20)])
    assert any(not byte.stop_ok for byte in decode_uart(capture, 115_200))


def test_long_stop_bits_still_decode():
    # The firmware's TX program stretches stop bits to ~1.5 bits.
    payload = [0x12, 0x34, 0x56]
    capture = synth_capture([(5_000, payload)], stop_bits=2)
    assert values(decode_uart(capture, 1_000_000)) == payload


def test_bursts_split_on_idle_gap():
    capture = synth_capture([(5_000, [0xC0]), (2_000, [0x00, 0x00])])
    bursts = group_bursts(capture, decode_uart(capture, 1_000_000), 1_000_000)
    assert [values(burst) for burst in bursts] == [[0xC0], [0x00, 0x00]]


def test_decodes_at_115200():
    payload = [0x01, 0x0A, 0xFF]
    capture = synth_capture([(50_000, payload)], baud=115_200)
    assert values(decode_uart(capture, 115_200)) == payload


def test_estimates_bit_time():
    capture = synth_capture([(5_000, [0x55] * 20)])
    assert nearest_standard_baud(estimate_bit_us(capture)) == 1_000_000


def test_parses_firmware_block():
    lines = [
        (
            "CAP version=0.1.0 pin=2 baud=1000000 mode=pp pull=none start_level=1 tick_hz=62500000 "
            "edges=3 start_us=10 tx_us=20 stop_us=50030 tx_bytes=1 truncated=0"
        ),
        "D 1000 62 125",
        "END",
    ]
    capture = parse_capture(lines)
    assert capture.durations == [1000, 62, 125]
    assert capture.start_level == 1
    assert capture.tx_window_us[0] == 10


def test_rejects_edge_count_mismatch():
    lines = ["CAP start_level=1 tick_hz=62500000 edges=4 baud=1", "D 1 2 3", "END"]
    with pytest.raises(ValueError, match="4 edges but 3"):
        parse_capture(lines)


@pytest.mark.parametrize(
    ("tokens", "expected"),
    [
        (["E003", "0x05", "ff"], [0xE0, 0x03, 0x05, 0xFF]),
        # zsh passes an unquoted "$bytes" as one argument
        (["E0 03 05 05 FF 00 00 00"], [0xE0, 0x03, 0x05, 0x05, 0xFF, 0, 0, 0]),
    ],
)
def test_parses_hex_tokens(tokens, expected):
    assert parse_hex_bytes(tokens) == expected


def test_rejects_odd_hex_digits():
    with pytest.raises(ValueError, match="odd number"):
        parse_hex_bytes(["E0", "3"])
