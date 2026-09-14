from __future__ import annotations

import pytest
from layout import LayoutError, parse_layout, render


def positions(reply: list[int]) -> dict[tuple[int, int], str]:
    layout = parse_layout(reply)
    return {(node.x, node.y): node.kind if node.index is None else f"square{node.index}" for node in layout.nodes}


# Bench readings: square A with the Pico on its right and the PSU on its left.
ONE_SQUARE = [0xC3, 0x00, 0x93, 0x00, 0x40]
B_ON_TOP = [0xC3, 0x00, 0x93, 0x00, 0xC0, 0x00, 0x00, 0x40]
B_ON_TOP_RELINKED = [0xC3, 0x00, 0x93, 0x00, 0xC1, 0x00, 0x00, 0x40]
B_UNDERNEATH = [0xC3, 0xC2, 0x00, 0x00, 0x00, 0x93, 0x00, 0x40]


def test_one_square_with_psu_on_the_left():
    assert positions(ONE_SQUARE) == {(0, 0): "square0", (-1, 0): "psu"}


@pytest.mark.parametrize("reply", [B_ON_TOP, B_ON_TOP_RELINKED])
def test_second_square_on_top(reply):
    assert positions(reply) == {(0, 0): "square0", (-1, 0): "psu", (0, 1): "square1"}


def test_second_square_underneath():
    assert positions(B_UNDERNEATH) == {(0, 0): "square0", (0, -1): "square1", (-1, 0): "psu"}


def test_controller_side_comes_from_root_header():
    assert parse_layout(B_UNDERNEATH).controller_side == 3  # right


def test_render_bench_layout():
    assert render(parse_layout(B_ON_TOP)).split("\n") == [
        "     [ 1 ]",
        " PSU [ 0 ]Pico",
    ]


def test_chain_turning_corners():
    # A → up: B (entered from its bottom) → B's right: C (entered from its left).
    # As on the bench, the end marker takes the place of the final empty side.
    reply = [0xC3, 0x00, 0x00, 0xC0, 0x00, 0x00, 0xC1, 0x00, 0x00, 0x40]
    assert positions(reply) == {(0, 0): "square0", (0, 1): "square1", (1, 1): "square2"}


def test_rejects_unknown_node_byte():
    with pytest.raises(LayoutError, match="unknown node byte 7A"):
        parse_layout([0xC3, 0x7A, 0x40])


def test_rejects_missing_end_marker():
    with pytest.raises(LayoutError, match="without 40"):
        parse_layout([0xC3, 0x00, 0x00])


def test_rejects_overlapping_positions():
    # A → down B → left C → up D → right E lands back on A's position.
    reply = [0xC3, 0xC2, 0x00, 0x00, 0xC1, 0x00, 0x00, 0xC0, 0x00, 0x00, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40]
    with pytest.raises(LayoutError):
        parse_layout(reply)


def test_render_rotated_half_turn():
    assert render(parse_layout(B_ON_TOP), quarter_turns=2).split("\n") == [
        "Pico [ 0 ] PSU",
        "     [ 1 ]",
    ]
