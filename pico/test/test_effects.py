"""Wall effects (src/effects.c) through ctypes."""

from __future__ import annotations

import ctypes

import pytest
from test_ha_light import LightState
from test_layout_c import WALL, Position

EFFECTS = ["Solid", "Colour cycle", "Breathe", "Twinkle", "Rainbow wave", "Colour wave", "Ripple", "Fire"]
SECOND = 1_000_000
FRAME = 40_000


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


class Memory(ctypes.Structure):
    _fields_ = [
        ("random", ctypes.c_uint32),
        ("last_frame_us", ctypes.c_uint64),
        ("sparkle", ctypes.c_float * 64),
        ("heat", ctypes.c_float * 64),
    ]


Rgbw = ctypes.c_uint8 * 4


@pytest.fixture(scope="module")
def lib(build_host_library):
    library = build_host_library("effects.c", "layout.c")
    library.effect_name.argtypes = [ctypes.c_int]
    library.effect_name.restype = ctypes.c_char_p
    library.effect_from_name.argtypes = [ctypes.c_char_p]
    library.effect_from_name.restype = ctypes.c_int
    library.layout_positions.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Position), ctypes.c_size_t]
    library.effect_points_from_layout.argtypes = [
        ctypes.POINTER(Position),
        ctypes.c_size_t,
        ctypes.c_uint,
        ctypes.POINTER(Point),
    ]
    library.effect_memory_init.argtypes = [ctypes.POINTER(Memory), ctypes.c_uint32]
    library.effect_render.argtypes = [
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.POINTER(Point),
        ctypes.c_size_t,
        ctypes.POINTER(LightState),
        ctypes.POINTER(Memory),
        ctypes.POINTER(Rgbw),
    ]
    return library


def wall_points(lib, quarter_turns: int = 2):
    positions = (Position * 64)()
    count = lib.layout_positions(WALL, len(WALL), positions, 64)
    points = (Point * 64)()
    lib.effect_points_from_layout(positions, count, quarter_turns, points)
    return points, count


class Renderer:
    def __init__(self, lib, effect: str, base: LightState | None = None, seed: int = 1):
        self.lib = lib
        self.effect = EFFECTS.index(effect)
        self.base = base or LightState(True, 255, 255, 120, 0)
        self.points, self.count = wall_points(lib)
        self.memory = Memory()
        lib.effect_memory_init(ctypes.byref(self.memory), seed)

    def frame(self, now_us: int) -> list[tuple[int, int, int, int]]:
        output = (Rgbw * self.count)()
        self.lib.effect_render(
            self.effect, now_us, self.points, self.count, ctypes.byref(self.base), ctypes.byref(self.memory), output
        )
        return [tuple(square) for square in output]


def brightness(colour) -> int:
    return sum(colour[:3])


def test_names_round_trip(lib):
    for index, name in enumerate(EFFECTS):
        assert lib.effect_name(index).decode() == name
        assert lib.effect_from_name(name.encode()) == index
    assert lib.effect_from_name(b"Disco") == len(EFFECTS)


def test_points_follow_the_wall_as_it_hangs(lib):
    # With the drawing turned 180° (as confirmed on the wall), tile 4 is the bottom row,
    # tile 8 the top-left corner and tile 0 sits at the right edge.
    points, count = wall_points(lib, quarter_turns=2)
    assert count == 13
    assert (points[4].y, points[8].x, points[8].y, points[0].x) == (0.0, 0.0, 1.0, 1.0)
    assert all(0.0 <= points[i].x <= 1.0 and 0.0 <= points[i].y <= 1.0 for i in range(count))


@pytest.mark.parametrize("effect", EFFECTS)
def test_every_effect_is_dark_when_off(lib, effect):
    renderer = Renderer(lib, effect, LightState(False, 255, 255, 255, 255))
    for step in range(20):
        assert all(colour == (0, 0, 0, 0) for colour in renderer.frame(SECOND + step * FRAME))


