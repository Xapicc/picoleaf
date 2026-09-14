"""The firmware's layout parser (src/layout.c) must agree with tools/layout.py."""

from __future__ import annotations

import ctypes
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent.parent / "tools"))

from layout import LayoutError, parse_layout

WALL = bytes.fromhex(
    "C1 00 00 C3 C3 C2 00 C3 00 00 93 00 00 C3 C2 00 00 00 00 C0 00 C1 00 00 C2 00 00 C2 00 00 00 00 00 C2 00 C2 00 00 00 00 40"
)
READINGS = {
    "one square": bytes([0xC3, 0x00, 0x93, 0x00, 0x40]),
    "square on top": bytes([0xC3, 0x00, 0x93, 0x00, 0xC0, 0x00, 0x00, 0x40]),
    "square underneath": bytes([0xC3, 0xC2, 0x00, 0x00, 0x00, 0x93, 0x00, 0x40]),
    "13-square wall": WALL,
}


class Position(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int8), ("y", ctypes.c_int8)]


@pytest.fixture(scope="module")
def layout_positions(build_host_library):
    function = build_host_library("layout.c").layout_positions
    function.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Position), ctypes.c_size_t]
    function.restype = ctypes.c_int

    def run(reply: bytes, max_squares: int = 64) -> list[tuple[int, int]] | None:
        positions = (Position * max_squares)()
        count = function(reply, len(reply), positions, max_squares)
        return None if count < 0 else [(positions[i].x, positions[i].y) for i in range(count)]

    return run


@pytest.mark.parametrize("name", READINGS)
def test_matches_python_parser(layout_positions, name):
    reply = READINGS[name]
    expected = [(node.x, node.y) for node in parse_layout(list(reply)).squares]
    assert layout_positions(reply) == expected


def test_wall_shape(layout_positions):
    positions = layout_positions(WALL)
    assert len(positions) == 13
    assert positions[0] == (0, 0)
    assert len(set(positions)) == 13


@pytest.mark.parametrize(
    "reply",
    [
        bytes([0xC3, 0x7A, 0x40]),  # unknown node byte
        bytes([0xC3, 0x00, 0x00]),  # no end marker
        bytes([0x93, 0x00, 0x40]),  # doesn't start with a square
        bytes([0xC3, 0x00, 0x00, 0x00, 0x40]),  # leftover end marker
        # A → down B → left C → up D → right E lands on A.
        bytes([0xC3, 0xC2, 0x00, 0x00, 0xC1, 0x00, 0x00, 0xC0, 0x00, 0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40]),
    ],
)
def test_rejects_what_python_rejects(layout_positions, reply):
    assert layout_positions(reply) is None
    with pytest.raises(LayoutError):
        parse_layout(list(reply))


def test_rejects_more_squares_than_room(layout_positions):
    assert layout_positions(WALL, max_squares=12) is None
