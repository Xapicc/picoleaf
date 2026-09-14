"""Parses the Canvas layout reply (the answer to `00` then `80`) and places squares on a grid.

The encoding is inferred from bench readings (docs/panel-bus.md, "Layout encoding"):
- `0xC0 | side`: a square. `side` is its own side facing its parent.
- After a square come its other three sides, clockwise from the entry side (seen from the front),
  each `0x00` for empty or the attached node's subtree.
- `0x90 | n` plus one byte: a power supply.
- `0x40` ends the stream; sides not listed yet are empty.

The drawing uses the root square's own frame, so the real wall may show it rotated in 90° steps.
"""

from __future__ import annotations

from dataclasses import dataclass

# Clockwise as seen from the front, matching the side numbering in square headers.
DOWN, LEFT, UP, RIGHT = range(4)
STEP = {DOWN: (0, -1), LEFT: (-1, 0), UP: (0, 1), RIGHT: (1, 0)}
EMPTY = 0x00
END = 0x40
SQUARE = 0xC0
PSU = 0x90


class LayoutError(ValueError):
    pass


@dataclass(frozen=True)
class Node:
    kind: str  # "square" or "psu"
    header: int
    x: int
    y: int
    index: int | None = None  # squares only: bus order, as used by F8 addressing


@dataclass(frozen=True)
class Layout:
    nodes: list[Node]
    controller_side: int

    @property
    def squares(self) -> list[Node]:
        return [node for node in self.nodes if node.kind == "square"]


class _Reader:
    def __init__(self, data: list[int]):
        self.data = data
        self.position = 0
        self.ended = False

    def next(self) -> int | None:
        """The next byte, or None once the end marker has been read."""
        if self.ended:
            return None
        if self.position >= len(self.data):
            raise LayoutError(f"layout reply ended without {END:02X}: {_hex(self.data)}")
        value = self.data[self.position]
        self.position += 1
        if value == END:
            self.ended = True
            return None
        return value


def parse_layout(reply: list[int]) -> Layout:
    reader = _Reader(reply)
    header = reader.next()
    if header is None or header & 0xF0 != SQUARE:
        raise LayoutError(f"expected a square header first, got {_hex(reply)}")
    controller_side = header & 0x0F
    nodes: list[Node] = []
    _read_square(reader, header, 0, 0, controller_side, nodes)
    if reader.position != len(reply):
        raise LayoutError(f"bytes left after the layout ended: {_hex(reply[reader.position :])}")

    occupied: dict[tuple[int, int], Node] = {}
    for node in nodes:
        other = occupied.setdefault((node.x, node.y), node)
        if other is not node:
            raise LayoutError(f"two nodes placed at ({node.x}, {node.y}): the encoding model is wrong for this layout")
    return Layout(nodes, controller_side)


def _read_square(reader: _Reader, header: int, x: int, y: int, entry_direction: int, nodes: list[Node]) -> None:
    square_count = sum(node.kind == "square" for node in nodes)
    nodes.append(Node("square", header, x, y, index=square_count))
    for turn in (1, 2, 3):
        direction = (entry_direction + turn) % 4
        value = reader.next()
        if value is None or value == EMPTY:
            continue
        dx, dy = STEP[direction]
        if value & 0xF0 == SQUARE:
            # The neighbour is entered from the side facing back towards this square.
            _read_square(reader, value, x + dx, y + dy, (direction + 2) % 4, nodes)
        elif value & 0xF0 == PSU:
            reader.next()  # trailing byte, 00 on every reading so far
            nodes.append(Node("psu", value, x + dx, y + dy))
        else:
            raise LayoutError(f"unknown node byte {value:02X} at offset {reader.position - 1} in {_hex(reader.data)}")


def _turn(x: int, y: int, quarter_turns: int) -> tuple[int, int]:
    """Rotates a grid position clockwise by 90° per quarter turn."""
    for _ in range(quarter_turns % 4):
        x, y = y, -x
    return x, y


def render(layout: Layout, quarter_turns: int = 0) -> str:
    """ASCII grid: [n] squares in bus order, PSU, and the Pico on the root's entry side.

    `quarter_turns` rotates the drawing clockwise, since the root square's frame need not match the wall.
    """
    cells = {
        _turn(node.x, node.y, quarter_turns): f"[{node.index:^3}]" if node.kind == "square" else " PSU "
        for node in layout.nodes
    }
    dx, dy = STEP[layout.controller_side]
    cells[_turn(dx, dy, quarter_turns)] = "Pico "
    xs = [x for x, _ in cells]
    ys = [y for _, y in cells]
    rows = []
    for y in range(max(ys), min(ys) - 1, -1):
        rows.append("".join(cells.get((x, y), "     ") for x in range(min(xs), max(xs) + 1)).rstrip())
    return "\n".join(rows)


def _hex(data: list[int]) -> str:
    return " ".join(f"{value:02X}" for value in data)