def test_solid_shows_base_colour_scaled_by_brightness(lib):
    frame = Renderer(lib, "Solid", LightState(True, 128, 255, 0, 200)).frame(SECOND)
    assert set(frame) == {(128, 0, 100, 0)}


def test_colour_cycle_is_uniform_and_changes_over_time(lib):
    renderer = Renderer(lib, "Colour cycle")
    first, later = renderer.frame(SECOND), renderer.frame(6 * SECOND)
    assert len(set(first)) == 1 and len(set(later)) == 1
    assert first[0] != later[0]


def test_breathe_stays_within_base_colour_and_varies(lib):
    renderer = Renderer(lib, "Breathe", LightState(True, 255, 200, 100, 0))
    levels = [renderer.frame(step * FRAME)[0] for step in range(100)]
    assert all(red <= 200 and green <= 100 for red, green, _, _ in levels)
    assert max(map(brightness, levels)) - min(map(brightness, levels)) > 150


def test_rainbow_wave_depends_only_on_horizontal_position(lib):
    renderer = Renderer(lib, "Rainbow wave")
    frame = renderer.frame(3 * SECOND)
    by_x: dict[float, set] = {}
    for index in range(renderer.count):
        by_x.setdefault(renderer.points[index].x, set()).add(frame[index])
    assert all(len(colours) == 1 for colours in by_x.values())
    assert len({colours.pop() for colours in by_x.values()}) == len(by_x)


def test_colour_wave_travels_upwards(lib):
    # The wall has five rows (y = 0, 0.25, ... 1). With a 3 s period, the row a quarter higher
    # must show exactly what the bottom row showed 0.75 s earlier if crests move upwards.
    renderer = Renderer(lib, "Colour wave")
    bottom = next(i for i in range(renderer.count) if renderer.points[i].y == 0.0)
    second_row = next(i for i in range(renderer.count) if renderer.points[i].y == 0.25)
    for start in (SECOND, 1_300_000, 2_100_000):
        earlier_bottom = renderer.frame(start)[bottom]
        later_second_row = renderer.frame(start + 750_000)[second_row]
        assert all(abs(a - b) <= 1 for a, b in zip(earlier_bottom, later_second_row))
    # A downward wave would match the other way round; make sure that does not hold.
    assert renderer.frame(SECOND)[second_row] != renderer.frame(SECOND + 750_000)[bottom]


def test_ripple_is_symmetric_around_the_centre(lib):
    renderer = Renderer(lib, "Ripple")
    frame = renderer.frame(SECOND)
    by_distance: dict[float, set] = {}
    for index in range(renderer.count):
        point = renderer.points[index]
        distance = round(((point.x - 0.5) ** 2 + (point.y - 0.5) ** 2) ** 0.5, 4)
        by_distance.setdefault(distance, set()).add(frame[index])
    assert all(len(colours) == 1 for colours in by_distance.values())


def test_fire_burns_hotter_at_the_bottom(lib):
    renderer = Renderer(lib, "Fire", seed=7)
    bottom = [i for i in range(renderer.count) if renderer.points[i].y == 0.0]
    top = [i for i in range(renderer.count) if renderer.points[i].y == 1.0]
    totals = {"bottom": 0, "top": 0}
    for step in range(250):  # 10 s
        frame = renderer.frame(SECOND + step * FRAME)
        totals["bottom"] += sum(brightness(frame[i]) for i in bottom) / len(bottom)
        totals["top"] += sum(brightness(frame[i]) for i in top) / len(top)
    assert totals["bottom"] > 1.5 * totals["top"]


def test_twinkle_sparkles_above_the_background(lib):
    renderer = Renderer(lib, "Twinkle", LightState(True, 255, 0, 0, 255), seed=3)
    background = renderer.frame(SECOND)[0]
    brightest = max(max(renderer.frame(SECOND + step * FRAME), key=brightness) for step in range(1, 250))
    assert brightness(background) <= 80
    assert brightness(brightest) > 3 * brightness(background)
